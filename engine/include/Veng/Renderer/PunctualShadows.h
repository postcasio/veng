#pragma once

#include <array>

#include <Veng/Veng.h>

// Punctual shadow-view math: pure, device-free functions turning a spot or
// point light into the world→light-clip matrices a depth pass renders the
// casters with and the lighting pass samples against. glm-only — no Context, no
// device — so it lives under the renderer surface but compiles and unit-tests
// with no ICD, beside ShadowCascades.h.

namespace Veng::Renderer
{
    // The six cube faces, in the fixed order the point-shadow atlas tiles them and
    // the lighting pass selects them by the major axis of the light→fragment
    // direction: +X, -X, +Y, -Y, +Z, -Z.
    inline constexpr u32 CubeFaceCount = 6;

    // A spot light's single perspective shadow view. ViewProj maps a world point to
    // the light's clip space (Vulkan ZO, Y-flipped to match Camera); a fragment in
    // the cone projects to z ∈ [0,1], xy ∈ [-1,1]. Near/Far are the clip range the
    // depth pass uses; the lighting pass reconstructs the same projective sample.
    struct SpotShadowView
    {
        mat4 ViewProj;
        f32  Near = 0.0f;
        f32  Far  = 0.0f;
    };

    // A point light's six cube-face shadow views, one perspective frustum per axis
    // (90° fovy, aspect 1). Element f is face f's world→clip matrix in CubeFace
    // order; all six share Near/Far. The lighting pass picks the face from the
    // light→fragment direction's major axis and samples projectively, exactly as a
    // spot tile is sampled.
    struct PointShadowView
    {
        std::array<mat4, CubeFaceCount> ViewProj;
        f32 Near = 0.0f;
        f32 Far  = 0.0f;
    };

    // Pure: the spot's perspective shadow view. fovy is derived from the outer cone
    // half-angle (2·OuterCone, clamped below π so the projection never degenerates),
    // aspect 1, far = the light's Range, near a small fixed fraction of Range. A
    // VE_ASSERT pins range > 0 and outerCone in (0, π/2).
    [[nodiscard]] SpotShadowView ComputeSpotShadowView(
        vec3 position, vec3 direction, f32 range, f32 outerCone);

    // Pure: the point's six cube-face views. Each face is lookAt(position, position +
    // faceForward, faceUp) with a 90° perspective; the six forwards/ups are the
    // canonical cube basis (the same set a Vulkan cube map expects). far = Range,
    // near a small fixed fraction. A VE_ASSERT pins range > 0.
    [[nodiscard]] PointShadowView ComputePointShadowView(vec3 position, f32 range);
}
