#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/Voice.h>
#include <Veng/Audio/Dsp/DelayLine.h>
#include <Veng/Audio/Dsp/Lfo.h>

namespace Veng::Audio
{
    /// @brief A public, embeddable Schröder/Freeverb-style reverb effect.
    ///
    /// A bank of feedback-comb and all-pass filters — the same node the mixer's master send is built
    /// on — usable two ways: as the mixer's master reverb, and embedded inside an @ref IAudioGenerator
    /// to wet only the sub-mix a consumer chooses. It is expressed on @ref Dsp::DelayLine (the comb and
    /// all-pass banks) and @ref Dsp::Lfo (the optional High-quality tap modulation).
    ///
    /// The lifecycle is the embeddable contract: @ref Prepare allocates the delay lines once, off the
    /// real-time thread; @ref ProcessBlock reads a mono send and writes a stereo wet pair with **no
    /// allocation and no lock** at every quality. @ref ReverbParams::Quality selects the density/CPU
    /// trade — the banks are sized to the maximum in @ref Prepare and only the active count is
    /// processed, so switching quality between blocks never allocates.
    class Reverb
    {
    public:
        /// @brief Allocates the maximum comb / all-pass banks for a sample rate.
        ///
        /// The one allocating call; run it off the real-time thread before any @ref ProcessBlock. It
        /// sizes every bank to the full High/Standard maximum (with modulation headroom) and clears
        /// all state, so a later ProcessBlock at any quality is allocation-free.
        /// @param sampleRate  Output sample rate in Hz.
        void Prepare(u32 sampleRate);

        /// @brief Processes a mono send block into a stereo wet block.
        ///
        /// Allocation-free and lock-free — safe on the real-time mixing thread.
        /// @param send    Mono send samples (length @p frames).
        /// @param wetL    Left wet output (length @p frames), overwritten.
        /// @param wetR    Right wet output (length @p frames), overwritten.
        /// @param frames  Number of sample frames.
        /// @param params  Reverb parameters (room size, damping, width, quality; Wet is applied by the
        ///                caller).
        void ProcessBlock(const f32* send, f32* wetL, f32* wetR, u32 frames,
                          const ReverbParams& params);

    private:
        /// @brief The maximum number of feedback-comb filters per channel (the High/Standard bank).
        static constexpr usize kMaxCombCount = 8;
        /// @brief The maximum number of all-pass filters per channel (the High/Standard bank).
        static constexpr usize kMaxAllpassCount = 4;

        /// @brief A single damped feedback-comb filter with an optional modulated tap.
        struct Comb
        {
            /// @brief The delay-line storage (sized to the base length plus modulation headroom).
            Dsp::DelayLine Line;
            /// @brief The one-pole low-pass state in the feedback path.
            f32 FilterStore = 0.0f;
            /// @brief The base tap length in samples (the scaled Freeverb tuning).
            u32 Length = 0;
            /// @brief The slow modulator driving the tap at High quality.
            Dsp::Lfo Mod;
            /// @brief The peak tap excursion in samples applied at High quality.
            f32 ModDepth = 0.0f;
        };

        /// @brief A single all-pass filter.
        struct Allpass
        {
            /// @brief The delay-line storage (sized to the tap length).
            Dsp::DelayLine Line;
            /// @brief The tap length in samples (the scaled Freeverb tuning).
            u32 Length = 0;
        };

        /// @brief Runs one comb filter for a sample, modulating the tap when @p modulate.
        static f32 ProcessComb(Comb& comb, f32 input, f32 feedback, f32 damp, bool modulate);
        /// @brief Runs one all-pass filter for a sample.
        static f32 ProcessAllpass(Allpass& allpass, f32 input);

        /// @brief The number of active combs per channel for a quality tier.
        static usize CombCountFor(ReverbQuality quality);
        /// @brief The number of active all-pass filters per channel for a quality tier.
        static usize AllpassCountFor(ReverbQuality quality);

        /// @brief Per-channel comb banks (index 0 left, 1 right), sized to the maximum in Prepare.
        Comb m_Combs[2][kMaxCombCount];
        /// @brief Per-channel all-pass banks (index 0 left, 1 right), sized to the maximum in Prepare.
        Allpass m_Allpasses[2][kMaxAllpassCount];
        /// @brief The sample rate Prepare was called with.
        u32 m_SampleRate = 0;
    };
}
