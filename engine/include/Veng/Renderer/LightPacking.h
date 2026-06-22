#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/SceneRenderer.h>

/// @brief Device-free packing of a scene's lights into the renderer's GPU light layout.
///
/// The CPU side of SceneRenderer's per-frame lighting setup: gathering Light entities,
/// selecting the directional light, assigning punctual shadow slots, and laying each
/// light out in the shader's std430 form. Pure glm math over a Scene — no device — so it
/// is unit-testable, like ShadowCascades and PunctualShadows.

namespace Veng
{
    class Scene;
}

namespace Veng::Renderer
{
    /// @brief One light packed for the ring-buffered light buffer (set-0 binding 6).
    ///
    /// std430-compatible, matching the shader's GpuLight byte-for-byte: four vec4s.
    struct PackedLight
    {
        /// @brief xyz world position, w range.
        vec4 PositionRange;
        /// @brief xyz travel direction, w LightType.
        vec4 DirectionType;
        /// @brief rgb linear color, a intensity.
        vec4 ColorIntensity;
        /// @brief x cos(inner), y cos(outer), z shadow slot (-1 unshadowed), w pad.
        vec4 Cone;
    };

    static_assert(sizeof(PackedLight) == BindlessRegistry::LightStride,
                  "PackedLight must match the bindless light buffer stride");

    /// @brief The per-frame result of packing a scene's lights for the renderer.
    ///
    /// Mirrors the SceneView fields the renderer fills each Execute: the packed light
    /// array and count, the selected punctual shadow records (tile-remapped for the
    /// lighting pass) plus their raw per-face matrices (for the depth pass and frustum
    /// cull), and the directional light selection.
    struct PackedSceneLights
    {
        /// @brief Lights packed in iteration order, valid in [0, LightCount).
        std::array<PackedLight, SceneView::MaxLights> Lights{};
        /// @brief Number of packed lights, capped at SceneView::MaxLights.
        u32 LightCount = 0;

        /// @brief Shadow records for the first MaxShadowedPunctual point/spot lights, valid in [0, PunctualCount).
        std::array<PunctualShadowRecord, MaxShadowedPunctual> PunctualRecords{};
        /// @brief Raw (non-tile-remapped) per-record/per-face matrices, parallel to PunctualRecords.
        std::array<std::array<mat4, CubeFaceCount>, MaxShadowedPunctual> PunctualRawViewProj{};
        /// @brief Number of shadowed punctual lights, capped at MaxShadowedPunctual.
        u32 PunctualCount = 0;

        /// @brief True if the scene has at least one directional light.
        bool HaveDirectional = false;
        /// @brief Travel direction of the first directional light (default straight down with none).
        vec3 DirectionalTravel{0.0f, -1.0f, 0.0f};
    };

    /// @brief Packs every Light entity in @p world into the renderer's GPU light layout.
    ///
    /// Iterates the scene's Light entities (capped at SceneView::MaxLights), records the
    /// first directional light's direction, and — when @p punctualShadows is set —
    /// assigns the first MaxShadowedPunctual point/spot lights an atlas shadow slot,
    /// computing each one's tile-remapped and raw shadow matrices and a texel-scaled
    /// depth bias. Each light's spot cone half-angles are stored as cosines for the
    /// shader's dot-product compare, and its shadow slot (or -1) rides Cone.z.
    ///
    /// @param world                    Scene whose Light entities are packed.
    /// @param punctualShadows          Whether point/spot lights are assigned shadow slots.
    /// @param punctualShadowResolution Per-tile edge length, used to scale the depth bias.
    /// @return The packed lights, shadow records, and directional selection for this frame.
    [[nodiscard]] PackedSceneLights PackSceneLights(const Scene& world, bool punctualShadows,
                                                    u32 punctualShadowResolution);
}
