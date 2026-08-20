#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ShadowCascades.h>

/// @brief Device-free packing of a scene's lights into the renderer's GPU light layout.
///
/// The CPU side of SceneRenderer's per-frame lighting setup: gathering Light entities,
/// assigning the cascade sets and the punctual shadow slots, and laying each light out in
/// the shader's std430 form. Pure glm math over a Scene — no device — so it is
/// unit-testable, like ShadowCascades and PunctualShadows.

namespace Veng
{
    class Scene;
}

namespace Veng::Renderer
{
    /// @brief The bit meanings of a PackedLight's Cone.w flags word, mirrored in light_flags.slang.
    ///
    /// A small integer carried in a float, so every value must stay well inside f32's exact
    /// integer range — which two flags, a two-bit index and one more flag comfortably do.
    namespace LightFlags
    {
        /// @brief The area light emits from both faces of its shape.
        inline constexpr u32 TwoSided = 1u << 0;
        /// @brief A near-parallel area light shadowed by the cascade atlas, not by a punctual tile.
        ///
        /// A Directional is cascade-shadowed by its type and never sets this; the flag exists
        /// because an area light's arm cannot be read off its type.
        inline constexpr u32 AreaCascadeShadowed = 1u << 1;
        /// @brief Shift of the two-bit cascade-set index within the flags word.
        inline constexpr u32 CascadeSetShift = 2u;
        /// @brief Mask of the cascade-set index: which of the atlas's sets shadows this light.
        inline constexpr u32 CascadeSetMask = 0x3u << CascadeSetShift;
        /// @brief The light asked for a cascade and the atlas had no set left; it shades unshadowed.
        ///
        /// Set only on a shadow-casting Directional, which has no punctual fallback — an area
        /// light denied a set keeps its perspective tile instead. The flag is what makes the
        /// budget's edge visible in the packed light rather than an unexplained missing shadow.
        inline constexpr u32 CascadeDenied = 1u << 4;
    }

    /// @brief One light packed for the ring-buffered light buffer (set-0 binding 6).
    ///
    /// std430-compatible, matching the shader's GpuLight byte-for-byte: six vec4s. The
    /// first four are the punctual-light fields; the last two carry the area-light
    /// shape (sphere radius, polygon vertex range into the area-vertex buffer, the
    /// area-shadow slot, and the precomputed world-space area normal).
    struct PackedLight
    {
        /// @brief xyz world position, w range.
        vec4 PositionRange;
        /// @brief xyz travel direction, w LightType.
        vec4 DirectionType;
        /// @brief rgb linear color, a intensity.
        vec4 ColorIntensity;
        /// @brief x cos(inner), y cos(outer), z punctual shadow slot (-1 unshadowed), w LightFlags.
        vec4 Cone;
        /// @brief x sphere radius, y polygon vertex base, z polygon vertex count, w area-shadow slot (-1 none).
        vec4 Area;
        /// @brief xyz world-space area normal (Rect/Polygon local +Z), w pad.
        vec4 AreaNormal;
    };

    static_assert(sizeof(PackedLight) == BindlessRegistry::LightStride,
                  "PackedLight must match the bindless light buffer stride");

    /// @brief A cascade-travel array filled with the direction a set with no source is fit to.
    ///
    /// Straight down: a scene with no directional light still produces a usable cascade matrix,
    /// which the ShadowParams enable flag then leaves unsampled.
    [[nodiscard]] inline std::array<vec3, MaxCascadeSets> DefaultCascadeTravel()
    {
        std::array<vec3, MaxCascadeSets> travel{};
        travel.fill(vec3(0.0f, -1.0f, 0.0f));
        return travel;
    }

    /// @brief The per-frame result of packing a scene's lights for the renderer.
    ///
    /// Mirrors the SceneView fields the renderer fills each Execute: the packed light
    /// array and count, the selected punctual shadow records (tile-remapped for the
    /// lighting pass) plus their raw per-face matrices (for the depth pass and frustum
    /// cull), and the cascade sets granted this frame.
    struct PackedSceneLights
    {
        /// @brief Lights packed in iteration order, valid in [0, LightCount).
        std::array<PackedLight, SceneView::MaxLights> Lights{};
        /// @brief Number of packed lights, capped at SceneView::MaxLights.
        u32 LightCount = 0;

