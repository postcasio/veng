#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief Selects how the deferred lighting pass attenuates a light.
    ///
    /// Directional has no position or falloff. Point and Spot are placed by the
    /// entity's Transform and fall off with distance (Spot adds a cone). Rect,
    /// Sphere, and Polygon are area lights: their emission is integrated over a
    /// finite shape (a linearly-transformed-cosine evaluation of the GGX BRDF), so
    /// they produce a soft, physically-sized specular highlight and penumbra rather
    /// than a punctual point response. Integer values are stable — packed into the
    /// light SSBO and persisted in prefabs.
    enum class LightType : u32
    {
        /// @brief Infinite directional light; no position or falloff. Intensity is an illuminance in lux.
        Directional = 0,
        /// @brief Omnidirectional point light; falls off within Range. Intensity is a luminous power in lumens.
        Point = 1,
        /// @brief Cone spot light; falls off within Range and the cone angles. Intensity is a luminous power in lumens.
        Spot = 2,
        /// @brief Rectangular area light in the entity's local XY plane (Width × Height). Intensity is a luminance in nits.
        Rect = 3,
        /// @brief Spherical area light of the given Radius; correct at any angular size. Intensity is a luminance in nits.
        Sphere = 4,
        /// @brief Convex-polygon area light from PolygonVertices in the entity's local frame. Intensity is a luminance in nits.
        Polygon = 5,
    };

    /// @brief Light component shaded by the deferred lighting pass.
    ///
    /// Type selects the light's shape. Direction is a spot's travel direction —
    /// entity-local, rotated into world space by the entity's Transform, so a spot
    /// parented to a moving body aims with it — and a directional's world-space travel
    /// direction. Color is linear RGB; Intensity scales it.
    /// Range is the falloff radius for every positioned light: measured from the light's
    /// position for a point or spot, and from the nearest point of the emitter for an area
    /// light, so a long Rect reaches as far from its ends as from its middle.
    /// InnerCone/OuterCone are the spot's half-angles in radians: full intensity within
    /// InnerCone, zero beyond OuterCone, smooth between.
    ///
    /// The area shapes are placed and oriented by the entity's Transform: a Rect
    /// spans Width × Height in the local XY plane and emits along local +Z; a Sphere
    /// has the given Radius; a Polygon takes its vertices from PolygonVertices in the
    /// local frame. TwoSided lets a Rect or Polygon emit from both faces.
    ///
    /// The light's world position comes from the entity's Transform — never stored
    /// here — so a parented or animated light moves with its entity.
    struct Light
    {
        /// @brief Light shape.
        LightType Type{LightType::Directional};
        /// @brief Travel direction. Entity-local for a spot (rotated into world space by the
        /// entity's Transform, so it aims with a parented body); world-space for a directional.
        vec3 Direction{0.0f, -1.0f, 0.0f};
        /// @brief Linear RGB color (a chromaticity; brightness lives in Intensity).
        vec3 Color{1.0f, 1.0f, 1.0f};
        /// @brief The light's brightness as a physical photometric quantity, its unit fixed by Type.
        ///
        /// Not a bare multiplier: the unit is the one a datasheet or a light meter prints for that
        /// kind of source, converted to the renderer's internal radiance once at pack time through
        /// the published Renderer::LuminousAnchor.
        ///
        /// | Type | Unit | Quantity |
        /// |---|---|---|
        /// | Directional | lux | illuminance a surface receives (an infinite source has no falloff) |
        /// | Point / Spot | lumens | total luminous power radiated (over the sphere / the cone) |
        /// | Rect / Sphere / Polygon | nits (cd/m²) | the emitter surface's luminance |
        ///
        /// So a sun is authored as its lux, a lamp as its lumens, a panel as its nits — the values
        /// that stay meaningful when the light moves. The anchor is pinned so a directional at a real
        /// solar illuminance carries an internal radiance of one (see Renderer::LuminousAnchor).
        f32 Intensity{1.0f};
        /// @brief Falloff radius for positioned lights (point, spot, and area).
        ///
        /// A point or spot measures it from its position; an area light from its emitter's surface.
        f32 Range{10.0f};
        /// @brief Spot inner half-angle in radians; full intensity within.
        f32 InnerCone{0.0f};
        /// @brief Spot outer half-angle in radians; zero intensity beyond.
        f32 OuterCone{0.5f};
        /// @brief Full width of a Rect area light along the entity's local X axis.
        f32 Width{1.0f};
        /// @brief Full height of a Rect area light along the entity's local Y axis.
        f32 Height{1.0f};
        /// @brief Spherical radius, in world units, of a Sphere area light's shape and of a
        /// Point or Spot light's emitter.
        ///
        /// For a Sphere it is the area emitter's radius. For a Point or Spot it is the spherical
        /// source radius, with two jobs: the lighting pass clamps the shading distance to be no
        /// less than it before the inverse-square, so the near field is bounded by a real source
        /// size rather than exploding as the distance goes to zero; and it widens the light's
        /// specular lobe by the angle it subtends from the shaded point, so a glossy surface
        /// reflects a bulb rather than a point (see AngularRadius for the approximation and its
        /// domain). It is scaled by the entity's world transform, so a parented, scaled light
        /// keeps a consistent size. Zero — the default — is an unsized emitter: a punctual
        /// Point/Spot reproducing the pure inverse-square and the surface's own lobe, or a
        /// degenerate Sphere.
        ///
        /// **A Sphere's radius drives two things, and only one of them is capped.** It sizes the
        /// LTC area-lighting integral, which is exact at any radius and uses the authored size
        /// unaltered; and it sizes the soft shadow, whose PCSS estimator is an approximation
        /// valid only while the source subtends a modest angle. The lighting pass therefore caps
        /// the *shadow* source's angular radius, leaving brightness and falloff untouched. A
        /// Sphere far larger than its distance to a receiver is lit correctly and casts a shadow
        /// softer than physics would give, rather than one the estimator cannot compute.
        f32 Radius{0.0f};
        /// @brief A Directional light's angular radius, in radians: the half-angle its disc subtends.
        ///
        /// It widens the light's specular lobe, so a glossy surface reflects a disc of this size
        /// rather than a point. The default is the Sun's as seen from Earth (0.004675 rad, 0.2679°),
        /// since a directional light most often stands for a sun; zero is a punctual light, whose
        /// highlight on a mirror-smooth surface is the surface's own lobe alone. The widening is a
        /// small-source approximation, accurate for a sun: a source spanning more than a few
        /// degrees should be a Sphere area light, which is exact at any size. It changes only the
        /// highlight's shape — the light's brightness, shadow and reach are untouched.
        f32 AngularRadius{0.004675f};
        /// @brief Whether a Rect or Polygon area light emits from both faces.
        bool TwoSided{false};
        /// @brief Whether this light is given a shadow map.
        ///
        /// True by default, so a light shadows unless it says otherwise. Clearing it takes the
        /// light out of every shadow arm at once: it is passed over for a punctual/area atlas slot,
        /// and it is not eligible to drive the cascade as a near-parallel area light.
        ///
        /// **The shadow arms are a scarce resource, and a light is often not worth one.** Only
        /// MaxShadowedPunctual point/spot/area lights are slotted per frame and the rest silently
        /// carry no shadow at all — and while the packing pass spends the budget on the lights it
        /// estimates contribute most, a bright light that wants no silhouette still outranks a
        /// dimmer one that does. The case this exists for is a **fill light**: one standing in for the emission of a surface that is already
        /// drawn, or filling a volume that has no occluder worth resolving. Such a light wants its
        /// contribution and not its silhouette, and a perspective tile fit to the whole scene bound
        /// resolves that silhouette badly at close range anyway — near geometry lands in the few
        /// texels nearest the light's own position, where a tile stretched across a large bound has
        /// the least to spend.
        bool CastsShadows{true};
        /// @brief Scales this light's specular contribution; 0 leaves it diffuse-only.
        ///
        /// 1 by default, the physical response. The diffuse term is untouched at every value, so
        /// this changes how the light reflects, never how much it illuminates. It exists for the
        /// lights that stand in for something rather than being a source in their own right: a
        /// fill or bounce light, or a light carrying the spill of a surface that is already drawn.
        /// Such a light's specular is a reflected image of an emitter nobody sees — on a glossy
        /// surface, a bright copy of its shape where the real surface is already in view.
        f32 SpecularScale{1.0f};
        /// @brief Convex polygon vertices in entity-local space, wound CCW about local +Z (Polygon).
        vector<vec3> PolygonVertices;
        /// @brief An identity a material can find this light by; 0, the default, is untagged.
        ///
        /// The renderer packs it with the light, and a shader looks the light up by it
        /// (`FindLightByTag`, Veng/light_visibility.slang) — so a material lighting itself from one
        /// particular light, such as a fog lit by the one light standing for its sun, can pick that
        /// light out of the view's list. It changes nothing about how the light shades. The values
        /// mean whatever the code writing them says: the engine assigns none and reserves only 0.
        /// Two lights in one view carrying the same nonzero tag make the lookup return the first
        /// of them, and a debug build warns.
        u32 Tag{0};
        /// @brief The light channels this light lights: bit c set lights the surfaces in channel c.
        ///
        /// Three channels (bits 0-2; higher bits are ignored). The default, channel 0 alone, is the
        /// channel every surface is in unless its material says otherwise, so a light that says
        /// nothing lights everything. A surface's material places it in channels through its
        /// surface flags (`SurfaceLightChannels`, Veng/surface_flags.slang), and a light lights only
        /// the surfaces sharing a channel with it: an interior fill kept off the exterior seen
        /// through a window, a rim light reaching one character, a light a self-lit surface
        /// computes itself and so leaves. A light lighting no channel lights nothing and is not
        /// packed. Sprites and particles are members of channel 0. Shadows are unaffected: the
        /// light casts as before, onto whatever it lights.
        u8 Channels{0x01};
    };
}

