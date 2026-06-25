#pragma once

#include <array>
#include <cmath>

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief Tuning for the adaptive render-resolution controller.
    ///
    /// Drives a viewport's RenderScale from the measured GPU frame time toward a frame budget:
    /// over budget the scale falls (the image is rendered below its region and upscaled), with
    /// headroom it rises back. The controller is pure (ComputeDynamicResolutionScale); these are
    /// its only inputs besides the current scale and the measurement.
    struct DynamicResolutionSettings
    {
        /// @brief The GPU frame-time budget in milliseconds the controller targets.
        ///
        /// Derived from a target frame rate (e.g. 1000/60 for 60 Hz). The scale converges so the
        /// measured GPU frame time sits at or just under this.
        f32 TargetFrameTimeMs = 1000.0f / 60.0f;
        /// @brief Lower bound on the render scale; the controller never drops below it.
        ///
        /// Sits strictly below the outer-loop floor tier (AllocationTierSettings default 0.5),
        /// so at the floor tier the per-frame sub-rect retains headroom to absorb a spike
        /// rather than the two floors coinciding.
        f32 MinScale = 0.25f;
        /// @brief Upper bound on the render scale; the controller never raises above it.
        f32 MaxScale = 1.0f;
        /// @brief Deadband below the budget, as a fraction of it, before the scale is raised.
        ///
        /// A measurement in [Target·(1 - Headroom), Target] holds the scale, so the controller
        /// settles instead of oscillating around the budget. Scaling down still reacts at any
        /// overshoot above the budget.
        f32 Headroom = 0.1f;
        /// @brief Maximum change applied to the scale in one update, in scale units.
        ///
        /// Rate-limits the response so a single frame spike cannot collapse (or inflate) the
        /// resolution in one step; the scale eases toward the ideal over several frames.
        f32 MaxStep = 0.1f;
    };

    /// @brief Maps a measured GPU frame time to the next render scale.
    ///
    /// Pure and device-free. GPU cost is taken proportional to the rendered pixel count (scale²),
    /// so the scale that would have hit the budget is currentScale·√(target / measured); the
    /// result eases toward that ideal, capped by DynamicResolutionSettings::MaxStep and clamped to
    /// [MinScale, MaxScale]. A measurement within the headroom deadband below the budget holds the
    /// scale; a non-positive measurement (no timing this frame) holds it too.
    /// @param currentScale     The viewport's current render scale (> 0).
    /// @param gpuFrameTimeMs   The most recent measured GPU frame time in ms; <= 0 means "no data".
    /// @param settings         The controller tuning.
    /// @return The render scale to apply next, in [settings.MinScale, settings.MaxScale].
    [[nodiscard]] inline f32
    ComputeDynamicResolutionScale(f32 currentScale, f32 gpuFrameTimeMs,
                                  const DynamicResolutionSettings& settings)
    {
        const f32 minScale = settings.MinScale;
        const f32 maxScale = settings.MaxScale;

        // No measurement this frame: hold the current scale (clamped into range).
        if (gpuFrameTimeMs <= 0.0f)
        {
            return glm::clamp(currentScale, minScale, maxScale);
        }

        const f32 target = settings.TargetFrameTimeMs;
        // Cost ~ rendered pixels ~ scale², so the scale that hits the budget is the current scale
        // times sqrt(target / measured).
        const f32 ideal = currentScale * std::sqrt(target / gpuFrameTimeMs);

        f32 next = currentScale;
        if (gpuFrameTimeMs > target)
        {
            // Over budget: ease down toward the ideal, capped per update.
            next = currentScale - glm::min(currentScale - ideal, settings.MaxStep);
        }
        else if (gpuFrameTimeMs < target * (1.0f - settings.Headroom))
        {
            // Comfortably under budget: ease up toward the ideal, capped per update.
            next = currentScale + glm::min(ideal - currentScale, settings.MaxStep);
        }

        return glm::clamp(next, minScale, maxScale);
    }

    /// @brief Tuning for the outer-loop allocation-tier controller.
    ///
    /// The inner loop (ComputeDynamicResolutionScale) drives a per-frame sub-rect scale from GPU
    /// frame time. The outer loop folds that scale into a long EMA and decides which quantized
    /// allocation tier the render targets should be sized at, moving across tiers only through a
    /// hysteresis band and only after a sustained dwell — so the expensive reallocation changes at
    /// most once every few seconds and never oscillates. These are its only inputs besides the
    /// caller-held AllocationTierState, the current sub-rect scale, and the frame delta.
    struct AllocationTierSettings
    {
        /// @brief The allowed allocation scales, descending; the controller's state is an index into this.
        ///
        /// The first entry is the baseline (full allocation), the last is the floor. A fixed-size
        /// array keeps the settings trivially copyable; few tiers means few possible transitions.
        std::array<f32, 3> Tiers = {1.0f, 0.75f, 0.5f};
        /// @brief Half-life in seconds of the long EMA the sub-rect scale folds into.
        ///
        /// Converted to a per-update alpha from the frame delta. Deliberately an order of magnitude
        /// longer than the inner loop's settling time, so the EMA reads the inner loop's settled
        /// operating point rather than its transient.
        f32 EmaHalfLifeSeconds = 2.0f;
        /// @brief Margin above the next-smaller tier the sustained scale must fall below to step down.
        ///
        /// A step down fires when SustainedScale < Tiers[next] + DownMargin: the sub-rect is
        /// consistently using almost all of the smaller tier, so the current larger allocation is
        /// wasted.
        f32 DownMargin = 0.03f;
        /// @brief Margin above the current tier the sustained scale must rise above to step up.
        ///
        /// The gap between the down-threshold and this up-threshold is the dead band a scene parked
        /// near a boundary lives in without flipping.
        f32 UpHysteresis = 0.12f;
        /// @brief Seconds the step-down condition must hold continuously before the tier shrinks.
        ///
        /// Quicker than GrowDwellSeconds: the sub-rect is already absorbing the load, so deferring
        /// the reclaim is cheap and the hitch is paid only when the load is clearly sustained.
        f32 ShrinkDwellSeconds = 1.5f;
        /// @brief Seconds the step-up condition must hold continuously before the tier grows.
        ///
        /// Slow on purpose: eager growth is what thrashes the expensive reallocation.
        f32 GrowDwellSeconds = 5.0f;
    };

    /// @brief Caller-held state for the outer-loop allocation-tier controller.
    ///
    /// Threaded through StepAllocationTier each frame; the controller mutates only this. Default-
    /// constructed it seeds to the baseline tier (index 0, scale 1.0).
    struct AllocationTierState
    {
        /// @brief The long EMA of the sub-rect scale, seeded to the baseline tier (1.0).
        f32 SustainedScale = 1.0f;
        /// @brief The current allocation tier index, seeded to 0 (baseline / full allocation).
        u32 TierIndex = 0;
        /// @brief Seconds the step-down condition has held continuously; reset when it lapses.
        f32 ShrinkTimer = 0.0f;
        /// @brief Seconds the step-up condition has held continuously; reset when it lapses.
        f32 GrowTimer = 0.0f;
    };

    /// @brief Folds the current sub-rect scale into the allocation tier and returns the chosen tier index.
    ///
    /// Pure, clock-free, and device-free: it mutates only the passed-in state and takes its time
    /// delta, so a unit test can drive a synthetic scale trace deterministically. Each call folds
    /// currentRenderScale into the long EMA, advances the asymmetric dwell timers against the
    /// down/up threshold conditions, and moves TierIndex by at most one step when a timer crosses
    /// its dwell. A step down additionally requires the instantaneous currentRenderScale to already
    /// fit the smaller tier (currentRenderScale <= Tiers[next]), so the sub-rect fits inside the
    /// allocation the moment it shrinks and the rendered pixel count is preserved across the resize.
    /// A non-positive deltaSeconds (no frame this tick) holds the state unchanged.
    /// @param state             The caller-held controller state, mutated in place.
    /// @param currentRenderScale The inner loop's current sub-rect scale this frame.
    /// @param deltaSeconds      Seconds since the last update; <= 0 holds the state.
    /// @param settings          The controller tuning.
    /// @return The resulting allocation tier index, in [0, Tiers.size() - 1].
    [[nodiscard]] inline u32 StepAllocationTier(AllocationTierState& state, f32 currentRenderScale,
                                                f32 deltaSeconds,
                                                const AllocationTierSettings& settings)
    {
        const u32 tierCount = static_cast<u32>(settings.Tiers.size());
        state.TierIndex = glm::clamp(state.TierIndex, 0u, tierCount - 1u);

        // No frame this tick: hold every piece of state.
        if (deltaSeconds <= 0.0f)
        {
            return state.TierIndex;
        }

        // Fold the instantaneous scale into the long EMA. The per-update alpha converts the
        // half-life to this frame's weight: alpha = 1 - 2^(-dt / halfLife).
        const f32 halfLife = glm::max(settings.EmaHalfLifeSeconds, 1e-6f);
        const f32 alpha = 1.0f - std::exp2(-deltaSeconds / halfLife);
        state.SustainedScale += alpha * (currentRenderScale - state.SustainedScale);

        const u32 current = state.TierIndex;

        // Step-down condition: a next-smaller tier exists, the sustained scale has fallen into its
        // margin band, and the instantaneous sub-rect already fits the smaller tier.
        bool downActive = false;
        if (current + 1u < tierCount)
        {
            const f32 nextTier = settings.Tiers[current + 1u];
            downActive = state.SustainedScale < nextTier + settings.DownMargin &&
                         currentRenderScale <= nextTier;
        }

        // Step-up condition: a larger tier exists and the sustained scale has risen past the
        // current tier plus the hysteresis gap.
        bool upActive = false;
        if (current > 0u)
        {
            upActive = state.SustainedScale > settings.Tiers[current] + settings.UpHysteresis;
        }

        // Advance each dwell timer while its condition holds; reset it the instant the condition
        // lapses, so a transient never accumulates toward a tier move.
        state.ShrinkTimer = downActive ? state.ShrinkTimer + deltaSeconds : 0.0f;
        state.GrowTimer = upActive ? state.GrowTimer + deltaSeconds : 0.0f;

        // At most one tier step per call. Down is checked first; the two conditions are mutually
        // exclusive (they straddle a hysteresis gap), so the order is not load-bearing.
        if (downActive && state.ShrinkTimer >= settings.ShrinkDwellSeconds)
        {
            state.TierIndex = current + 1u;
            state.ShrinkTimer = 0.0f;
        }
        else if (upActive && state.GrowTimer >= settings.GrowDwellSeconds)
        {
            state.TierIndex = current - 1u;
            state.GrowTimer = 0.0f;
        }

        return state.TierIndex;
    }
}
