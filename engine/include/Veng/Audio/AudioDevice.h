#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/Voice.h>

#include <span>

namespace Veng::Audio
{
    class AudioEngine;

    /// @brief Which output backend a device uses.
    enum class AudioBackend : u8
    {
        /// @brief Try the platform's hardware device; fall to Null if none initializes.
        Auto = 0,
        /// @brief A silent device: every call succeeds and voices are tracked, but nothing is
        ///        emitted. Chosen automatically when no hardware inits, or forced for
        ///        headless / CI / cook runs.
        Null,
    };

    /// @brief Construction descriptor for an AudioDevice.
    struct AudioDeviceInfo
    {
        /// @brief The requested backend; Auto falls to Null when no hardware initializes.
        AudioBackend Backend = AudioBackend::Auto;
        /// @brief Requested output sample rate in Hz; 0 asks the backend for its default.
        u32 SampleRate = 48000;
        /// @brief Requested output channel count (the mixer targets 2).
        u32 Channels = 2;
        /// @brief Whether to emit the diagnostic self-test tone at init.
        bool RunSelfTest = false;
    };

    /// @brief The self-test tone: a bounded, finite mono buffer proving the mix path is live.
    struct SelfTestTone
    {
        /// @brief Interleaved (mono) samples.
        vector<f32> Samples;
        /// @brief Number of sample frames.
        u64 FrameCount = 0;
        /// @brief Channel count (1).
        u32 Channels = 1;
        /// @brief Sample rate in Hz.
        u32 SampleRate = 0;
        /// @brief Peak absolute sample value.
        f32 Peak = 0.0f;
    };

    /// @brief Builds the diagnostic self-test tone for a sample rate (a short faded sine burst).
    /// @param sampleRate Output sample rate in Hz.
    /// @return A finite mono tone whose sample count and peak are deterministic.
    SelfTestTone GenerateSelfTestTone(u32 sampleRate);

    /// @brief The audio device: the output backend and the real-time mixing path.
    ///
    /// Constructed once and held for the whole run, mirroring Renderer::Context. It owns the
    /// miniaudio device (or a null stand-in), the triple-buffered voice snapshot the mixing thread
    /// reads, the master reverb, and the reclamation and retired-voice bridges. The mixer-facing
    /// API — buses and voices — is reached through GetEngine().
    ///
    /// @warning The real-time callback thread reads only the immutable snapshot and mixes. It
    ///          touches no engine API and no engine state, which is the whole basis for the audio
    ///          subsystem being the one sanctioned exception to veng's single-thread rule.
    class AudioDevice
    {
    public:
        /// @brief Frames of one block the callback-side tap ring carries.
        ///
        /// A callback longer than this is published as several blocks; a shorter one fills a block
        /// partially. The ring's payload is a fixed POD block because the backend reuses its output
        /// buffer the moment the callback returns.
        static constexpr u32 TapBlockFrames = 1024;

        /// @brief Channels a tap block carries, the widest output the tap supports.
        static constexpr u32 MaxTapChannels = 2;

        /// @brief Receives one mixed block of interleaved samples on the main thread.
        ///
        /// @param interleaved  GetChannels() interleaved samples per frame; valid only for the call.
        /// @param frames       Number of sample frames in @p interleaved.
        using BlockTap = function<void(std::span<const f32> interleaved, u32 frames)>;

        /// @brief Creates a device from its descriptor; never fails (falls to the null backend).
        /// @param info The construction descriptor.
        /// @return A unique-ownership device.
        static Unique<AudioDevice> Create(const AudioDeviceInfo& info);

        /// @brief Stops and joins the mixing thread, then releases every referenced resource.
        ///
        /// @warning Declared as an Application member after the asset manager and world runner so
        ///          it destructs first: the callback is joined before any clip or generator a voice
        ///          may reference is freed.
        ~AudioDevice();

        AudioDevice(const AudioDevice&) = delete;
        AudioDevice& operator=(const AudioDevice&) = delete;

        /// @brief Returns the mixer-facing API (buses and voices).
        [[nodiscard]] AudioEngine& GetEngine() const { return *m_Engine; }

        /// @brief Returns the backend actually in use (Null when hardware failed to initialize).
        [[nodiscard]] AudioBackend GetBackend() const { return m_Backend; }
        /// @brief Returns whether the device is the silent null backend.
        [[nodiscard]] bool IsNull() const { return m_Backend == AudioBackend::Null; }
        /// @brief Returns the output sample rate in Hz.
        [[nodiscard]] u32 GetSampleRate() const { return m_SampleRate; }
        /// @brief Returns the output channel count.
        [[nodiscard]] u32 GetChannels() const { return m_Channels; }

        /// @brief Drives one frame of the subsystem from the main thread.
        ///
        /// Publishes the latest voice snapshot, advances the null device's virtual playback clock
        /// (a real device advances on its own callback thread), then reaps retired voices and frees
        /// reclaimed resources whose generation has passed.
        /// While driven the pump also mixes, on the main thread, exactly the frames @p deltaSeconds
        /// names — the hitch clamp that bounds a null device's virtual clock is lifted, since a
        /// driven delta is exact rather than measured.
        /// @param deltaSeconds Frame time since the last pump, as the caller's frame clock reports it.
        void Pump(f32 deltaSeconds);