VE_ENUM(::Veng::LightType, 0x006B1D62EF4B5A16ULL)
VE_ENUMERATOR(Directional)
VE_ENUMERATOR(Point)
VE_ENUMERATOR(Spot)
VE_ENUMERATOR(Rect)
VE_ENUMERATOR(Sphere)
VE_ENUMERATOR(Polygon)
VE_ENUM_END();

VE_REFLECT(::Veng::Light, 0xECF6442708DF7C00ULL)
VE_FIELD(Type, .DisplayName = "Type")
VE_FIELD(Direction, .DisplayName = "Direction")
VE_FIELD(Color, .DisplayName = "Color")
VE_FIELD(Intensity, .DisplayName = "Intensity", .Display = {.Min = 0.0})
VE_FIELD(Range, .DisplayName = "Range", .Display = {.Min = 0.01, .Step = 0.1},
         .VisibleIf = VE_WHEN(self.Type != ::Veng::LightType::Directional))
VE_FIELD(InnerCone, .DisplayName = "Inner Cone",
         .Display = {.Min = 0.0, .Max = 3.14159265, .Step = 0.01},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Spot))
VE_FIELD(OuterCone, .DisplayName = "Outer Cone",
         .Display = {.Min = 0.0, .Max = 3.14159265, .Step = 0.01},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Spot))
