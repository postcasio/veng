#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/MeshLod.h>

namespace Veng
{
    class Mesh;
    struct VisibleMesh;
}

namespace Veng::Renderer
{
    /// @brief The previous-level value meaning "no level was drawn last frame".
    inline constexpr u32 NoPreviousLod = ~0u;

    /// @brief What level-of-detail selection needs of one view, derived once per view.
    ///
    /// A mesh's projected screen size is its level-0 bounding sphere's diameter as a fraction of
    /// the view's height: radius × ProjectionScale / distance under a perspective projection, and
    /// radius × ProjectionScale under an orthographic one. The distance is to the eye, not the view
    /// depth, so turning the view in place moves no mesh across a threshold.
    struct VE_API LodView
    {
        /// @brief The eye position (perspective only).
        vec3 Eye{0.0f};
        /// @brief The projection's vertical scale: 1 / tan(fovY / 2), or 2 / height when orthographic.
        f32 ProjectionScale = 1.0f;
        /// @brief Whether the projection is a perspective one.
        bool Perspective = true;
        /// @brief The bias's multiplier on every screen size: 2^-bias.
        f32 ScreenScale = 1.0f;

        /// @brief Derives a view from its world → clip matrix.
        /// @param viewProj The view-projection the view rasterizes through (unjittered).
        /// @param bias     Levels of bias: each +1 halves every screen size, coarsening selection.
        [[nodiscard]] static LodView FromViewProjection(const mat4& viewProj, f32 bias);
    };

    /// @brief The projected screen size of a sphere in a view, bias applied.
    ///
    /// A sphere containing the eye projects as unboundedly large.
    /// @param view   The view.
    /// @param center The sphere's world-space centre.
    /// @param radius The sphere's world-space radius.
    /// @return The diameter as a fraction of the view height, times the view's ScreenScale.
    [[nodiscard]] VE_API f32 ProjectedScreenSize(const LodView& view, const vec3& center,
                                                 f32 radius);

    /// @brief Selects a level from a screen size against descending thresholds, with hysteresis.
    ///
    /// Level i (i ≥ 1) is wanted while the screen size is below thresholds[i - 1]. Each threshold
    /// is widened into a band of ±hysteresis (relative): a mesh coarsens across one only once it is
    /// below the band and refines across it only once above, so a screen size resting inside a band
    /// keeps the level it had. With no previous level the band's middle decides.
    /// @param thresholds Levels 1.. switch thresholds, descending (Mesh::GetLodScreenSizes).
    /// @param screenSize The mesh's projected screen size this frame.
    /// @param previous   The level drawn last frame, or NoPreviousLod.
    /// @param hysteresis The band's half-width as a fraction of the threshold, in [0, 1).
    /// @return The level, in [0, thresholds.size()].
    [[nodiscard]] VE_API u32 SelectLod(std::span<const f32> thresholds, f32 screenSize,
                                       u32 previous, f32 hysteresis);

    /// @brief How a view chooses levels: its forced level and its hysteresis band.
    struct LodPolicy
    {
        /// @brief The view-wide forced level, or AutomaticLod to select by screen size.
        i32 Forced = AutomaticLod;
        /// @brief The hysteresis band's half-width (see SelectLod); 0 for a stateless view.
        f32 Hysteresis = 0.0f;
    };

    /// @brief Resolves the level one visible mesh draws at in one view.
    ///
    /// A per-entity forced level (VisibleMesh::ForcedLod) wins, then the view's, then selection by
    /// screen size; a forced level clamps to the mesh's coarsest. A single-level mesh is level 0.
    /// @param item     The visible mesh.
    /// @param view     The view.
    /// @param policy   The view's forced level and hysteresis.
    /// @param previous The level this mesh drew at in this view last frame, or NoPreviousLod.
    /// @return The level, below item.Mesh->GetLodCount().
    [[nodiscard]] VE_API u32 ResolveMeshLod(const VisibleMesh& item, const LodView& view,
                                            const LodPolicy& policy, u32 previous);
}
