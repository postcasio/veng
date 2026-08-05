#include <Veng/Audio/Reverb.h>

#include <algorithm>
#include <cmath>

namespace Veng::Audio
{
    namespace
    {
        // The classic Freeverb delay-line lengths, in samples at 44.1 kHz; scaled to the output
        // rate in Prepare. The right channel adds a fixed stereo spread so the two banks decorrelate.
        constexpr int kCombTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        constexpr int kAllpassTuning[4] = {556, 441, 341, 225};
        constexpr int kStereoSpread = 23;
        constexpr f32 kReferenceRate = 44100.0f;
        constexpr f32 kFixedGain = 0.015f;

        // High-quality comb-tap modulation: a slow LFO wobbles each tap by this fraction of its base
        // length, so the tail does not ring on fixed comb frequencies. Each comb runs at a slightly
        // different rate and phase so the banks do not modulate in lockstep.
        constexpr f32 kCombModFraction = 0.03f;
        constexpr f32 kCombModBaseHz = 0.7f;
        constexpr f32 kCombModRateSpread = 0.11f;

        u32 ScaledLength(int base, u32 sampleRate, int spread)
        {
            const f32 scaled =
                static_cast<f32>(base + spread) * static_cast<f32>(sampleRate) / kReferenceRate;
            return std::max<u32>(1, static_cast<u32>(scaled));
        }
    }

    usize Reverb::CombCountFor(ReverbQuality quality)
    {
        // Low halves the bank for a cheap send; Standard and High run the full classic eight combs.
        return quality == ReverbQuality::Low ? 4 : kMaxCombCount;
    }

    usize Reverb::AllpassCountFor(ReverbQuality quality)
    {
        return quality == ReverbQuality::Low ? 2 : kMaxAllpassCount;
    }

    void Reverb::Prepare(u32 sampleRate)
    {
        m_SampleRate = sampleRate;
        for (usize channel = 0; channel < 2; ++channel)
        {
            const int spread = channel == 0 ? 0 : kStereoSpread;
            for (usize c = 0; c < kMaxCombCount; ++c)
            {
                Comb& comb = m_Combs[channel][c];
                comb.Length = ScaledLength(kCombTuning[c], sampleRate, spread);
                comb.ModDepth = static_cast<f32>(comb.Length) * kCombModFraction;
                // Headroom for the modulated tap and the interpolation's straddling sample.
                const u32 capacity = comb.Length + static_cast<u32>(std::ceil(comb.ModDepth)) + 2;
                comb.Line.Prepare(capacity);
                comb.FilterStore = 0.0f;
                comb.Mod.SetFrequency(kCombModBaseHz + static_cast<f32>(c) * kCombModRateSpread,
                                      sampleRate);
                comb.Mod.SetPhase(static_cast<f32>(c) / static_cast<f32>(kMaxCombCount));
            }
            for (usize a = 0; a < kMaxAllpassCount; ++a)
            {
                Allpass& allpass = m_Allpasses[channel][a];
                allpass.Length = ScaledLength(kAllpassTuning[a], sampleRate, spread);
                allpass.Line.Prepare(allpass.Length);
            }
        }
    }

    f32 Reverb::ProcessComb(Comb& comb, f32 input, f32 feedback, f32 damp, bool modulate)
    {
        f32 output = 0.0f;
        if (modulate)
        {
            const f32 delay = static_cast<f32>(comb.Length) + comb.Mod.Tick() * comb.ModDepth;
            output = comb.Line.ReadInterpolated(delay);
        }
        else
        {
            output = comb.Line.Read(comb.Length);
        }
        comb.FilterStore = (output * (1.0f - damp)) + (comb.FilterStore * damp);
        comb.Line.Write(input + (comb.FilterStore * feedback));
        return output;
    }

    f32 Reverb::ProcessAllpass(Allpass& allpass, f32 input)
    {
        const f32 buffered = allpass.Line.Read(allpass.Length);
        const f32 output = -input + buffered;
        allpass.Line.Write(input + (buffered * 0.5f));
        return output;
    }

    void Reverb::ProcessBlock(const f32* send, f32* wetL, f32* wetR, u32 frames,
                              const ReverbParams& params)
    {
        // Map the 0..1 room-size to a Freeverb feedback and clamp damping. Width folds the two
        // banks toward mono at 0 and holds them fully separated at 1.
        const f32 feedback = 0.7f + (std::clamp(params.RoomSize, 0.0f, 1.0f) * 0.28f);
        const f32 damp = std::clamp(params.Damping, 0.0f, 1.0f) * 0.4f;
        const f32 width = std::clamp(params.Width, 0.0f, 1.0f);
        const usize combCount = CombCountFor(params.Quality);
        const usize allpassCount = AllpassCountFor(params.Quality);
        const bool modulate = params.Quality == ReverbQuality::High;

        for (u32 i = 0; i < frames; ++i)
        {
            const f32 input = send[i] * kFixedGain;
            f32 channelOut[2] = {0.0f, 0.0f};
            for (usize channel = 0; channel < 2; ++channel)
            {
                f32 acc = 0.0f;
                for (usize c = 0; c < combCount; ++c)
                {
                    acc += ProcessComb(m_Combs[channel][c], input, feedback, damp, modulate);
                }
                for (usize a = 0; a < allpassCount; ++a)
                {
                    acc = ProcessAllpass(m_Allpasses[channel][a], acc);
                }
                channelOut[channel] = acc;
            }
            const f32 wet1 = 0.5f + (width * 0.5f);
            const f32 wet2 = (1.0f - width) * 0.5f;
            wetL[i] = (channelOut[0] * wet1) + (channelOut[1] * wet2);
            wetR[i] = (channelOut[1] * wet1) + (channelOut[0] * wet2);
        }
    }
}