VE_FIELD(Width, .DisplayName = "Width", .Display = {.Min = 0.0, .Step = 0.05},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Rect))
VE_FIELD(Height, .DisplayName = "Height", .Display = {.Min = 0.0, .Step = 0.05},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Rect))
VE_FIELD(Radius, .DisplayName = "Radius", .Display = {.Min = 0.0, .Step = 0.05},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Point ||
                              self.Type == ::Veng::LightType::Spot ||
                              self.Type == ::Veng::LightType::Sphere))
VE_FIELD(AngularRadius, .DisplayName = "Angular Radius",
         .Tooltip = "Half-angle the light's disc subtends, in radians (the Sun's is 0.004675)",
         .Display = {.Min = 0.0, .Max = 1.57079633, .Step = 0.0005, .Precision = 5},
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Directional))
VE_FIELD(TwoSided, .DisplayName = "Two Sided",
         .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Rect ||
                              self.Type == ::Veng::LightType::Polygon))
VE_ARRAY_FIELD(PolygonVertices, .DisplayName = "Polygon Vertices",
               .VisibleIf = VE_WHEN(self.Type == ::Veng::LightType::Polygon))
VE_FIELD(CastsShadows, .DisplayName = "Casts Shadows")
VE_FIELD(SpecularScale, .DisplayName = "Specular Scale",
         .Display = {.Min = 0.0, .Max = 1.0, .Step = 0.05})
VE_FIELD(Tag, .DisplayName = "Tag",
         .Tooltip = "An identity a material can find this light by; 0 is untagged")
VE_FIELD(Channels, .DisplayName = "Channels",
         .Tooltip =
             "The light channels this light lights, one bit each (bits 0-2); 1 is the default "
             "channel every surface is in",
         .Display = {.Min = 0.0, .Max = 7.0})
VE_REFLECT_END();