        /// @brief World-space polygon vertices for Rect/Polygon area lights, valid in [0, AreaVertexCount).
        std::array<vec4, BindlessRegistry::MaxAreaVertices> AreaVertices{};
        /// @brief Number of packed area vertices, capped at MaxAreaVertices.
        u32 AreaVertexCount = 0;

        /// @brief Shadow records for the first MaxShadowedPunctual point/spot lights, valid in [0, PunctualCount).
        std::array<PunctualShadowRecord, MaxShadowedPunctual> PunctualRecords{};
        /// @brief Raw (non-tile-remapped) per-record/per-face matrices, parallel to PunctualRecords.
        std::array<std::array<mat4, CubeFaceCount>, MaxShadowedPunctual> PunctualRawViewProj{};
        /// @brief Number of shadowed punctual lights, capped at MaxShadowedPunctual.
        u32 PunctualCount = 0;

        /// @brief Number of cascade sets granted this frame, capped at MaxCascadeSets.
        ///
        /// A set is granted to a shadow-casting Directional, or to a near-parallel area light,
        /// in descending order of estimated contribution — so set 0 belongs to the source that
        /// delivers most light to the scene, whatever order the scene iterated its lights in.
        u32 CascadeSetCount = 0;
        /// @brief Travel direction of each granted set's source; [0, CascadeSetCount) valid.
        ///
        /// Entries past the count keep the default straight-down direction, so a scene with no
        /// directional light still drives a sensible cascade matrix.
        std::array<vec3, MaxCascadeSets> CascadeTravel = DefaultCascadeTravel();

        /// @brief Shadow-casting directionals the cascade budget could not seat.
        ///
        /// Each is packed with LightFlags::CascadeDenied and shades unshadowed. Nonzero means the
        /// scene asked for more cascade sets than MaxCascadeSets, which is a budget statement
        /// rather than an error — but a silent one without this count.
        u32 DeniedDirectionalCount = 0;
    };

    /// @brief Packs every Light entity in @p world into the renderer's GPU light layout.
    ///
    /// Iterates the scene's Light entities (capped at SceneView::MaxLights) and packs each in
    /// iteration order; spot cone half-angles are stored as cosines for the shader's dot-product
    /// compare and the punctual shadow slot (or -1) rides Cone.z.
    ///
    /// **The two shadow budgets are spent by contribution, not by arrival.** Every
    /// shadow-casting light is scored by the radiance the lighting pass would apply to the point
    /// of @p sceneBounds nearest it — a directional's unattenuated radiance, or a punctual
    /// light's radiance under the shader's own range falloff and inverse-square, the latter
    /// clamped at its value one world unit out so a light standing inside the bound cannot
    /// outrank by an unbounded factor. The scored lights are then walked from the top: a
    /// Directional (or a near-parallel area light) takes one of MaxCascadeSets cascade sets,
    /// and a point/spot/area light takes one of MaxShadowedPunctual atlas slots, each computing
    /// its tile-remapped and raw shadow matrices and a texel-scaled depth bias when
    /// @p punctualShadows is set. **Equal scores keep scene iteration order**, which makes the
    /// ranking a stable total order: the same scene packs the same way every frame, and two
    /// identically-contributing lights cannot trade a slot between frames.
    ///
    /// A near-parallel area light denied a cascade set falls back to its own perspective tile.
    /// A Directional has no such fallback, so it is packed with LightFlags::CascadeDenied and
    /// counted in DeniedDirectionalCount — it shades unshadowed, and says so.
    ///
    /// @param world                    Scene whose Light entities are packed.
    /// @param punctualShadows          Whether point/spot lights are assigned shadow slots.
    /// @param punctualShadowResolution Per-tile edge length, used to scale the depth bias.
    /// @param sceneBounds              Caster bound the spot/area shadow frustums are fit to; the
    ///                                 empty box (the default) leaves each frustum at its light's
    ///                                 own range and cone.
    /// @return The packed lights, shadow records, and cascade-set selection for this frame.
    [[nodiscard]] PackedSceneLights PackSceneLights(const Scene& world, bool punctualShadows,
                                                    u32 punctualShadowResolution,
                                                    const AABB& sceneBounds = AABB::Empty());
}
