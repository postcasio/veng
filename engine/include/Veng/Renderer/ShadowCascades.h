#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Camera.h>

/// @brief Pure, device-free cascaded shadow map math.
///
/// Turns a camera, a light direction, and the scene bound into per-cascade
/// light-space view-proj matrices and split distances. glm-only — no Context, no
/// device — so it compiles and unit-tests without an ICD, beside PunctualShadows.h.
namespace Veng::Renderer
{
    /// @brief Maximum number of shadow cascades the engine supports.
    inline constexpr u32 MaxCascades = 4;

    /// @brief Output of ComputeCascades: per-cascade matrices and split distances.
    struct CascadeData
    {
        /// @brief Per-cascade world → light-clip matrices; only [0, Count) are valid.
        std::array<mat4, MaxCascades> ViewProj;

        /// @brief Each cascade's far distance in view space.
        ///
        /// The lighting pass selects a cascade by comparing the fragment's view depth
        /// against these. Element i is cascade i's far split; [0, Count) valid. With
        /// MaxCascades == 4 this array packs directly into the ShadowConstants
        /// CascadeSplits vec4.
        std::array<f32, MaxCascades> SplitFar;

        /// @brief Number of valid cascades; clamp(settings.Count, 1, MaxCascades).
        u32 Count = 0;
    };

    /// @brief Inputs that control how cascades are split and sized.
    struct CascadeSettings
    {
        /// @brief Requested cascade count; clamped to [1, MaxCascades].
        u32 Count = 4;
        /// @brief PSSM split blend: 0 = uniform splits, 1 = logarithmic.
        f32 Lambda = 0.85f;
        /// @brief Per-cascade tile edge in texels; drives texel snapping.
        u32 Resolution = 1024;
    };

    /// @brief Computes per-cascade fit-to-frustum light matrices.
    ///
    /// lightDir is the light's travel direction. sceneBounds (world space, possibly
    /// empty) extends each cascade's near plane toward the light so off-screen casters
    /// are included.
    /// @param camera       The view camera that defines the split frustum.
    /// @param lightDir     World-space direction the light travels (toward receivers).
    /// @param sceneBounds  World-space scene bound; may be empty.
    /// @param settings     Split count, lambda, and tile resolution.
    /// @return Per-cascade matrices and view-space split distances.
    [[nodiscard]] CascadeData ComputeCascades(const CameraView& camera, vec3 lightDir,
                                              const AABB& sceneBounds,
                                              const CascadeSettings& settings);

    /// @brief Tile layout of the directional shadow atlas for a given cascade count.
    ///
    /// min(Count,2) columns × ceil(Count/2) rows of square tiles: 1×1 for one cascade,
    /// 2×1 for two, 2×2 for three or four. A low cascade count pays for no idle tiles.
    /// Cascade k maps to tile (k % Columns, k / Columns). Both the render pass
    /// (per-cascade viewport) and the lighting-constant tile remap derive their layout
    /// from this.
    struct ShadowAtlasGrid
    {
        /// @brief Number of tile columns in the atlas.
        u32 Columns;
        /// @brief Number of tile rows in the atlas.
        u32 Rows;
    };

    /// @brief Returns the shadow atlas tile grid for the given cascade count.
    /// @param cascadeCount  Requested cascade count; clamped to [1, MaxCascades].
    [[nodiscard]] inline ShadowAtlasGrid ComputeShadowAtlasGrid(u32 cascadeCount)
    {
        const u32 count =
            cascadeCount < 1 ? 1 : (cascadeCount > MaxCascades ? MaxCascades : cascadeCount);
        const u32 columns = count < 2 ? count : 2;
        const u32 rows = (count + 1) / 2;
        return {.Columns = columns, .Rows = rows};
    }
}
