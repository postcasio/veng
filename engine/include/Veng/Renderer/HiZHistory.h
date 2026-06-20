#pragma once

#include <Veng/Veng.h>

/// @brief Pure, device-free hi-Z history-validity decision.
///
/// The temporal hi-Z occlusion test reads the previous frame's pyramid and
/// view-projection. This decides whether that history is trustworthy this frame, or
/// whether to skip occlusion (frustum-only) — drawing more is free, a missed
/// invalidation risks a one-frame false-cull, so every threshold is biased toward
/// invalidation. glm-only, no Context, so it unit-tests without an ICD.
namespace Veng::Renderer
{
    /// @brief Thresholds controlling when the previous-frame pyramid is invalidated.
    ///
    /// Tunable; the safe direction is documented per field. Lowering a threshold
    /// invalidates more readily — more frustum-only frames, never a stale false-cull.
    struct HiZHistorySettings
    {
        /// @brief Camera-translation fraction of the scene-bound diagonal that invalidates.
        ///
        /// A move farther than this fraction of the scene's diagonal is treated as a
        /// teleport-sized cut: last frame's depth is unrelated, so occlusion is skipped.
        /// Lower = invalidate on smaller moves (more conservative).
        f32 TranslationFraction = 0.02f;

        /// @brief Forward-axis rotation, in radians, that invalidates.
        ///
        /// A rotation larger than this between frames reorients the view enough that
        /// last frame's depth no longer screen-aligns. Lower = invalidate on smaller
        /// rotations (more conservative). Default ~10 degrees.
        f32 RotationRadians = 0.1745329f;
    };

    /// @brief A frame's camera state, captured for next frame's history comparison.
    ///
    /// World-space position, the normalized forward axis, and the projection matrix —
    /// the inputs the validity decision compares against last frame's.
    struct HiZHistoryState
    {
        /// @brief Camera world-space position.
        vec3 CameraPosition{0.0f};
        /// @brief Normalized camera forward axis (world space).
        vec3 CameraForward{0.0f, 0.0f, -1.0f};
        /// @brief The frame's projection matrix; any change invalidates.
        mat4 Projection{1.0f};
    };

    /// @brief Decides whether the previous-frame pyramid is valid to occlusion-test against.
    ///
    /// Invalidates when the camera translated more than a fraction of the scene-bound
    /// diagonal, the forward axis rotated past the angle threshold, or the projection
    /// changed at all (FOV / near-far / aspect). All thresholds fail toward
    /// invalidation; a caller pairs this with frame-0 and post-resize invalidation,
    /// which it cannot see.
    /// @param previous       Last frame's captured camera state.
    /// @param current        This frame's camera state.
    /// @param sceneDiagonal  Length of the world-space scene-bound diagonal (>= 0).
    /// @param settings       Translation / rotation thresholds.
    /// @return True if last frame's depth is trustworthy this frame.
    [[nodiscard]] bool IsHiZHistoryValid(const HiZHistoryState& previous,
                                         const HiZHistoryState& current, f32 sceneDiagonal,
                                         const HiZHistorySettings& settings);
}
