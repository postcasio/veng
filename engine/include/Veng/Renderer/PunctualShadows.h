#pragma once

#include <array>
#include <optional>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>

/// @brief Pure, device-free shadow-view math for punctual lights.
///
/// Turns a spot or point light into the world→light-clip matrices a depth pass renders
/// casters with and the lighting pass samples against. glm-only — no Context, no device
/// — so it compiles and unit-tests without an ICD, beside ShadowCascades.h.
namespace Veng::Renderer
{
    /// @brief Number of cube faces, and the fixed order the point-shadow atlas tiles them.
    ///
    /// The lighting pass selects a face by the major axis of the light→fragment
    /// direction: +X, -X, +Y, -Y, +Z, -Z.
    inline constexpr u32 CubeFaceCount = 6;

    /// @brief A spot light's single perspective shadow view.
    ///
    /// ViewProj maps a world point to the light's clip space (Vulkan ZO, Y-flipped to
    /// match Camera); a fragment in the cone projects to z ∈ [0,1], xy ∈ [-1,1].
    /// Near/Far are the clip range the depth pass uses; the lighting pass reconstructs
    /// the same projective sample.
    struct SpotShadowView
    {
        /// @brief World → light clip-space transform.
        mat4 ViewProj;
        /// @brief Near clip distance.
        f32 Near = 0.0f;
        /// @brief Far clip distance (the light's range, or the fitted scene-bound far).
        f32 Far = 0.0f;
        /// @brief Vertical field of view of the projection, in radians.
        ///
        /// The cone width the frustum was built with (2·outerCone, or the tighter scene-bound
        /// fit). A caller sizes the tile's world-per-texel from this and Far to scale depth bias.
        f32 Fovy = 0.0f;
    };

    /// @brief A point light's six cube-face shadow views.
    ///
    /// One perspective frustum per axis (90° fovy, aspect 1). Element f is face f's
    /// world→clip matrix in CubeFace order; all six share Near/Far. The lighting pass
    /// picks the face from the light→fragment direction's major axis and samples
    /// projectively.
    struct PointShadowView
    {
        /// @brief Per-face world → clip transforms, in CubeFace order.
        std::array<mat4, CubeFaceCount> ViewProj;
        /// @brief Near clip distance (shared by all six faces).
        f32 Near = 0.0f;
        /// @brief Far clip distance (equal to the light's range).
        f32 Far = 0.0f;
    };

    /// @brief Compute a spot light's perspective shadow view, optionally fit to a scene bound.
    ///
    /// Base frustum: fovy from the outer cone half-angle (2·OuterCone, clamped below π so the
    /// projection never degenerates), aspect 1, far = range, near a small fixed fraction of range.
    ///
    /// When @p sceneBounds is supplied and non-empty, the frustum is **tightened** (never widened)
    /// to the bound the light must shadow — the perspective analogue of the cascade slab fit: the
    /// far plane pulls in to the bound's farthest front corner, and, when the whole bound lies in
    /// front of the light, the near plane pulls out to its nearest corner and the cone narrows to
    /// the bound's angular radius about the aim axis. This packs the tile's texels onto the actual
    /// casters — a distant bright light over a small, far-off scene gains a large texel-density
    /// win. A bound straddling the light (a corner behind it) leaves the cone and near at the
    /// authored width; only the far tightens.
    /// @param position    World-space light position.
    /// @param direction   World-space light direction (toward the target).
    /// @param range       Light falloff radius; the far plane before any scene-bound fit.
    /// @param outerCone   Outer cone half-angle in radians; must be in (0, π/2).
    /// @param sceneBounds World-space bound to fit the frustum to, or nullopt for the base frustum.
    /// @return The (possibly fitted) view-proj, near, far, and fovy.
    /// @pre range > 0, outerCone ∈ (0, π/2) — asserted.
    [[nodiscard]] SpotShadowView
    ComputeSpotShadowView(vec3 position, vec3 direction, f32 range, f32 outerCone,
                          std::optional<AABB> sceneBounds = std::nullopt);

    /// @brief Compute a point light's six cube-face shadow views.
    ///
    /// Each face is lookAt(position, position + faceForward, faceUp) with a 90°
    /// perspective; the six forwards/ups are the canonical cube basis (the same set a
    /// Vulkan cube map expects). far = range, near a small fixed fraction of range.
    /// @param position  World-space light position.
    /// @param range     Light falloff radius; used as the far plane.
    /// @pre range > 0 — asserted.
    [[nodiscard]] PointShadowView ComputePointShadowView(vec3 position, f32 range);
}
