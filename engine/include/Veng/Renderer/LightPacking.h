#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>
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
    /// @brief Reference illuminance the photometric calibration is anchored to: direct sunlight.
    ///
    /// A round figure for the illuminance a surface receives under direct midday sun. It fixes the
    /// scale of LuminousAnchor below — the one physical quantity the whole calibration is pinned to
    /// — so a directional authored at this many lux carries an internal radiance of exactly one.
    inline constexpr f32 ReferenceSolarIlluminanceLux = 100000.0f;

    /// @brief The engine's photometric calibration: internal linear radiance produced per lux.
    ///
    /// A light's authored intensity is a **physical photometric quantity** whose unit is fixed by
    /// the light's type — lux (illuminance) for a directional, lumens (luminous power) for a point or
    /// spot, nits (cd/m², luminance) for an area light. LightPacking converts that authored quantity
    /// into the renderer's internal linear radiance once, at pack time, and this anchor is the single
    /// scalar tying the two together: the internal radiance carried per lux of directional
    /// illuminance (equivalently, per nit of area-light luminance).
    ///
    /// It is **anchored to a real solar illuminance** — a directional of ReferenceSolarIlluminanceLux
    /// lux, direct sunlight, packs an internal radiance of exactly one — so a real-world lumen or lux
    /// count read off a datasheet or a light meter lands at a physically sensible brightness relative
    /// to a sun. The point/spot divisions by the emitter's solid angle (`Φ / 4π`, `Φ / 2π(1−cos θ)`)
    /// and the area path's direct luminance are all expressed against this same number.
    ///
    /// It is **exported** so a consumer authoring its own physically-measured light model expresses
    /// that model against the identical scalar and cannot drift from the engine's calibration.
    inline constexpr f32 LuminousAnchor = 1.0f / ReferenceSolarIlluminanceLux;

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
    /// std430-compatible, matching the shader's GpuLight byte-for-byte: seven vec4s. The
    /// first four are the punctual-light fields; the next two carry the area-light
    /// shape (emitter radius, polygon vertex range into the area-vertex buffer, the
    /// area-shadow slot, the precomputed world-space area normal, and the shadow source radius);
    /// the last carries how the light's response is weighted.
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
        /// @brief x emitter radius, y polygon vertex base, z polygon vertex count, w area-shadow slot (-1 none).
        ///
        /// x is the lighting radius: a Sphere's emitter radius for the LTC integral, or a
        /// Point/Spot's source radius clamping the shading distance. It is a physical size and is
        /// never capped — see AreaNormal's w, which is the shadow-sizing lane.
        vec4 Area;
        /// @brief xyz world-space area normal (Rect/Polygon local +Z), w shadow source radius.
        ///
        /// w is the world radius the PCSS estimator sizes its penumbra from — an area light's
        /// emitter or bounding radius, zero for a punctual light, which carries no soft shadow.
        /// It is authored the same size as Area's x but serves a different role: the lighting
        /// path integrates exactly at any size, while the shadow estimator is an approximation
        /// with a bounded domain, so the lighting pass caps this lane's *angular* size per
        /// fragment. Keep the two lanes separate; the cap must never reach Area's x.
        vec4 AreaNormal;
        /// @brief x the specular scale (Light::SpecularScale); y, z and w unused.
        vec4 Response;
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
        /// @brief The selected lights in scene iteration order, valid in [0, LightCount).
        std::array<PackedLight, SceneView::MaxLights> Lights{};
        /// @brief Number of packed lights, capped at SceneView::MaxLights.
        u32 LightCount = 0;
        /// @brief Lights that could contribute but ranked below the MaxLights packed this frame.
        ///
        /// Nonzero means the scene offered more contributing lights than one view carries; the
        /// excess shades nothing this frame. Lights skipped because they cannot contribute (zero
        /// radiance, zero range, a degenerate emitter, or out of the camera frustum's reach) are
        /// not counted.
        u32 DroppedLightCount = 0;

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
        /// @brief Per record, the views the depth pass renders: bit f set renders face f.
        ///
        /// A spot or area record carries bit 0 alone. A point record carries one bit per cube face
        /// whose frustum can reach the camera frustum — a face that cannot is never sampled by a
        /// visible pixel, so its tile is left at the clear. All six without a camera frustum.
        std::array<u8, MaxShadowedPunctual> PunctualFaceMask{};

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

    /// @brief Packs the Light entities in @p world that can light this view into the renderer's GPU light layout.
    ///
    /// **Only a light that can contribute is packed.** A light is skipped when its radiance is
    /// zero, when it is positioned and its range is not positive, when it is an area light whose
    /// emitter has no area (a Rect with no extent, a Polygon of fewer than three non-collinear
    /// vertices, a Sphere of zero radius), or — given @p cameraFrustum — when its range sphere,
    /// grown by its emitter's reach, misses the frustum: such a light lights no visible pixel.
    ///
    /// **Past the cap, the lights that look brightest from the camera win.** The remaining lights
    /// are ranked by the radiance each delivers at @p viewpoint — a directional's unattenuated
    /// radiance, a positioned light's radiance under the inverse square from its emitter's
    /// surface, clamped at its value one world unit out — and the top SceneView::MaxLights are
    /// packed, the rest counted in DroppedLightCount. The ranking leaves out the range cutoff
    /// deliberately: a light whose range ends short of the camera still lights what the camera
    /// sees, and the cutoff would score it zero. Equal scores keep scene iteration order, and the
    /// packed lights are laid out in scene iteration order, so a scene of at most MaxLights
    /// contributing lights packs exactly as it iterates.
    ///
    /// Spot cone half-angles are stored as cosines for the shader's dot-product compare and the
    /// punctual shadow slot (or -1) rides Cone.z.
    ///
    /// **The two shadow budgets are spent by contribution, not by arrival.** Every packed
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
    /// **A punctual slot goes only to a light that can shadow something visible.** A point
    /// light's cube faces whose frustums miss @p cameraFrustum are left out of PunctualFaceMask,
    /// and a point light none of whose faces survive takes no slot, leaving it to the next light
    /// in the ranking.
    ///
    /// @param world                    Scene whose Light entities are packed.
    /// @param punctualShadows          Whether point/spot lights are assigned shadow slots.
    /// @param punctualShadowResolution Per-tile edge length, used to scale the depth bias.
    /// @param sceneBounds              Caster bound the spot/area shadow frustums are fit to; the
    ///                                 empty box (the default) leaves each frustum at its light's
    ///                                 own range and cone.
    /// @param cameraFrustum            The view's camera frustum the light, slot and face tests
    ///                                 use; null (the default) packs and grants without testing.
    /// @param viewpoint                The camera position the cap's ranking is measured from;
    ///                                 null (the default) ranks by radiance alone.
    /// @return The packed lights, shadow records, and cascade-set selection for this frame.
    [[nodiscard]] PackedSceneLights PackSceneLights(const Scene& world, bool punctualShadows,
                                                    u32 punctualShadowResolution,
                                                    const AABB& sceneBounds = AABB::Empty(),
                                                    const Frustum* cameraFrustum = nullptr,
                                                    const vec3* viewpoint = nullptr);
}
