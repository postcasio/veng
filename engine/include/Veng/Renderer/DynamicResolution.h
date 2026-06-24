#pragma once

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
        f32 MinScale = 0.5f;
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
}
