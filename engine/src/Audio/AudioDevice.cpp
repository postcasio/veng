#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Log.h>

#include <Veng/Audio/Reverb.h>
#include <Veng/Audio/TripleBuffer.h>

#include "AudioFrame.h"
#include "BufferedGenerator.h"
#include "SpscRing.h"
#include "StreamVoice.h"

#include <miniaudio.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <span>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <xmmintrin.h>
#endif

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace Veng::Audio
{
    namespace
    {
        // Flushes subnormal (denormal) floats to zero for its lifetime, restoring the prior FPU mode on
        // exit. Reverb feedback and filter states decay toward zero and reach subnormals, whose
        // arithmetic is far slower on both common targets — on the real-time mixing thread that surfaces
        // as buffer underruns. Scoping it (rather than setting the mode once) is what makes the
        // null-device pump safe: that path mixes on the main thread, which must not be left in
        // flush-to-zero for the rest of the frame's math.
        class ScopedFlushDenormals
        {
        public:
            ScopedFlushDenormals()
            {
#if defined(__aarch64__) || defined(_M_ARM64)
                __asm__ __volatile__("mrs %0, fpcr" : "=r"(m_Saved));
                __asm__ __volatile__("msr fpcr, %0" : : "r"(m_Saved | (u64{1} << 24))); // FZ
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
                m_Saved = _mm_getcsr();
                _mm_setcsr(m_Saved | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#endif
            }

            ~ScopedFlushDenormals()
            {
#if defined(__aarch64__) || defined(_M_ARM64)
                __asm__ __volatile__("msr fpcr, %0" : : "r"(m_Saved));
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
                _mm_setcsr(m_Saved);
#endif
            }

            ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
            ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

        private:
#if defined(__aarch64__) || defined(_M_ARM64)
            u64 m_Saved = 0;
#else
            unsigned int m_Saved = 0;
#endif
        };

        // The largest block the mixer processes at once; a callback asking for more is split into
        // chunks so the scratch buffers below stay a fixed, pre-allocated size (allocation-free RT).
        constexpr u32 kMaxChunkFrames = 1024;
        // The most frames a single null-device pump advances, so a stalled first frame (or a
        // breakpoint) cannot ask the mixer to grind through minutes of virtual time.
        constexpr u32 kMaxPumpFrames = 4800;
        // A generator voice's Pitch is clamped to this before resampling, bounding how many source
        // samples one chunk pulls (and so the GenScratch size); a Doppler ratio never approaches it.
        constexpr f32 kMaxGeneratorPitch = 4.0f;

        void EqualPowerPan(f32 pan, f32& leftGain, f32& rightGain)
        {
            const f32 t =
                (std::clamp(pan, -1.0f, 1.0f) * 0.5f + 0.5f) * (std::numbers::pi_v<f32> * 0.5f);
            leftGain = std::cos(t);
            rightGain = std::sin(t);
        }

        f32 FrameMono(const VoiceSnapshot& voice, u64 frame)
        {
            const u64 base = frame * voice.PcmChannels;
            f32 sum = 0.0f;
            for (u32 c = 0; c < voice.PcmChannels; ++c)
            {
                sum += voice.Pcm[base + c];
            }
            return sum / static_cast<f32>(voice.PcmChannels);
        }

        f32 SampleMono(const VoiceSnapshot& voice, f64 cursor)
        {
            const u64 i0 = static_cast<u64>(cursor);
            const f32 frac = static_cast<f32>(cursor - static_cast<f64>(i0));
            u64 i1 = i0 + 1;
            if (voice.Loop)
            {
                i1 %= voice.PcmFrameCount;
            }
            else if (i1 >= voice.PcmFrameCount)
            {
                i1 = i0;
            }
            const f32 s0 = FrameMono(voice, i0);
            const f32 s1 = FrameMono(voice, i1);
            return s0 + (s1 - s0) * frac;
        }

        f32 OnePoleCoefficient(f32 cutoffHz, u32 sampleRate)
        {
            const f32 nyquist = static_cast<f32>(sampleRate) * 0.5f;
            if (cutoffHz <= 0.0f || cutoffHz >= nyquist)
            {
                return 1.0f;
            }
            return 1.0f - std::exp(-2.0f * std::numbers::pi_v<f32> * cutoffHz /
                                   static_cast<f32>(sampleRate));
        }
    }

    struct AudioDevice::Native
    {
        /// @brief The main-thread → RT snapshot bridge.
        TripleBuffer<AudioFrame> Snapshots;
        /// @brief The RT → main-thread retired-voice channel.
        SpscRing<RetiredVoice, 256> Retired;
        /// @brief The generation (published serial) the mixer has consumed up to.
        std::atomic<u64> ConsumedSerial{0};

        /// @brief The main-thread → decode-thread stream registration channel (ordered).
        SpscRing<StreamCommand, 512> DecodeCommands;
        /// @brief The main-thread → fill-thread buffered-generator registration channel (ordered).
        SpscRing<BufferedGeneratorCommand, 512> BufferedCommands;
        /// @brief The engine-external decode thread that keeps every stream voice's ring filled.
        ///
        /// It also tops up every buffered-generator ring — the one off-real-time audio fill thread.
        std::thread DecodeThread;
        /// @brief Signals the decode thread to stop; joined before any decoder or clip is freed.
        std::atomic<bool> DecodeStop{false};
        /// @brief Guards the decode thread's wait; the callback thread never touches it.
        std::mutex DecodeMutex;
        /// @brief Wakes the decode thread when a command arrives, cutting its idle latency.
        std::condition_variable DecodeCv;

        /// @brief RT-owned per-voice playback state.
        struct RtVoice
        {
            f64 Cursor = 0.0;
            u32 Generation = 0;
            bool Finished = false;
            f32 OcclusionState = 0.0f;
            /// @brief Generator resampler: fractional read position between GenS0 and GenS1, [0, 1).
            f64 GenFrac = 0.0;
            /// @brief Generator resampler: the two source samples the current output interpolates.
            ///        For a stereo generator these are the left channel's window.
            f32 GenS0 = 0.0f;
            f32 GenS1 = 0.0f;
            /// @brief Generator resampler: the right channel's two-sample window (stereo voice only).
            f32 GenS0R = 0.0f;
            f32 GenS1R = 0.0f;
            /// @brief Whether the two-sample interpolation window has been primed for this voice.
            bool GenPrimed = false;
        };
        std::array<RtVoice, MaxVoices> RtVoices{};

        /// @brief Per-bus low-pass state (stereo).
        std::array<f32, AudioBusCount> BusLpL{};
        std::array<f32, AudioBusCount> BusLpR{};

        /// @brief The first-party master reverb node.
        Reverb MasterReverb;

        /// @brief Per-bus stereo accumulation scratch (sized kMaxChunkFrames * 2).
        std::array<vector<f32>, AudioBusCount> BusAccum;
        /// @brief The mono reverb-send accumulation (sized kMaxChunkFrames).
        vector<f32> ReverbSend;
        /// @brief The stereo wet reverb output (sized kMaxChunkFrames each).
        vector<f32> WetL;
        vector<f32> WetR;
        /// @brief Per-voice mono render scratch (sized kMaxChunkFrames); a stereo generator's left.
        vector<f32> VoiceScratch;
        /// @brief A stereo generator's right-channel render scratch (sized kMaxChunkFrames).
        vector<f32> VoiceScratchR;
        /// @brief Generator source-render scratch (sized for the widest interleaved-stereo resample).
        vector<f32> GenScratch;

        /// @brief The last null-device pump output, for headless inspection and tests.
        vector<f32> NullOutput;

        u32 Channels = 2;
        u32 SampleRate = 48000;

        /// @brief Whether a real miniaudio device was initialized.
        bool HasDevice = false;
        /// @brief The miniaudio playback device (valid only when HasDevice).
        ma_device Device{};

        void PrepareScratch(u32 sampleRate, u32 channels)
        {
            Channels = channels;
            SampleRate = sampleRate;
            for (auto& bus : BusAccum)
            {
                bus.assign(static_cast<usize>(kMaxChunkFrames) * 2, 0.0f);
            }
            ReverbSend.assign(kMaxChunkFrames, 0.0f);
            WetL.assign(kMaxChunkFrames, 0.0f);
            WetR.assign(kMaxChunkFrames, 0.0f);
            VoiceScratch.assign(kMaxChunkFrames, 0.0f);
            VoiceScratchR.assign(kMaxChunkFrames, 0.0f);
            // The most source samples a chunk resamples: floor(GenFrac + frames * pitch) + 1, bounded
            // by the pitch clamp; a fixed margin covers the +1 and the fractional carry. Doubled for
            // an interleaved stereo pull, so this one buffer covers the mono pull too.
            GenScratch.assign(static_cast<usize>(kMaxChunkFrames) *
                                      static_cast<usize>(kMaxGeneratorPitch) * 2 +
                                  8,
                              0.0f);
            MasterReverb.Prepare(sampleRate);
        }
    };

    // -- self-test tone ------------------------------------------------------

    SelfTestTone GenerateSelfTestTone(u32 sampleRate)
    {
        SelfTestTone tone;
        tone.SampleRate = sampleRate;
        tone.Channels = 1;
        tone.FrameCount = static_cast<u64>(sampleRate) / 4; // 0.25 s
        tone.Samples.resize(tone.FrameCount);

        constexpr f32 amplitude = 0.2f;
        constexpr f32 frequency = 440.0f;
        const u64 fadeFrames = std::min<u64>(tone.FrameCount / 8, sampleRate / 100);
        f32 peak = 0.0f;
        for (u64 i = 0; i < tone.FrameCount; ++i)
        {
            const f32 phase = 2.0f * std::numbers::pi_v<f32> * frequency * static_cast<f32>(i) /
                              static_cast<f32>(sampleRate);
            f32 envelope = 1.0f;
            if (fadeFrames > 0)
            {
                if (i < fadeFrames)
                {
                    envelope = static_cast<f32>(i) / static_cast<f32>(fadeFrames);
                }
                else if (i >= tone.FrameCount - fadeFrames)
                {
                    envelope =
                        static_cast<f32>(tone.FrameCount - 1 - i) / static_cast<f32>(fadeFrames);
                }
            }
            const f32 sample = std::sin(phase) * amplitude * envelope;
            tone.Samples[i] = sample;
            peak = std::max(peak, std::abs(sample));
        }
        tone.Peak = peak;
        return tone;
    }

    // -- device --------------------------------------------------------------

    namespace
    {
        void DeviceDataCallback(ma_device* device, void* output, const void* /*input*/,
                                ma_uint32 frameCount)
        {
            auto* self = static_cast<AudioDevice*>(device->pUserData);
            const u32 channels = self->GetChannels();
            self->RenderBlock(
                {static_cast<f32*>(output), static_cast<usize>(frameCount) * channels}, frameCount);
        }

        // Pulls one chunk of mono samples from a generator voice into native.VoiceScratch, applying
        // its occlusion low-pass. The generator renders at the device rate; Pitch (Doppler) is
        // applied by linearly resampling that stream through a two-sample interpolation window
        // (GenS0/GenS1) advanced by a fractional cursor. Each chunk pulls exactly the source samples
        // the cursor crosses — no lookahead is rendered and dropped — so a stateful generator
        // advances contiguously across chunks with no seam. Render is the only foreign code the RT
        // thread runs; the contract binds it.
        void RenderGeneratorChunk(AudioDevice::Native& native, const VoiceSnapshot& voice,
                                  AudioDevice::Native::RtVoice& rt, u32 frames)
        {
            const f64 ratio = static_cast<f64>(std::clamp(voice.Pitch, 0.01f, kMaxGeneratorPitch));
            f32* batch = native.GenScratch.data();

            // Prime the interpolation window from the generator's first two samples, once per voice.
            if (!rt.GenPrimed)
            {
                voice.Generator->Render(batch, 2, 1, native.SampleRate);
                rt.GenS0 = batch[0];
                rt.GenS1 = batch[1];
                rt.GenFrac = 0.0;
                rt.GenPrimed = true;
            }

            // The cursor advances ratio per output frame; the number of integer boundaries it crosses
            // is exactly how many new source samples this chunk consumes (GenFrac starts in [0, 1)).
            const u64 pulls = static_cast<u64>(rt.GenFrac + static_cast<f64>(frames) * ratio);
            VE_ASSERT(pulls <= native.GenScratch.size(),
                      "generator chunk pulls {} source samples, scratch holds {}", pulls,
                      native.GenScratch.size());
            voice.Generator->Render(batch, static_cast<u32>(pulls), 1, native.SampleRate);

            const f32 occlusionCoef = 1.0f - std::clamp(voice.Occlusion, 0.0f, 1.0f) * 0.98f;
            f64 frac = rt.GenFrac;
            u64 pullIdx = 0;
            for (u32 i = 0; i < frames; ++i)
            {
                const f32 sample = rt.GenS0 + (rt.GenS1 - rt.GenS0) * static_cast<f32>(frac);
                rt.OcclusionState += occlusionCoef * (sample - rt.OcclusionState);
                native.VoiceScratch[i] = rt.OcclusionState;
                frac += ratio;
                while (frac >= 1.0)
                {
                    rt.GenS0 = rt.GenS1;
                    rt.GenS1 = batch[pullIdx++];
                    frac -= 1.0;
                }
            }
            rt.GenFrac = frac;
        }

        // Pulls one chunk of interleaved stereo from a non-spatial generator voice into
        // native.VoiceScratch (left) and native.VoiceScratchR (right). The generator renders at the
        // device rate; the only resample ratio is the voice's base Pitch (a non-spatial voice has no
        // Doppler), applied per channel through a shared two-sample window advanced by one fractional
        // cursor — so at unity pitch this is a straight device-rate copy. No occlusion: a stereo
        // voice is non-spatial by construction, so its occlusion is a bypass.
        void RenderGeneratorStereoChunk(AudioDevice::Native& native, const VoiceSnapshot& voice,
                                        AudioDevice::Native::RtVoice& rt, u32 frames)
        {
            const f64 ratio = static_cast<f64>(std::clamp(voice.Pitch, 0.01f, kMaxGeneratorPitch));
            f32* batch = native.GenScratch.data();

            // Prime both channels' windows from the generator's first two interleaved frames, once.
            if (!rt.GenPrimed)
            {
                voice.Generator->Render(batch, 2, 2, native.SampleRate);
                rt.GenS0 = batch[0];
                rt.GenS1 = batch[2];
                rt.GenS0R = batch[1];
                rt.GenS1R = batch[3];
                rt.GenFrac = 0.0;
                rt.GenPrimed = true;
            }

            const u64 pulls = static_cast<u64>(rt.GenFrac + static_cast<f64>(frames) * ratio);
            VE_ASSERT(pulls * 2 <= native.GenScratch.size(),
                      "stereo generator chunk pulls {} interleaved samples, scratch holds {}",
                      pulls * 2, native.GenScratch.size());
            voice.Generator->Render(batch, static_cast<u32>(pulls), 2, native.SampleRate);

            f64 frac = rt.GenFrac;
            u64 pullIdx = 0;
            for (u32 i = 0; i < frames; ++i)
            {
                native.VoiceScratch[i] = rt.GenS0 + (rt.GenS1 - rt.GenS0) * static_cast<f32>(frac);
                native.VoiceScratchR[i] =
                    rt.GenS0R + (rt.GenS1R - rt.GenS0R) * static_cast<f32>(frac);
                frac += ratio;
                while (frac >= 1.0)
                {
                    rt.GenS0 = rt.GenS1;
                    rt.GenS1 = batch[pullIdx * 2];
                    rt.GenS0R = rt.GenS1R;
                    rt.GenS1R = batch[pullIdx * 2 + 1];
                    ++pullIdx;
                    frac -= 1.0;
                }
            }
            rt.GenFrac = frac;
        }

        // Pulls one chunk of mono samples from a stream voice's ring into native.VoiceScratch,
        // applying its occlusion low-pass. The ring carries mono samples at the clip's rate; Pitch
        // and the clip->device rate conversion are the same two-sample resampling window the
        // generator path uses (GenS0/GenS1 advanced by a fractional cursor) — no second resampler.
        // An empty ring is an underrun: the block's remaining frames are silence and the window
        // freezes so playback resumes cleanly when samples return. When the ring drains and the
        // decode thread has marked the stream ended, the voice is reported exhausted so the mixer
        // retires it. RT-thread code: it only drains the lock-free ring, never decodes.
        void RenderStreamChunk(AudioDevice::Native& native, const VoiceSnapshot& voice,
                               AudioDevice::Native::RtVoice& rt, u32 frames, bool& exhausted)
        {
            StreamVoice& stream = *voice.Stream;
            const f64 srcRate = voice.PcmSampleRate == 0 ? static_cast<f64>(native.SampleRate)
                                                         : static_cast<f64>(voice.PcmSampleRate);
            const f64 ratio = (srcRate / static_cast<f64>(native.SampleRate)) *
                              static_cast<f64>(std::clamp(voice.Pitch, 0.01f, kMaxGeneratorPitch));
            const f32 occlusionCoef = 1.0f - std::clamp(voice.Occlusion, 0.0f, 1.0f) * 0.98f;

            u32 i = 0;
            for (; i < frames; ++i)
            {
                if (!rt.GenPrimed)
                {
                    f32 s = 0.0f;
                    if (!stream.Ring.Pop(s))
                    {
                        if (stream.AtEnd.load(std::memory_order_acquire))
                        {
                            exhausted = true;
                            break;
                        }
                        native.VoiceScratch[i] = 0.0f;
                        continue;
                    }
                    rt.GenS0 = s;
                    rt.GenS1 = stream.Ring.Pop(s) ? s : rt.GenS0;
                    rt.GenFrac = 0.0;
                    rt.GenPrimed = true;
                }

                const f32 sample = rt.GenS0 + (rt.GenS1 - rt.GenS0) * static_cast<f32>(rt.GenFrac);
                rt.OcclusionState += occlusionCoef * (sample - rt.OcclusionState);
                native.VoiceScratch[i] = rt.OcclusionState;

                rt.GenFrac += ratio;
                bool starved = false;
                while (rt.GenFrac >= 1.0)
                {
                    f32 s = 0.0f;
                    if (!stream.Ring.Pop(s))
                    {
                        starved = true;
                        break;
                    }
                    rt.GenS0 = rt.GenS1;
                    rt.GenS1 = s;
                    rt.GenFrac -= 1.0;
                }
                if (starved)
                {
                    if (stream.AtEnd.load(std::memory_order_acquire))
                    {
                        exhausted = true;
                    }
                    // Freeze the fractional cursor: the next block resumes from the same window when
                    // more samples arrive (underrun) or the voice is retired (end).
                    rt.GenFrac = 0.0;
                    ++i;
                    break;
                }
            }
            for (; i < frames; ++i)
            {
                native.VoiceScratch[i] = 0.0f;
            }
        }

        // Drains one chunk from a buffered generator voice's ring into native.VoiceScratch (mono) or
        // native.VoiceScratch/VoiceScratchR (stereo). The fill thread rendered the samples at the
        // device rate, so the callback only pops — no resample, no occlusion (a buffered voice is
        // non-spatial). An empty ring is an underrun: the missing frames are silence and the drain
        // continues, never blocking and never allocating. A stereo frame is popped only when both its
        // samples are present, so an underrun mid-pair never shifts the L/R interleaving. RT-thread
        // code: it only drains the lock-free ring, never renders.
        void RenderBufferedChunk(AudioDevice::Native& native, const VoiceSnapshot& voice,
                                 u32 frames)
        {
            BufferedGenerator& gen = *voice.Buffered;
            if (voice.GeneratorChannels == 2)
            {
                for (u32 i = 0; i < frames; ++i)
                {
                    f32 l = 0.0f;
                    f32 r = 0.0f;
                    if (gen.Ring.Available() >= 2)
                    {
                        gen.Ring.Pop(l);
                        gen.Ring.Pop(r);
                    }
                    native.VoiceScratch[i] = l;
                    native.VoiceScratchR[i] = r;
                }
            }
            else
            {
                for (u32 i = 0; i < frames; ++i)
                {
                    f32 s = 0.0f;
                    gen.Ring.Pop(s);
                    native.VoiceScratch[i] = s;
                }
            }
        }

        // Fills a buffered generator's ring by rendering the borrowed generator at the device rate,
        // off the real-time thread. Renders only what the ring has room for (so nothing rendered is
        // dropped), interleaved by the voice's channel count. A generator is unbounded — there is no
        // end and no AtEnd — so this refills forever until the fill thread drops the voice on a
        // Remove. Returns whether it pushed any samples this pass. Render is the only foreign code the
        // fill thread runs; the same lock-free, allocation-free contract binds it.
        bool TopUpBufferedGenerator(BufferedGenerator& gen)
        {
            usize free = gen.Ring.Free();
            const u32 channels = gen.Channels == 0 ? 1 : gen.Channels;
            if (free < channels)
            {
                return false;
            }
            bool produced = false;
            while (free >= channels)
            {
                const u32 wantFrames = static_cast<u32>(
                    std::min<usize>(free / channels, BufferedGeneratorChunkFrames));
                gen.Generator->Render(gen.RenderScratch.data(), wantFrames, channels,
                                      gen.SampleRate);
                const usize count = static_cast<usize>(wantFrames) * channels;
                for (usize i = 0; i < count; ++i)
                {
                    // The ring has room for every sample rendered this pass (count <= free).
                    gen.Ring.Push(gen.RenderScratch[i]);
                }
                free -= count;
                produced = true;
            }
            return produced;
        }

        // Fills a stream voice's ring from its decoder, off the real-time thread. Decodes only what
        // the ring has room for (so nothing decoded is dropped), collapses each frame to mono, and —
        // for a looping stream — seeks back to the start the instant the decoder reports its end, so
        // the loop point carries no gap. A finite stream marks AtEnd when it runs out, which the
        // mixer retires on. Returns whether it pushed any samples this pass.
        bool TopUpStream(StreamVoice& stream)
        {
            if (stream.AtEnd.load(std::memory_order_relaxed))
            {
                return false;
            }
            usize free = stream.Ring.Free();
            if (free == 0)
            {
                return false;
            }

            const u32 channels = stream.Channels == 0 ? 1 : stream.Channels;
            bool produced = false;
            bool progressedSinceSeek = true;
            while (free > 0)
            {
                const u32 want = static_cast<u32>(std::min<usize>(free, StreamDecodeChunkFrames));
                const std::span<f32> scratch(stream.DecodeScratch.data(),
                                             static_cast<usize>(want) * channels);
                const u64 got = stream.Decoder->Read(scratch);
                if (got == 0)
                {
                    // A looping stream rewinds and keeps filling; a finite one ends. The
                    // progress guard stops a degenerate empty loop from spinning this pass.
                    if (stream.Loop && progressedSinceSeek)
                    {
                        stream.Decoder->SeekStart();
                        progressedSinceSeek = false;
                        continue;
                    }
                    if (!stream.Loop)
                    {
                        stream.AtEnd.store(true, std::memory_order_release);
                    }
                    break;
                }
                progressedSinceSeek = true;
                for (u64 f = 0; f < got; ++f)
                {
                    f32 sum = 0.0f;
                    for (u32 c = 0; c < channels; ++c)
                    {
                        sum += stream.DecodeScratch[f * channels + c];
                    }
                    // The ring has room for every sample decoded this pass (got <= want <= free).
                    stream.Ring.Push(sum / static_cast<f32>(channels));
                }
                free -= got;
                produced = true;
            }
            return produced;
        }

        // The engine-external off-real-time audio fill thread: it owns every active stream voice's
        // decoder and every buffered generator, and keeps each ring topped. It learns of voices
        // through two ordered command channels (an Add enrolls a voice into the working set; a Remove
        // drops it and acknowledges through ReleasedByDecoder so the main thread may free it). It
        // sleeps on the condition variable when no ring needed filling. It touches no engine state and
        // no scene API — only the voices' lock-free rings, their decoders, and the borrowed generators
        // it renders — so it is the second sanctioned engine-external thread beside the real-time
        // callback.
        void DecodeThreadMain(AudioDevice::Native* native)
        {
#if defined(__APPLE__)
            // The fill thread must keep every ring topped ahead of the real-time drain, so a busy
            // machine must not deschedule it long enough to underrun a buffered voice — even though
            // its throughput is far above real time, the scheduling latency is what starves it. Raise
            // its QoS to the interactive band (still below the audio callback's real-time thread).
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            std::vector<StreamVoice*> working;
            std::vector<BufferedGenerator*> buffered;
            while (!native->DecodeStop.load(std::memory_order_acquire))
            {
                StreamCommand cmd;
                while (native->DecodeCommands.Pop(cmd))
                {
                    if (cmd.Op == StreamCommand::Kind::Add)
                    {
                        working.push_back(cmd.Stream);
                    }
                    else
                    {
                        std::erase(working, cmd.Stream);
                        cmd.Stream->ReleasedByDecoder.store(true, std::memory_order_release);
                    }
                }

                BufferedGeneratorCommand bcmd;
                while (native->BufferedCommands.Pop(bcmd))
                {
                    if (bcmd.Op == BufferedGeneratorCommand::Kind::Add)
                    {
                        buffered.push_back(bcmd.Generator);
                    }
                    else
                    {
                        std::erase(buffered, bcmd.Generator);
                        bcmd.Generator->ReleasedByDecoder.store(true, std::memory_order_release);
                    }
                }

                bool produced = false;
                for (StreamVoice* stream : working)
                {
                    if (TopUpStream(*stream))
                    {
                        produced = true;
                    }
                }
                for (BufferedGenerator* gen : buffered)
                {
                    if (TopUpBufferedGenerator(*gen))
                    {
                        produced = true;
                    }
                }

                if (!produced)
                {
                    std::unique_lock lock(native->DecodeMutex);
                    native->DecodeCv.wait_for(lock, std::chrono::milliseconds(2));
                }
            }
        }

        // Mixes one bounded chunk of the snapshot into an interleaved stereo output. Runs on the
        // real-time callback thread (or the main thread in null mode) and touches only the device's
        // own RT state — never the engine or any scene API.
        void MixChunk(AudioDevice::Native& native, const AudioFrame& frame, f32* out, u32 frames)
        {
            const u32 sampleRate = native.SampleRate;
            const u32 channels = native.Channels;

            for (auto& bus : native.BusAccum)
            {
                std::fill_n(bus.data(), static_cast<usize>(frames) * 2, 0.0f);
            }
            std::fill_n(native.ReverbSend.data(), frames, 0.0f);

            for (u32 slot = 0; slot < MaxVoices; ++slot)
            {
                const VoiceSnapshot& voice = frame.Voices[slot];
                AudioDevice::Native::RtVoice& rt = native.RtVoices[slot];
                const bool isGenerator = voice.Generator != nullptr;
                const bool isStream = voice.Stream != nullptr;
                const bool isBuffered = voice.Buffered != nullptr;
                if (!voice.Active || (!isGenerator && !isStream && !isBuffered &&
                                      (voice.Pcm == nullptr || voice.PcmFrameCount == 0)))
                {
                    continue;
                }
                if (rt.Generation != voice.Generation)
                {
                    rt.Cursor = 0.0;
                    rt.Generation = voice.Generation;
                    rt.Finished = false;
                    rt.OcclusionState = 0.0f;
                    rt.GenFrac = 0.0;
                    rt.GenS0 = 0.0f;
                    rt.GenS1 = 0.0f;
                    rt.GenS0R = 0.0f;
                    rt.GenS1R = 0.0f;
                    rt.GenPrimed = false;
                }
                if (rt.Finished)
                {
                    continue;
                }

                // A generator or buffered voice may carry an authored stereo image; both fan the same
                // way (straight to the stereo bus, bypassing the pan).
                const bool stereoImage =
                    (isGenerator || isBuffered) && voice.GeneratorChannels == 2;
                bool exhausted = false;
                if (isGenerator)
                {
                    // An on-demand generator is unbounded; it renders at the device rate and never
                    // retires by exhaustion — only StopVoice removes it.
                    if (stereoImage)
                    {
                        RenderGeneratorStereoChunk(native, voice, rt, frames);
                    }
                    else
                    {
                        RenderGeneratorChunk(native, voice, rt, frames);
                    }
                }
                else if (isBuffered)
                {
                    // A buffered generator rendered ahead of time on the fill thread; the callback
                    // only drains its ring, never retiring by exhaustion (only StopVoice removes it).
                    RenderBufferedChunk(native, voice, frames);
                }
                else if (isStream)
                {
                    RenderStreamChunk(native, voice, rt, frames, exhausted);
                }
                else
                {
                    const f64 srcRate = voice.PcmSampleRate == 0
                                            ? static_cast<f64>(sampleRate)
                                            : static_cast<f64>(voice.PcmSampleRate);
                    const f64 ratio = (srcRate / static_cast<f64>(sampleRate)) *
                                      static_cast<f64>(std::max(voice.Pitch, 0.0f));
                    const f32 occlusionCoef =
                        1.0f - std::clamp(voice.Occlusion, 0.0f, 1.0f) * 0.98f;

                    for (u32 i = 0; i < frames; ++i)
                    {
                        if (rt.Cursor >= static_cast<f64>(voice.PcmFrameCount) && !voice.Loop)
                        {
                            native.VoiceScratch[i] = 0.0f;
                            exhausted = true;
                            continue;
                        }
                        f32 sample = SampleMono(voice, rt.Cursor);
                        rt.OcclusionState += occlusionCoef * (sample - rt.OcclusionState);
                        sample = rt.OcclusionState;
                        native.VoiceScratch[i] = sample;
                        rt.Cursor += ratio;
                        if (voice.Loop && rt.Cursor >= static_cast<f64>(voice.PcmFrameCount))
                        {
                            rt.Cursor -= static_cast<f64>(voice.PcmFrameCount);
                        }
                    }
                }

                const f32 gain = std::max(voice.Gain, 0.0f);
                vector<f32>& bus = native.BusAccum[static_cast<usize>(voice.Bus)];
                const f32 send = std::clamp(voice.ReverbSend, 0.0f, 1.0f);
                if (stereoImage)
                {
                    // A non-spatial stereo generator carries an authored L/R image: sum both channels
                    // straight into the stereo bus with no EqualPowerPan, and fold to a mono reverb
                    // send the way the bus-master send folds a stereo bus.
                    for (u32 i = 0; i < frames; ++i)
                    {
                        const f32 l = native.VoiceScratch[i] * gain;
                        const f32 r = native.VoiceScratchR[i] * gain;
                        bus[i * 2] += l;
                        bus[i * 2 + 1] += r;
                        native.ReverbSend[i] += (l + r) * 0.5f * send;
                    }
                }
                else
                {
                    f32 leftGain = 0.0f;
                    f32 rightGain = 0.0f;
                    EqualPowerPan(voice.Pan, leftGain, rightGain);
                    for (u32 i = 0; i < frames; ++i)
                    {
                        const f32 s = native.VoiceScratch[i] * gain;
                        bus[i * 2] += s * leftGain;
                        bus[i * 2 + 1] += s * rightGain;
                        native.ReverbSend[i] += s * send;
                    }
                }

                if (exhausted && !voice.Loop)
                {
                    rt.Finished = true;
                    native.Retired.Push(RetiredVoice{.Slot = slot, .Generation = voice.Generation});
                }
            }

            // Sum the category buses into the master, each through its own low-pass and gain, then
            // apply the master's own low-pass and gain, then add the reverb wet.
            vector<f32>& master = native.BusAccum[static_cast<usize>(AudioBus::Master)];
            for (usize b = 0; b < AudioBusCount; ++b)
            {
                if (b == static_cast<usize>(AudioBus::Master))
                {
                    continue;
                }
                vector<f32>& bus = native.BusAccum[b];
                const f32 coef = OnePoleCoefficient(frame.BusLowpassCutoff[b], sampleRate);
                const f32 gain = std::max(frame.BusGain[b], 0.0f);
                const f32 busSend = std::clamp(frame.BusReverbSend[b], 0.0f, 1.0f);
                for (u32 i = 0; i < frames; ++i)
                {
                    native.BusLpL[b] += coef * (bus[i * 2] - native.BusLpL[b]);
                    native.BusLpR[b] += coef * (bus[i * 2 + 1] - native.BusLpR[b]);
                    const f32 l = native.BusLpL[b] * gain;
                    const f32 r = native.BusLpR[b] * gain;
                    master[i * 2] += l;
                    master[i * 2 + 1] += r;
                    native.ReverbSend[i] += (l + r) * 0.5f * busSend;
                }
            }

            const auto masterIdx = static_cast<usize>(AudioBus::Master);
            const f32 masterCoef =
                OnePoleCoefficient(frame.BusLowpassCutoff[masterIdx], sampleRate);
            const f32 masterGain = std::max(frame.BusGain[masterIdx], 0.0f);
            const f32 masterSend = std::clamp(frame.BusReverbSend[masterIdx], 0.0f, 1.0f);
            for (u32 i = 0; i < frames; ++i)
            {
                native.BusLpL[masterIdx] += masterCoef * (master[i * 2] - native.BusLpL[masterIdx]);
                native.BusLpR[masterIdx] +=
                    masterCoef * (master[i * 2 + 1] - native.BusLpR[masterIdx]);
                master[i * 2] = native.BusLpL[masterIdx];
                master[i * 2 + 1] = native.BusLpR[masterIdx];
                native.ReverbSend[i] += (master[i * 2] + master[i * 2 + 1]) * 0.5f * masterSend;
            }

            native.MasterReverb.ProcessBlock(native.ReverbSend.data(), native.WetL.data(),
                                             native.WetR.data(), frames, frame.Reverb);
            const f32 wet = std::clamp(frame.Reverb.Wet, 0.0f, 1.0f);

            for (u32 i = 0; i < frames; ++i)
            {
                const f32 left = master[i * 2] * masterGain + native.WetL[i] * wet;
                const f32 right = master[i * 2 + 1] * masterGain + native.WetR[i] * wet;
                if (channels == 1)
                {
                    out[i] = (left + right) * 0.5f;
                }
                else
                {
                    out[static_cast<usize>(i) * channels] = left;
                    out[static_cast<usize>(i) * channels + 1] = right;
                    for (u32 c = 2; c < channels; ++c)
                    {
                        out[static_cast<usize>(i) * channels + c] = 0.0f;
                    }
                }
            }
        }
    }

    AudioDevice::AudioDevice(const AudioDeviceInfo& info)
        : m_Engine(CreateUnique<AudioEngine>(*this)), m_Native(CreateUnique<Native>()),
          m_Channels(info.Channels == 0 ? 2 : info.Channels),
          m_SampleRate(info.SampleRate == 0 ? 48000 : info.SampleRate)
    {
        bool started = false;
        if (info.Backend == AudioBackend::Auto)
        {
            ma_device_config config = ma_device_config_init(ma_device_type_playback);
            config.playback.format = ma_format_f32;
            config.playback.channels = m_Channels;
            config.sampleRate = info.SampleRate;
            config.dataCallback = &DeviceDataCallback;
            config.pUserData = this;
            if (ma_device_init(nullptr, &config, &m_Native->Device) == MA_SUCCESS)
            {
                m_Channels = m_Native->Device.playback.channels;
                m_SampleRate = m_Native->Device.sampleRate;
                m_Native->HasDevice = true;
                m_Backend = AudioBackend::Auto;
                started = ma_device_start(&m_Native->Device) == MA_SUCCESS;
                if (!started)
                {
                    ma_device_uninit(&m_Native->Device);
                    m_Native->HasDevice = false;
                }
            }
        }

        if (!started)
        {
            m_Backend = AudioBackend::Null;
            if (m_SampleRate == 0)
            {
                m_SampleRate = 48000;
            }
        }

        m_Native->PrepareScratch(m_SampleRate, m_Channels);

        // The decode thread runs regardless of backend: on the null device the main-thread Pump
        // drains the rings it fills, so a finite stream still retires headless with no device.
        m_Native->DecodeThread = std::thread(DecodeThreadMain, m_Native.get());

        Log::Info("Audio: {} backend, {} Hz, {} channels", IsNull() ? "null" : "hardware",
                  m_SampleRate, m_Channels);

        if (info.RunSelfTest)
        {
            RunSelfTest();
        }
    }

    AudioDevice::~AudioDevice()
    {
        // Stop and join the two engine-external threads before any referenced resource can be freed:
        // the decode thread (which owns the stream decoders) first, then the callback thread (via
        // ma_device_uninit). Once both are quiesced the engine's deferred sources — buffers, and the
        // stream voices carrying decoders and clip handles — release safely as members destruct.
        if (m_Native && m_Native->DecodeThread.joinable())
        {
            m_Native->DecodeStop.store(true, std::memory_order_release);
            m_Native->DecodeCv.notify_one();
            m_Native->DecodeThread.join();
        }
        if (m_Native && m_Native->HasDevice)
        {
            ma_device_uninit(&m_Native->Device);
            m_Native->HasDevice = false;
        }
    }

    Unique<AudioDevice> AudioDevice::Create(const AudioDeviceInfo& info)
    {
        return Unique<AudioDevice>(new AudioDevice(info));
    }

    AudioDevice::Native& AudioDevice::GetNative() const
    {
        return *m_Native;
    }

    u64 AudioDevice::GetConsumedSerial() const
    {
        return m_Native->ConsumedSerial.load(std::memory_order_acquire);
    }

    std::span<const f32> AudioDevice::GetDebugMixBuffer() const
    {
        return m_Native->NullOutput;
    }

    void AudioDevice::RunSelfTest()
    {
        const SelfTestTone tone = GenerateSelfTestTone(m_SampleRate);
        const Ref<AudioBuffer> buffer =
            AudioBuffer::Create(tone.Samples, tone.Channels, tone.SampleRate);
        m_Engine->AddVoice(buffer,
                           VoiceParams{.Bus = AudioBus::Master, .Gain = 1.0f, .Loop = false});
    }

    void AudioDevice::Pump(f32 deltaSeconds)
    {
        m_Engine->Publish();

        if (IsNull())
        {
            u32 frames =
                static_cast<u32>(std::lround(deltaSeconds * static_cast<f32>(m_SampleRate)));
            frames = std::min(frames, kMaxPumpFrames);
            if (frames > 0)
            {
                m_Native->NullOutput.assign(static_cast<usize>(frames) * m_Channels, 0.0f);
                RenderBlock(m_Native->NullOutput, frames);
            }
        }

        m_Engine->DrainRetired();
        m_Engine->CollectDeferred();
    }

    void AudioDevice::RenderBlock(std::span<f32> output, u32 frames)
    {
        // Subnormals in the reverb/filter tails are slow enough to underrun the RT thread; flush them
        // for the mix (restored on exit so the null-device main-thread path is unaffected).
        const ScopedFlushDenormals flushDenormals;

        Native& native = *m_Native;
        native.Snapshots.FetchNewest();
        const AudioFrame& frame = native.Snapshots.FrontBuffer();

        const u32 channels = native.Channels;
        VE_ASSERT(output.size() >= static_cast<usize>(frames) * channels,
                  "RenderBlock output span {} too small for {} frames of {} channels",
                  output.size(), frames, channels);

        u32 done = 0;
        while (done < frames)
        {
            const u32 chunk = std::min(frames - done, kMaxChunkFrames);
            MixChunk(native, frame, output.data() + static_cast<usize>(done) * channels, chunk);
            done += chunk;
        }

        native.ConsumedSerial.store(frame.Serial, std::memory_order_release);
    }

    // -- engine --------------------------------------------------------------

    AudioEngine::AudioEngine(AudioDevice& device)
        : m_Device(device), m_Music(CreateUnique<MusicDirector>(*this))
    {
    }

    AudioEngine::~AudioEngine() = default;

    void AudioEngine::SetBusGain(AudioBus bus, f32 gain)
    {
        m_BusGain[static_cast<usize>(bus)] = std::max(gain, 0.0f);
    }

    f32 AudioEngine::GetBusGain(AudioBus bus) const
    {
        return m_BusGain[static_cast<usize>(bus)];
    }

    void AudioEngine::SetBusLowpassCutoff(AudioBus bus, f32 cutoffHz)
    {
        m_BusLowpassCutoff[static_cast<usize>(bus)] = std::max(cutoffHz, 0.0f);
    }

    f32 AudioEngine::GetBusLowpassCutoff(AudioBus bus) const
    {
        return m_BusLowpassCutoff[static_cast<usize>(bus)];
    }

    void AudioEngine::SetBusReverbSend(AudioBus bus, f32 send)
    {
        m_BusReverbSend[static_cast<usize>(bus)] = std::clamp(send, 0.0f, 1.0f);
    }

    f32 AudioEngine::GetBusReverbSend(AudioBus bus) const
    {
        return m_BusReverbSend[static_cast<usize>(bus)];
    }

    void AudioEngine::SetReverbParams(const ReverbParams& params)
    {
        m_Reverb = params;
    }

    ReverbParams AudioEngine::GetReverbParams() const
    {
        return m_Reverb;
    }

    u32 AudioEngine::GetOutputSampleRate() const
    {
        return m_Device.GetSampleRate();
    }

    u32 AudioEngine::AllocateSlot(const f32 incomingGain)
    {
        for (u32 i = 0; i < MaxVoices; ++i)
        {
            if (!m_Voices[i].Active)
            {
                return i;
            }
        }
        // Budget full: evict the quietest active voice, but only if the incoming voice is louder —
        // otherwise it is the one that loses, and the request is rejected.
        u32 quietest = VoiceHandle::InvalidSlot;
        f32 quietestGain = incomingGain;
        for (u32 i = 0; i < MaxVoices; ++i)
        {
            if (m_Voices[i].Active && m_Voices[i].Params.Gain < quietestGain)
            {
                quietestGain = m_Voices[i].Params.Gain;
                quietest = i;
            }
        }
        if (quietest == VoiceHandle::InvalidSlot)
        {
            return VoiceHandle::InvalidSlot;
        }
        RetireSlot(quietest);
        return quietest;
    }

    VoiceHandle AudioEngine::AddVoice(const Ref<AudioBuffer>& buffer, const VoiceParams& params)
    {
        VE_ASSERT(buffer, "AddVoice requires a non-null buffer");

        const u32 slot = AllocateSlot(params.Gain);
        if (slot == VoiceHandle::InvalidSlot)
        {
            return {};
        }

        Voice& voice = m_Voices[slot];
        voice.Generation += 1;
        voice.Active = true;
        voice.Source = buffer;
        voice.Generator = nullptr;
        voice.GeneratorChannels = 1;
        voice.Params = params;
        // A raw voice carries no engine-owned metadata; PlayOneShot / PlayAt / the director stamp the
        // slot after this returns, so a reused slot never inherits its predecessor's managed role.
        m_Managed[slot] = Managed{};
        ++m_ActiveCount;
        return VoiceHandle{.Slot = slot, .Generation = voice.Generation};
    }

    VoiceHandle AudioEngine::AddStreamVoice(const AssetHandle<AudioClip>& clip,
                                            const VoiceParams& params)
    {
        AudioClip* resolved = clip.Get();
        if (resolved == nullptr || resolved->Storage() != AudioStorage::Encoded)
        {
            return {};
        }
        Result<Unique<VorbisMemoryDecoder>> decoder = resolved->OpenDecoder();
        if (!decoder)
        {
            return {};
        }

        auto stream = CreateUnique<StreamVoice>();
        stream->Clip = clip;
        stream->Decoder = std::move(*decoder);
        stream->Loop = params.Loop;
        stream->Channels = stream->Decoder->Channels() == 0 ? 1 : stream->Decoder->Channels();
        stream->SampleRate = stream->Decoder->SampleRate();
        stream->DecodeScratch.assign(static_cast<usize>(StreamDecodeChunkFrames) * stream->Channels,
                                     0.0f);

        const u32 slot = AllocateSlot(params.Gain);
        if (slot == VoiceHandle::InvalidSlot)
        {
            return {};
        }

        StreamVoice* streamPtr = stream.get();
        Voice& voice = m_Voices[slot];
        voice.Generation += 1;
        voice.Active = true;
        voice.Source = nullptr;
        voice.Generator = nullptr;
        voice.GeneratorChannels = 1;
        voice.Stream = std::move(stream);
        voice.Params = params;
        m_Managed[slot] = Managed{};
        ++m_ActiveCount;

        // Enroll the stream with the decode thread so it begins filling the ring before the next mix.
        AudioDevice::Native& native = m_Device.GetNative();
        native.DecodeCommands.Push(
            StreamCommand{.Op = StreamCommand::Kind::Add, .Stream = streamPtr});
        native.DecodeCv.notify_one();

        return VoiceHandle{.Slot = slot, .Generation = voice.Generation};
    }

    VoiceHandle AudioEngine::AddClipVoice(const AssetHandle<AudioClip>& clip,
                                          const VoiceParams& params)
    {
        AudioClip* resolved = clip.Get();
        if (resolved == nullptr)
        {
            return {};
        }
        if (resolved->Storage() == AudioStorage::Encoded)
        {
            return AddStreamVoice(clip, params);
        }
        if (resolved->Buffer())
        {
            return AddVoice(resolved->Buffer(), params);
        }
        return {};
    }

    AssetHandle<AudioClip> AudioEngine::CreateClip(const std::span<const f32> samples,
                                                   const AudioBufferFormat format)
    {
        const u32 channels = format.Channels == 0 ? 1 : format.Channels;
        const u32 sampleRate =
            format.SampleRate == 0 ? m_Device.GetSampleRate() : format.SampleRate;
        Ref<AudioBuffer> buffer = AudioBuffer::Create(samples, channels, sampleRate);
        return AssetManager::Adopt<AudioClip>(AudioClip::CreatePcm(std::move(buffer)));
    }

    VoiceHandle AudioEngine::PlayGenerator(IAudioGenerator* generator,
                                           const GeneratorVoiceParams& params)
    {
        VE_ASSERT(generator != nullptr, "PlayGenerator requires a non-null generator");

        // Stereo is a non-spatial-only width: spatialization is a mono-source-then-pan model, so a
        // stereo point source has no defined pan. Reject it the way a full budget rejects a voice.
        if (params.Channels == 2 && params.Spatial)
        {
            return {};
        }
        // A buffered voice renders ahead of time on the fill thread and carries no per-frame pan or
        // Doppler, so it is non-spatial by construction; reject a buffered spatial request likewise.
        if (params.Buffered && params.Spatial)
        {
            return {};
        }

        // A spatial generator carries the same Managed metadata a PlayAt voice does, so it is
        // re-spatialized by UpdateManagedVoices and moved by SetVoicePose through the one shared
        // path; a non-spatial generator routes straight to its bus with static params.
        Managed managed;
        VoiceParams voiceParams;
        if (params.Spatial)
        {
            managed = Managed{.Kind = ManagedKind::Spatial,
                              .Bus = params.Bus,
                              .BaseGain = params.Gain,
                              .BasePitch = params.Pitch,
                              .Loop = false,
                              .WorldPos = params.Position,
                              .Velocity = params.Velocity,
                              .MinDistance = params.MinDistance,
                              .MaxDistance = params.MaxDistance,
                              .Occlusion = params.OcclusionFactor};
            voiceParams = SpatializeManaged(managed, m_Listener);
        }
        else
        {
            voiceParams = VoiceParams{
                .Bus = params.Bus, .Gain = params.Gain, .Pitch = params.Pitch, .Loop = false};
        }

        const u32 slot = AllocateSlot(params.Gain);
        if (slot == VoiceHandle::InvalidSlot)
        {
            return {};
        }

        Voice& voice = m_Voices[slot];
        voice.Generation += 1;
        voice.Active = true;
        voice.Source = nullptr;
        voice.Generator = generator;
        voice.GeneratorChannels = params.Channels == 2 ? 2 : 1;
        voice.Params = voiceParams;
        m_Managed[slot] = managed;

        if (params.Buffered)
        {
            // Wrap the borrowed generator in an engine-owned ring the fill thread fills off the
            // real-time thread, and enroll it before the next mix so the ring has samples to drain.
            auto buffered = CreateUnique<BufferedGenerator>();
            buffered->Generator = generator;
            buffered->Channels = voice.GeneratorChannels;
            buffered->SampleRate = m_Device.GetSampleRate();
            buffered->RenderScratch.assign(
                static_cast<usize>(BufferedGeneratorChunkFrames) * buffered->Channels, 0.0f);

            // Size the ring to the consumer's requested depth (clamped to a sane band), rounded up to
            // a power of two — done here, off the real-time and fill threads, before either can reach
            // it. The band and the pow2 rounding are why BufferSeconds cannot allocate a runaway ring.
            const f32 seconds = std::clamp(params.BufferSeconds, BufferedGeneratorMinSeconds,
                                           BufferedGeneratorMaxSeconds);
            const usize wantSamples =
                static_cast<usize>(std::ceil(static_cast<f32>(buffered->SampleRate) * seconds)) *
                buffered->Channels;
            buffered->Ring.Prepare(std::bit_ceil(std::max<usize>(wantSamples, usize{2})));

            BufferedGenerator* bufferedPtr = buffered.get();
            voice.Buffered = std::move(buffered);

            AudioDevice::Native& native = m_Device.GetNative();
            native.BufferedCommands.Push(BufferedGeneratorCommand{
                .Op = BufferedGeneratorCommand::Kind::Add, .Generator = bufferedPtr});
            native.DecodeCv.notify_one();
        }

        ++m_ActiveCount;
        return VoiceHandle{.Slot = slot, .Generation = voice.Generation};
    }

    bool AudioEngine::IsVoiceLive(VoiceHandle voice) const
    {
        return voice.IsValid() && voice.Slot < MaxVoices && m_Voices[voice.Slot].Active &&
               m_Voices[voice.Slot].Generation == voice.Generation;
    }

    void AudioEngine::SetVoiceParams(VoiceHandle voice, const VoiceParams& params)
    {
        if (IsVoiceLive(voice))
        {
            m_Voices[voice.Slot].Params = params;
        }
    }

    void AudioEngine::StopVoice(VoiceHandle voice)
    {
        if (!IsVoiceLive(voice))
        {
            return;
        }
        const bool isBuffered = m_Voices[voice.Slot].Buffered != nullptr;
        const bool isGenerator = !isBuffered && m_Voices[voice.Slot].Generator != nullptr;
        // The buffered wrapper carries the fill thread's release ack; grab it before RetireSlot moves
        // the wrapper into the deferred queue (from which this main thread will not free it mid-wait).
        BufferedGenerator* bufferedPtr = isBuffered ? m_Voices[voice.Slot].Buffered.get() : nullptr;
        RetireSlot(voice.Slot);
        if (!isGenerator && !isBuffered)
        {
            // A buffer or stream voice's source is reclaimed asynchronously through the deferred-free
            // queue; nothing borrows it, so there is nothing to wait on here.
            return;
        }

        // A generator (plain or buffered) is caller-owned and borrowed, so its reclamation is the
        // reclamation handshake made synchronous: publish a frame that no longer names the generator,
        // then wait until every thread that could reach it is provably past it. For a plain generator
        // that is the mixer's consumed serial reaching the removal frame; a buffered generator adds
        // the fill thread, which renders the borrowed generator, so the wait also covers its Remove
        // ack (ReleasedByDecoder). Once both hold, the caller may free the generator the moment this
        // returns.
        Publish();
        const u64 target = m_PublishedSerial;
        const auto released = [bufferedPtr]
        {
            return bufferedPtr == nullptr ||
                   bufferedPtr->ReleasedByDecoder.load(std::memory_order_acquire);
        };
        if (m_Device.IsNull())
        {
            // No real-time thread exists; drive the mixer here to advance the consumed serial past
            // the removal (mixing one frame latches the just-published generator-free snapshot), then
            // yield until the fill thread — which runs regardless of backend — acknowledges.
            std::array<f32, 8> scratch{};
            const u32 channels = std::min<u32>(m_Device.GetChannels(), 8);
            while (m_Device.GetConsumedSerial() < target || !released())
            {
                if (m_Device.GetConsumedSerial() < target)
                {
                    m_Device.RenderBlock(std::span<f32>(scratch.data(), channels), 1);
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        }
        else
        {
            while (m_Device.GetConsumedSerial() < target || !released())
            {
                std::this_thread::yield();
            }
        }
    }

    usize AudioEngine::GetActiveVoiceCount() const
    {
        return m_ActiveCount;
    }

    vector<VoiceInfo> AudioEngine::GetVoiceInfos() const
    {
        vector<VoiceInfo> infos;
        infos.reserve(m_ActiveCount);
        for (u32 slot = 0; slot < MaxVoices; ++slot)
        {
            const Voice& voice = m_Voices[slot];
            if (!voice.Active)
            {
                continue;
            }
            const Managed& managed = m_Managed[slot];
            VoiceOrigin origin = VoiceOrigin::Source;
            switch (managed.Kind)
            {
            case ManagedKind::OneShot:
                origin = VoiceOrigin::OneShot;
                break;
            case ManagedKind::Spatial:
                origin = VoiceOrigin::Spatial;
                break;
            case ManagedKind::Music:
                origin = VoiceOrigin::Music;
                break;
            case ManagedKind::None:
                origin = VoiceOrigin::Source;
                break;
            }
            const bool spatial = managed.Kind == ManagedKind::Spatial;
            infos.push_back(VoiceInfo{
                .Handle = VoiceHandle{.Slot = slot, .Generation = voice.Generation},
                .Bus = voice.Params.Bus,
                .Origin = origin,
                .Generator = voice.Generator != nullptr,
                .Gain = voice.Params.Gain,
                .Pan = voice.Params.Pan,
                .Pitch = voice.Params.Pitch,
                .Occlusion = voice.Params.Occlusion,
                .ReverbSend = voice.Params.ReverbSend,
                .Loop = voice.Params.Loop,
                .Spatial = spatial,
                .Position = spatial ? managed.WorldPos : vec3(0.0f),
                .Velocity = spatial ? managed.Velocity : vec3(0.0f),
            });
        }
        return infos;
    }

    void AudioEngine::RetireSlot(u32 slot)
    {
        Voice& voice = m_Voices[slot];
        if (!voice.Active)
        {
            return;
        }
        if (voice.Source)
        {
            // The source may still be referenced by the most recently published frame, so it is
            // freed only once the mixer's consumed generation passes this serial.
            m_Deferred.push_back(
                Deferred{.Source = std::move(voice.Source), .SafeAfterSerial = m_PublishedSerial});
        }
        if (voice.Stream)
        {
            // Tell the decode thread to drop the stream (it acks through ReleasedByDecoder), and
            // defer the free until both the mixer's consumed serial passes this frame and the decode
            // thread has released it — the dual handshake keeps the decoder use-after-free-safe.
            AudioDevice::Native& native = m_Device.GetNative();
            native.DecodeCommands.Push(
                StreamCommand{.Op = StreamCommand::Kind::Remove, .Stream = voice.Stream.get()});
            native.DecodeCv.notify_one();
            m_Deferred.push_back(
                Deferred{.Stream = std::move(voice.Stream), .SafeAfterSerial = m_PublishedSerial});
        }
        if (voice.Buffered)
        {
            // Tell the fill thread to drop the buffered generator (it acks through ReleasedByDecoder),
            // and defer the wrapper's free until both the mixer's consumed serial passes this frame
            // and the fill thread has released it — the same dual handshake the stream voice rides.
            AudioDevice::Native& native = m_Device.GetNative();
            native.BufferedCommands.Push(BufferedGeneratorCommand{
                .Op = BufferedGeneratorCommand::Kind::Remove, .Generator = voice.Buffered.get()});
            native.DecodeCv.notify_one();
            m_Deferred.push_back(Deferred{.Buffered = std::move(voice.Buffered),
                                          .SafeAfterSerial = m_PublishedSerial});
        }
        voice.Active = false;
        voice.Source = nullptr;
        m_Managed[slot] = Managed{};
        if (m_ActiveCount > 0)
        {
            --m_ActiveCount;
        }
    }

    void AudioEngine::Publish()
    {
        AudioDevice::Native& native = m_Device.GetNative();
        AudioFrame& frame = native.Snapshots.BackBuffer();

        for (usize b = 0; b < AudioBusCount; ++b)
        {
            frame.BusGain[b] = m_BusGain[b];
            frame.BusLowpassCutoff[b] = m_BusLowpassCutoff[b];
            frame.BusReverbSend[b] = m_BusReverbSend[b];
        }
        frame.Reverb = m_Reverb;

        for (u32 slot = 0; slot < MaxVoices; ++slot)
        {
            VoiceSnapshot& snapshot = frame.Voices[slot];
            const Voice& voice = m_Voices[slot];
            if (voice.Active && (voice.Source || voice.Generator || voice.Stream))
            {
                snapshot.Active = true;
                snapshot.Generation = voice.Generation;
                // A buffered voice's generator ran ahead of time on the fill thread, so the callback
                // must drain its ring, not call Render: publish the ring and null the generator.
                snapshot.Generator = voice.Buffered ? nullptr : voice.Generator;
                snapshot.GeneratorChannels = voice.GeneratorChannels;
                snapshot.Stream = voice.Stream.get();
                snapshot.Buffered = voice.Buffered.get();
                if (voice.Source)
                {
                    const std::span<const f32> samples = voice.Source->Samples();
                    snapshot.Pcm = samples.data();
                    snapshot.PcmFrameCount = voice.Source->FrameCount();
                    snapshot.PcmChannels = voice.Source->Channels();
                    snapshot.PcmSampleRate = voice.Source->SampleRate();
                }
                else
                {
                    snapshot.Pcm = nullptr;
                    snapshot.PcmFrameCount = 0;
                    snapshot.PcmChannels = 0;
                    // A stream carries its clip rate so the resampler converts to the device rate;
                    // the ring is mono, so the buffer channel count is unused on the stream path.
                    snapshot.PcmSampleRate = voice.Stream ? voice.Stream->SampleRate : 0;
                }
                snapshot.Bus = voice.Params.Bus;
                snapshot.Gain = voice.Params.Gain;
                snapshot.Pan = voice.Params.Pan;
                snapshot.Pitch = voice.Params.Pitch;
                snapshot.Occlusion = voice.Params.Occlusion;
                snapshot.ReverbSend = voice.Params.ReverbSend;
                snapshot.Loop = voice.Params.Loop;
            }
            else
            {
                snapshot.Active = false;
                snapshot.Generator = nullptr;
                snapshot.Stream = nullptr;
                snapshot.Buffered = nullptr;
                snapshot.Pcm = nullptr;
                snapshot.PcmFrameCount = 0;
            }
        }

        frame.Serial = ++m_PublishedSerial;
        native.Snapshots.Publish();
    }

    void AudioEngine::DrainRetired()
    {
        AudioDevice::Native& native = m_Device.GetNative();
        RetiredVoice retired;
        while (native.Retired.Pop(retired))
        {
            if (retired.Slot < MaxVoices && m_Voices[retired.Slot].Active &&
                m_Voices[retired.Slot].Generation == retired.Generation)
            {
                RetireSlot(retired.Slot);
            }
        }
    }

    void AudioEngine::CollectDeferred()
    {
        const u64 consumed = m_Device.GetConsumedSerial();
        std::erase_if(m_Deferred,
                      [consumed](const Deferred& deferred)
                      {
                          if (consumed <= deferred.SafeAfterSerial)
                          {
                              return false;
                          }
                          // A stream also waits on the decode thread's release ack, so its decoder
                          // is freed only once neither thread can still reach it.
                          if (deferred.Stream &&
                              !deferred.Stream->ReleasedByDecoder.load(std::memory_order_acquire))
                          {
                              return false;
                          }
                          // A buffered generator wrapper waits on the fill thread's ack the same way.
                          if (deferred.Buffered &&
                              !deferred.Buffered->ReleasedByDecoder.load(std::memory_order_acquire))
                          {
                              return false;
                          }
                          return true;
                      });
    }

    VoiceParams AudioEngine::SpatializeManaged(const Managed& managed,
                                               const ListenerPose& listener) const
    {
        VoiceParams params;
        params.Bus = managed.Bus;
        params.Loop = managed.Loop;
        const f32 distance = glm::length(managed.WorldPos - listener.Position);
        params.Gain = managed.BaseGain * listener.Gain *
                      DistanceAttenuation(distance, managed.MinDistance, managed.MaxDistance);
        params.Pan = StereoPan(listener.Position, listener.Rotation, managed.WorldPos);
        params.Pitch =
            managed.BasePitch * DopplerRatio(listener.Position, listener.Velocity, managed.WorldPos,
                                             managed.Velocity, DefaultSpeedOfSound);
        params.Occlusion = std::clamp(managed.Occlusion, 0.0f, 1.0f);
        params.ReverbSend = ReverbSend(distance, managed.MinDistance, managed.MaxDistance);
        return params;
    }

    bool AudioEngine::ReserveOneShotSlot(const f32 incomingGain)
    {
        u32 count = 0;
        u32 quietest = VoiceHandle::InvalidSlot;
        f32 quietestGain = incomingGain;
        for (u32 i = 0; i < MaxVoices; ++i)
        {
            if (!m_Voices[i].Active || m_Voices[i].Generator != nullptr)
            {
                continue;
            }
            const ManagedKind kind = m_Managed[i].Kind;
            if (kind != ManagedKind::OneShot && kind != ManagedKind::Spatial)
            {
                continue;
            }
            ++count;
            if (m_Managed[i].BaseGain < quietestGain)
            {
                quietestGain = m_Managed[i].BaseGain;
                quietest = i;
            }
        }
        if (count < MaxOneShotVoices)
        {
            return true;
        }
        // The pool is full: evict the quietest only if the incoming voice is louder, else reject.
        if (quietest == VoiceHandle::InvalidSlot)
        {
            return false;
        }
        RetireSlot(quietest);
        return true;
    }

    VoiceHandle AudioEngine::PlayOneShot(const AssetHandle<AudioClip>& clip,
                                         const OneShotParams& params)
    {
        AudioClip* resolved = clip.Get();
        if (resolved == nullptr)
        {
            return {};
        }
        const bool encoded = resolved->Storage() == AudioStorage::Encoded;
        if (!encoded && !resolved->Buffer())
        {
            return {};
        }
        if (!ReserveOneShotSlot(params.Gain))
        {
            return {};
        }
        const VoiceParams voiceParams{
            .Bus = params.Bus, .Gain = params.Gain, .Pitch = params.Pitch, .Loop = params.Loop};
        const VoiceHandle handle =
            encoded ? AddStreamVoice(clip, voiceParams) : AddVoice(resolved->Buffer(), voiceParams);
        if (handle.IsValid())
        {
            m_Managed[handle.Slot] = Managed{.Kind = ManagedKind::OneShot,
                                             .Bus = params.Bus,
                                             .BaseGain = params.Gain,
                                             .BasePitch = params.Pitch,
                                             .Loop = params.Loop};
        }
        return handle;
    }

    VoiceHandle AudioEngine::PlayAt(const AssetHandle<AudioClip>& clip, const vec3 worldPos,
                                    const SpatialOneShotParams& params)
    {
        AudioClip* resolved = clip.Get();
        if (resolved == nullptr)
        {
            return {};
        }
        const bool encoded = resolved->Storage() == AudioStorage::Encoded;
        if (!encoded && !resolved->Buffer())
        {
            return {};
        }
        if (!ReserveOneShotSlot(params.Gain))
        {
            return {};
        }
        const Managed managed{.Kind = ManagedKind::Spatial,
                              .Bus = params.Bus,
                              .BaseGain = params.Gain,
                              .BasePitch = params.Pitch,
                              .Loop = params.Loop,
                              .WorldPos = worldPos,
                              .Velocity = params.Velocity,
                              .MinDistance = params.MinDistance,
                              .MaxDistance = params.MaxDistance,
                              .Occlusion = params.OcclusionFactor};
        const VoiceParams voiceParams = SpatializeManaged(managed, m_Listener);
        const VoiceHandle handle =
            encoded ? AddStreamVoice(clip, voiceParams) : AddVoice(resolved->Buffer(), voiceParams);
        if (handle.IsValid())
        {
            m_Managed[handle.Slot] = managed;
        }
        return handle;
    }

    void AudioEngine::SetVoicePose(const VoiceHandle voice, const vec3 worldPos,
                                   const vec3 velocity)
    {
        if (!IsVoiceLive(voice) || m_Managed[voice.Slot].Kind != ManagedKind::Spatial)
        {
            return;
        }
        Managed& managed = m_Managed[voice.Slot];
        managed.WorldPos = worldPos;
        managed.Velocity = velocity;
        m_Voices[voice.Slot].Params = SpatializeManaged(managed, m_Listener);
    }

    void AudioEngine::UpdateManagedVoices(const ListenerPose& listener, const f32 delta)
    {
        m_Listener = listener;
        m_Music->Advance(delta);
        for (u32 slot = 0; slot < MaxVoices; ++slot)
        {
            if (m_Voices[slot].Active && m_Managed[slot].Kind == ManagedKind::Spatial)
            {
                m_Voices[slot].Params = SpatializeManaged(m_Managed[slot], listener);
            }
        }
    }

    MusicDirector& AudioEngine::Music()
    {
        return *m_Music;
    }

    optional<VoiceParams> AudioEngine::GetVoiceParams(const VoiceHandle voice) const
    {
        if (!IsVoiceLive(voice))
        {
            return std::nullopt;
        }
        return m_Voices[voice.Slot].Params;
    }

    usize AudioEngine::GetManagedVoiceCount() const
    {
        usize count = 0;
        for (u32 i = 0; i < MaxVoices; ++i)
        {
            const ManagedKind kind = m_Managed[i].Kind;
            if (m_Voices[i].Active && m_Voices[i].Generator == nullptr &&
                (kind == ManagedKind::OneShot || kind == ManagedKind::Spatial))
            {
                ++count;
            }
        }
        return count;
    }

    // -- music director ------------------------------------------------------

    MusicDirector::MusicDirector(AudioEngine& engine) : m_Engine(engine) {}

    f32 MusicDirector::TrackGain(const Track& track) const
    {
        if (track.Steady)
        {
            return m_Gain;
        }
        // Equal-power crossfade: the incoming track rises as sin, the outgoing falls as cos, so the
        // summed power stays constant across the fade and neither dips at the midpoint.
        const f32 t = std::clamp(track.Phase, 0.0f, 1.0f) * (std::numbers::pi_v<f32> * 0.5f);
        const f32 envelope = track.FadingOut ? std::cos(t) : std::sin(t);
        return envelope * m_Gain;
    }

    void MusicDirector::Apply(const Track& track) const
    {
        m_Engine.SetVoiceParams(
            track.Voice,
            VoiceParams{.Bus = AudioBus::Music, .Gain = TrackGain(track), .Loop = track.Loop});
    }

    void MusicDirector::Set(const AssetHandle<AudioClip>& track, const MusicTransition& transition)
    {
        // Already the one logical track: a no-op, so re-setting the playing track never re-triggers.
        if (m_Current.Get() != nullptr && m_Current.Get() == track.Get())
        {
            return;
        }

        // Hold at most the crossfade pair: if a fade is still in flight, drop its oldest voice.
        while (m_Tracks.size() >= 2)
        {
            m_Engine.StopVoice(m_Tracks.front().Voice);
            m_Tracks.erase(m_Tracks.begin());
        }

        const bool hardCut = transition.FadeSeconds <= 0.0f;
        for (Track& existing : m_Tracks)
        {
            existing.FadingOut = true;
            existing.Steady = false;
            existing.Phase = 0.0f;
            existing.FadeDuration = transition.FadeSeconds;
        }
        if (hardCut)
        {
            for (const Track& existing : m_Tracks)
            {
                m_Engine.StopVoice(existing.Voice);
            }
            m_Tracks.clear();
        }

        AudioClip* resolved = track.Get();
        const bool playable = resolved != nullptr && (resolved->Buffer() != nullptr ||
                                                      resolved->Storage() == AudioStorage::Encoded);
        if (playable)
        {
            Track incoming;
            incoming.Clip = track;
            incoming.Loop = transition.Loop;
            incoming.FadingOut = false;
            incoming.Phase = 0.0f;
            incoming.FadeDuration = transition.FadeSeconds;
            incoming.Steady = hardCut;
            incoming.Voice = m_Engine.AddClipVoice(track, VoiceParams{.Bus = AudioBus::Music,
                                                                      .Gain = TrackGain(incoming),
                                                                      .Loop = transition.Loop});
            if (incoming.Voice.IsValid())
            {
                m_Engine.m_Managed[incoming.Voice.Slot].Kind = AudioEngine::ManagedKind::Music;
                m_Tracks.push_back(incoming);
            }
        }

        m_Current = track;
    }

    void MusicDirector::Stop(const f32 fadeSeconds)
    {
        if (fadeSeconds <= 0.0f)
        {
            for (const Track& track : m_Tracks)
            {
                m_Engine.StopVoice(track.Voice);
            }
            m_Tracks.clear();
        }
        else
        {
            for (Track& track : m_Tracks)
            {
                track.FadingOut = true;
                track.Steady = false;
                track.Phase = 0.0f;
                track.FadeDuration = fadeSeconds;
            }
        }
        m_Current = AssetHandle<AudioClip>{};
    }

    void MusicDirector::SetGain(const f32 gain)
    {
        m_Gain = std::max(gain, 0.0f);
    }

    void MusicDirector::Advance(const f32 delta)
    {
        for (usize i = 0; i < m_Tracks.size();)
        {
            Track& track = m_Tracks[i];
            if (!track.Steady)
            {
                track.Phase = track.FadeDuration > 0.0f
                                  ? std::min(track.Phase + delta / track.FadeDuration, 1.0f)
                                  : 1.0f;
            }
            if (track.FadingOut && track.Phase >= 1.0f)
            {
                m_Engine.StopVoice(track.Voice);
                m_Tracks.erase(m_Tracks.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (!track.FadingOut && track.Phase >= 1.0f)
            {
                track.Steady = true;
            }
            Apply(track);
            ++i;
        }
    }

    vector<MusicDirector::VoiceState> MusicDirector::GetVoiceStates() const
    {
        vector<VoiceState> states;
        states.reserve(m_Tracks.size());
        for (const Track& track : m_Tracks)
        {
            states.push_back(VoiceState{.Voice = track.Voice,
                                        .Clip = track.Clip,
                                        .Gain = TrackGain(track),
                                        .FadingOut = track.FadingOut});
        }
        return states;
    }
}
