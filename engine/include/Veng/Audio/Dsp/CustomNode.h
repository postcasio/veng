#pragma once

#include <Veng/Veng.h>

#include <utility>

namespace Veng::Audio::Dsp
{
    /// @brief A generator node whose samples come from a caller-supplied callable.
    ///
    /// The escape hatch for a sound that maps onto no standard primitive: a bespoke fill routine
    /// (a Shepard–Risset glissando, a wavetable scan, an FM stack) plugged in at the same level as
    /// @ref Oscillator. The callable is bound once at construction — a @c function is not POD and may
    /// allocate when constructed, so it can never cross a @c GeneratorParams block — while its @e live
    /// knobs still flow through that block: the generator latches its params, stores them in plain
    /// members, and the callable (which closed over a pointer to that state) reads them. So the
    /// @e code is fixed at build time, never the parameters.
    ///
    /// @warning @ref Render runs on the real-time mixing thread. Invoking an already-constructed
    ///          @c function allocates nothing, so the call is RT-safe, but the callable's body carries
    ///          the same no-lock / no-alloc / no-engine contract as any generator's Render.
    class CustomSource
    {
    public:
        /// @brief The fill callback: writes @p frames samples at @p sampleRate. RT-contract-bound.
        using Fill = function<void(f32* out, u32 frames, u32 sampleRate)>;

        /// @brief Binds the callable off the real-time thread; it is invoked on it.
        /// @param fill  The fill callable; bound once, never reassigned on the RT thread.
        explicit CustomSource(Fill fill) : m_Fill(std::move(fill)) {}

        /// @brief Invokes the bound callable to fill a block.
        /// @param out         Destination for @p frames samples.
        /// @param frames      The number of samples to produce.
        /// @param sampleRate  The output sample rate in Hz.
        void Render(f32* out, u32 frames, u32 sampleRate) { m_Fill(out, frames, sampleRate); }

    private:
        Fill
            m_Fill; ///< @brief The bound callable; constructed once, never reassigned on the RT thread.
    };

    /// @brief A processing node that transforms a stream through a caller-supplied callable.
    ///
    /// The processing counterpart to @ref CustomSource: a bespoke in-place transform (a waveshaper, a
    /// bit-crusher, a custom comb) at the same level as @ref Filter. The same binding rule holds — the
    /// callable is bound once at construction, its live knobs reach it through the pointer it captured
    /// — and the same real-time contract binds its body.
    ///
    /// @warning @ref Apply runs on the real-time mixing thread; the callable's body must not lock,
    ///          allocate, or call any engine API.
    class CustomFilter
    {
    public:
        /// @brief The process callback: rewrites @p frames samples in place. RT-contract-bound.
        using Process = function<void(f32* samples, u32 frames, u32 sampleRate)>;

        /// @brief Binds the callable off the real-time thread; it is invoked on it.
        /// @param process  The process callable; bound once, never reassigned on the RT thread.
        explicit CustomFilter(Process process) : m_Process(std::move(process)) {}

        /// @brief Invokes the bound callable over a block.
        /// @param samples     The samples to transform in place, @p frames long.
        /// @param frames      The number of samples in the block.
        /// @param sampleRate  The sample rate in Hz.
        void Apply(f32* samples, u32 frames, u32 sampleRate)
        {
            m_Process(samples, frames, sampleRate);
        }

    private:
        Process
            m_Process; ///< @brief The bound callable; constructed once, never reassigned on the RT thread.
    };
}
