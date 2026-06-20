#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Camera.h>

// Cascaded-shadow-map math: a pure, device-free function turning a camera, a
// light direction, and the scene bound into per-cascade light-space view-proj
// matrices and split distances. glm-only — no Context, no device — so it lives
// under the renderer surface but compiles and unit-tests with no ICD.

namespace Veng::Renderer
{
    inline constexpr u32 MaxCascades = 4;

    struct CascadeData
    {
        // Per-cascade world → light-clip matrices (only [0, Count) are valid).
        std::array<mat4, MaxCascades> ViewProj;
        // Each cascade's far distance in view space (the lighting pass selects a
        // cascade by comparing the fragment's view depth against these). Element i
        // is cascade i's far split; [0, Count) valid. With MaxCascades == 4 this
        // array packs directly into the ShadowConstants CascadeSplits vec4.
        std::array<f32, MaxCascades> SplitFar;
        u32 Count = 0; // cascades produced = clamp(settings.Count, 1, MaxCascades)
    };

    struct CascadeSettings
    {
        u32 Count = 4; // clamped to [1, MaxCascades]
        f32 Lambda = 0.85f; // 0 = uniform splits, 1 = logarithmic (PSSM)
        u32 Resolution = 1024; // per-cascade tile edge, drives texel snapping
    };

    // Pure: per-cascade fit-to-frustum light matrices. lightDir is the light's
    // travel direction; sceneBounds (world space, possibly empty) extends each
    // cascade's near plane toward the light so off-screen casters are included.
    [[nodiscard]] CascadeData ComputeCascades(
        const Camera& camera, vec3 lightDir, const AABB& sceneBounds,
        const CascadeSettings& settings);

    // The shadow atlas tile grid for a cascade count: min(Count,2) columns ×
    // ceil(Count/2) rows of square tiles (1×1 for one cascade, 2×1 for two, 2×2 for
    // three or four), so a low cascade count pays for no idle tiles. Cascade k maps
    // to tile (k % Columns, k / Columns). Both the render pass (per-cascade
    // viewport) and the lighting-constant tile remap derive their layout from this.
    struct ShadowAtlasGrid
    {
        u32 Columns;
        u32 Rows;
    };

    [[nodiscard]] inline ShadowAtlasGrid ComputeShadowAtlasGrid(u32 cascadeCount)
    {
        const u32 count = cascadeCount < 1 ? 1 : (cascadeCount > MaxCascades ? MaxCascades : cascadeCount);
        const u32 columns = count < 2 ? count : 2;
        const u32 rows = (count + 1) / 2;
        return {columns, rows};
    }
}
