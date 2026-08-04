#include "Reverb.h"

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

        usize ScaledLength(int base, u32 sampleRate, int spread)
        {
            const f32 scaled =
                static_cast<f32>(base + spread) * static_cast<f32>(sampleRate) / kReferenceRate;
            return std::max<usize>(1, static_cast<usize>(scaled));
        }
    }

    void Reverb::Prepare(u32 sampleRate)
    {
        m_SampleRate = sampleRate;
        for (usize channel = 0; channel < 2; ++channel)
        {
            const int spread = channel == 0 ? 0 : kStereoSpread;
            for (usize c = 0; c < kCombCount; ++c)
            {
                m_Combs[channel][c].Buffer.assign(ScaledLength(kCombTuning[c], sampleRate, spread),
                                                  0.0f);
                m_Combs[channel][c].FilterStore = 0.0f;
                m_Combs[channel][c].Index = 0;
            }
            for (usize a = 0; a < kAllpassCount; ++a)
            {
                m_Allpasses[channel][a].Buffer.assign(
                    ScaledLength(kAllpassTuning[a], sampleRate, spread), 0.0f);
                m_Allpasses[channel][a].Index = 0;
            }
        }
    }

    f32 Reverb::ProcessComb(Comb& comb, f32 input, f32 feedback, f32 damp)
    {
        const f32 output = comb.Buffer[comb.Index];
        comb.FilterStore = (output * (1.0f - damp)) + (comb.FilterStore * damp);
        comb.Buffer[comb.Index] = input + (comb.FilterStore * feedback);
        comb.Index = (comb.Index + 1) % comb.Buffer.size();
        return output;
    }

    f32 Reverb::ProcessAllpass(Allpass& allpass, f32 input)
    {
        const f32 buffered = allpass.Buffer[allpass.Index];
        const f32 output = -input + buffered;
        allpass.Buffer[allpass.Index] = input + (buffered * 0.5f);
        allpass.Index = (allpass.Index + 1) % allpass.Buffer.size();
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

        for (u32 i = 0; i < frames; ++i)
        {
            const f32 input = send[i] * kFixedGain;
            f32 channelOut[2] = {0.0f, 0.0f};
            for (usize channel = 0; channel < 2; ++channel)
            {
                f32 acc = 0.0f;
                for (auto& comb : m_Combs[channel])
                {
                    acc += ProcessComb(comb, input, feedback, damp);
                }
                for (auto& allpass : m_Allpasses[channel])
                {
                    acc = ProcessAllpass(allpass, acc);
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