        /// @brief Stops the hardware and mixes on the main thread instead, or resumes the hardware.
        ///
        /// For a caller whose frame clock is driven rather than measured: a hardware device mixes on
        /// its own callback thread at the hardware rate, so the mix would run ahead of a frame loop
        /// that is no longer paced by real time. Driven, the device is stopped (synchronously — the
        /// callback thread is quiescent before this returns, so the main thread is the mixer's only
        /// caller and the one-thread rule holds) and each Pump mixes exactly its own frame's samples.
        /// Nothing is emitted for the span. Voices, snapshots and reclamation are unaffected: the
        /// mixer does not know which thread calls it.
        ///
        /// A null device has no hardware to stop, so this only records the mode; its Pump already
        /// mixes on the main thread. If the output device cannot be restarted on release — it went
        /// away during the driven span — the device degrades to the null backend and logs, the
        /// subsystem's device-loss policy.
        /// @param driven  True to stop the hardware and mix per pump, false to resume the hardware.
        void SetDriven(bool driven);

        /// @brief Returns whether the device is in driven mode (see SetDriven).
        [[nodiscard]] bool IsDriven() const { return m_Driven; }

        /// @brief Installs the per-block tap, or clears it with nullptr.
        ///
        /// Every mixed block reaches @p tap on the main thread, in order: directly from Pump while
        /// driven or on a null device, and through a lock-free ring the mixing thread pushes into
        /// when a hardware device is running. Replacing or clearing a tap first drains the ring into
        /// the outgoing sink, so no block is lost at a swap or a stop; installing one also resets the
        /// overrun count.
        ///
        /// Call from the main thread only. While no tap is set the mixing thread does no tap work
        /// beyond reading one atomic.
        /// @param tap  The sink, or nullptr to clear.
        /// @pre GetChannels() <= MaxTapChannels — asserted when installing a tap.
        void SetBlockTap(BlockTap tap);

        /// @brief Returns how many mixed blocks the tap ring dropped because it was full.
        ///
        /// The ring drops the *newest* block when full — the mixing thread never blocks — so a
        /// consumer that must stay aligned with the mix substitutes silence for the lost blocks.
        /// Counted since the current tap was installed.
        [[nodiscard]] u64 GetTapOverruns() const;

        /// @brief Publishes one mixed block to the tap from the mixing thread.
        ///
        /// The backend callback's side of the tap: splits @p interleaved into TapBlockFrames-sized
        /// blocks and pushes each into the ring Pump drains on the main thread. Lock-free and
        /// allocation-free, and a whole no-op while no tap is set.
        /// @param interleaved  GetChannels() interleaved samples per frame.
        /// @param frames       Number of sample frames in @p interleaved.
        void PublishTapBlock(std::span<const f32> interleaved, u32 frames);

        /// @brief Mixes one block from the latest published snapshot into an interleaved output.
        ///
        /// Latches the newest snapshot, mixes GetChannels() interleaved channels, and advances the
        /// consumed-generation counter. This is the block the backend callback drives; it is also
        /// callable directly, which is what makes the mix path unit-testable without hardware.
        /// @param output Interleaved output, at least @p frames * GetChannels() samples.
        /// @param frames Number of sample frames to mix.
        void RenderBlock(std::span<f32> output, u32 frames);

        /// @brief Returns the most recent RenderBlock output (for headless inspection and tests).
        [[nodiscard]] std::span<const f32> GetDebugMixBuffer() const;

        /// @brief Returns the generation of the snapshot the mixer has consumed up to.
        ///
        /// The reclamation handshake frees a resource only once this has passed the last generation
        /// that referenced it.
        [[nodiscard]] u64 GetConsumedSerial() const;

        /// @brief Registers the self-test tone as a one-shot voice on the master bus.
        void RunSelfTest();

        /// @brief Opaque backend handle (miniaudio device); defined in the matching .cpp.
        struct Native;
        /// @brief Returns the backend handle.
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit AudioDevice(const AudioDeviceInfo& info);

        /// @brief The mixer-facing main-thread API; constructed over this device.
        Unique<AudioEngine> m_Engine;
        /// @brief Backend state (miniaudio device, snapshot ring, RT cursors, reverb, channels).
        Unique<Native> m_Native;
        /// @brief The backend actually in use.
        AudioBackend m_Backend = AudioBackend::Null;
        /// @brief Output sample rate in Hz.
        u32 m_SampleRate = 48000;
        /// @brief Output channel count.
        u32 m_Channels = 2;
        /// @brief Whether the hardware is stopped and each Pump mixes the frame's samples.
        bool m_Driven = false;

        /// @brief Delivers one mixed block to the installed tap, if any.
        /// @param interleaved  GetChannels() interleaved samples per frame.
        /// @param frames       Number of sample frames in @p interleaved.
        void DeliverTapBlock(std::span<const f32> interleaved, u32 frames);

        /// @brief Delivers every block the mixing thread pushed into the tap ring, in order.
        void DrainTapRing();
    };
}
