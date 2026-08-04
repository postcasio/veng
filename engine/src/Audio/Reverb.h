#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/Voice.h>

namespace Veng::Audio
{
    /// @brief A first-party Schröder/Freeverb-style master reverb node.
    ///
    /// miniaudio carries no reverb, so the master reverb send is a small first-party node: a bank
    /// of feedback-comb and all-pass filters. Its delay lines are allocated once in Prepare (never
    /// on the real-time thread); ProcessBlock is allocation-free and reads a mono send, producing a
    /// stereo wet signal to add over the master mix.
    class Reverb
    {
    public:
        /// @brief Allocates the delay lines for a sample rate; call before any ProcessBlock.
        /// @param sampleRate Output sample rate in Hz.
        void Prepare(u32 sampleRate);

        /// @brief Processes a mono send block into a stereo wet block.
        /// @param send   Mono send samples (length @p frames).
        /// @param wetL   Left wet output (length @p frames), overwritten.
        /// @param wetR   Right wet output (length @p frames), overwritten.
        /// @param frames Number of sample frames.
        /// @param params Reverb parameters (room size, damping, width; Wet is applied by the caller).
        void ProcessBlock(const f32* send, f32* wetL, f32* wetR, u32 frames,
                          const ReverbParams& params);

    private:
        /// @brief The number of feedback-comb filters per channel.
        static constexpr usize kCombCount = 8;
        /// @brief The number of all-pass filters per channel.
        static constexpr usize kAllpassCount = 4;

        /// @brief A single damped feedback-comb filter.
        struct Comb
        {
            /// @brief The delay line.
            vector<f32> Buffer;
            /// @brief The one-pole low-pass state in the feedback path.
            f32 FilterStore = 0.0f;
            /// @brief The write/read cursor.
            usize Index = 0;
        };

        /// @brief A single all-pass filter.
        struct Allpass
        {
            /// @brief The delay line.
            vector<f32> Buffer;
            /// @brief The write/read cursor.
            usize Index = 0;
        };

        /// @brief Runs one comb filter for a sample.
        static f32 ProcessComb(Comb& comb, f32 input, f32 feedback, f32 damp);
        /// @brief Runs one all-pass filter for a sample.
        static f32 ProcessAllpass(Allpass& allpass, f32 input);

        /// @brief Per-channel comb banks (index 0 left, 1 right).
        Comb m_Combs[2][kCombCount];
        /// @brief Per-channel all-pass banks (index 0 left, 1 right).
        Allpass m_Allpasses[2][kAllpassCount];
        /// @brief The sample rate Prepare was called with.
        u32 m_SampleRate = 0;
    };
}
