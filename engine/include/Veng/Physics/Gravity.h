#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

#include <span>

namespace Veng
{
    /// @brief The shape of a bounded region of space: a box, a sphere, or a cylinder.
    ///
    /// Integer values are stable — persisted where a Region is authored.
    enum class RegionShape : u8
    {
        /// @brief An oriented box; HalfExtents are its half sizes along the region's local axes.
        Box = 0,
        /// @brief A sphere; HalfExtents.x is the radius, the orientation irrelevant.
        Sphere = 1,
        /// @brief A cylinder about the region's local Y axis; HalfExtents.x is the radius and
        ///        HalfExtents.y the half height.
        Cylinder = 2,
    };

    /// @brief A bounded, oriented region of space — a box, sphere, or cylinder.
    ///
    /// A closed-form volume with a containment test and a distance-to-boundary, expressed in the
    /// frame Center/Orientation place it in. A point is transformed into the region's local frame
    /// and tested against the shape, so an oriented box or a tilted cylinder needs no per-face data.
    struct Region
    {
        /// @brief Which shape the region is.
        RegionShape Shape = RegionShape::Box;
        /// @brief The region's centre.
        vec3 Center = vec3(0.0f);
        /// @brief The region's orientation; ignored by a sphere.
        quat Orientation = quat(1.0f, 0.0f, 0.0f, 0.0f);
        /// @brief Half sizes read per Shape: box half extents; sphere radius in x; cylinder radius
        ///        in x and half height in y.
        vec3 HalfExtents = vec3(1.0f);
    };

    /// @brief How a GravitySource shapes its field over its region.
    ///
    /// A source declares the *shape* of the field; the magnitude is authored separately. Integer
    /// values are stable — persisted in prefabs.
    enum class GravityKind : u8
    {
        /// @brief Constant direction over the region. A flat deck, a plated floor.
        Uniform = 0,
        /// @brief Toward (or away from) the source's origin. A planet, an asteroid.
        Radial = 1,
        /// @brief Radially outward from the source's local axis. A rotating habitat.
        Axial = 2,
    };

    /// @brief One authored gravity field: its shape, strength, region, and blend policy.
    ///
    /// Gravity is a field evaluated per body per step rather than a single world constant. A source
    /// declares the *shape* of its field — uniform, radial, or about an axis — over a bounded
    /// region, with a priority deciding who wins where regions overlap and a blend band smoothing
    /// the seam. A body reached by no source falls under no gravity at all: free-fall is a real
    /// state, not a hidden default.
    ///
    /// The component is authored in the entity's local frame; the physics step resolves it to world
    /// space against the entity's Transform before evaluating the field.
    ///
    /// **Magnitude is authored, never derived from an angular rate.** Axial supplies the field's
    /// *direction* from geometry — the part a Uniform source cannot express — and takes its
    /// *strength* as a plain number. It is deliberately not derived from a spin rate: a habitat spun
    /// at a rate that is pleasant to look at and safe to approach produces a centrifugal
    /// acceleration orders of magnitude below anything a body could stand in, so deriving the
    /// magnitude would force a choice between a habitat one can approach and one one can stand in.
    /// Direction is a geometric fact; strength is authored feel; the two are kept separate.
    struct GravitySource
    {
        /// @brief The shape of the field.
        GravityKind Kind = GravityKind::Uniform;
        /// @brief Uniform: the local down vector. Axial: the local spin axis. Unused by Radial.
        vec3 Direction = vec3(0.0f, -1.0f, 0.0f);
        /// @brief The acceleration the field reaches at full strength, in metres per second squared.
        ///
        /// Authored, never derived from an angular rate — see the type's own note.
        f32 Magnitude = 9.81f;
        /// @brief Radial/Axial: the radius below which the field falls off to zero.
        ///
        /// Distance from the origin (Radial) or from the axis (Axial). Below it the field is zero —
        /// the zero-gravity core of a rotating habitat falls out of the geometry rather than needing
        /// a volume of its own. Unused by Uniform.
        f32 InnerRadius = 0.0f;
        /// @brief Radial/Axial: the radius at which the field reaches full strength.
        ///
        /// The field ramps linearly from zero at InnerRadius to Magnitude at OuterRadius, then holds
        /// full strength beyond. Unused by Uniform.
        f32 OuterRadius = 0.0f;
        /// @brief The region of space the source influences.
        Region Bounds;
        /// @brief Higher wins where regions overlap.
        i32 Priority = 0;
        /// @brief Metres over which this source fades in from its region boundary inward.
        ///
        /// Zero is a hard edge. A positive width lets a body crossing from one field into another
        /// blend across the seam rather than snapping.
        f32 BlendWidth = 0.0f;
    };

    /// @brief A GravitySource resolved into world space, the pure evaluator's input.
    ///
    /// The physics step composes each source's authored (local) form against its entity's world
    /// transform to produce one of these, so the evaluator itself is free of any scene, transform,
    /// or physics type. It is runtime-only derived state — never authored, never serialized.
    struct GravitySourceInstance
    {
        /// @brief The shape of the field.
        GravityKind Kind = GravityKind::Uniform;
        /// @brief World-space Uniform down vector, or world-space Axial spin axis.
        vec3 Direction = vec3(0.0f, -1.0f, 0.0f);
        /// @brief World-space origin: the Radial centre, or a point on the Axial axis.
        vec3 Origin = vec3(0.0f);
        /// @brief The acceleration the field reaches at full strength, in metres per second squared.
        f32 Magnitude = 9.81f;
        /// @brief Radius below which a Radial/Axial field is zero. See GravitySource::InnerRadius.
        f32 InnerRadius = 0.0f;
        /// @brief Radius at which a Radial/Axial field reaches full strength. See GravitySource::OuterRadius.
        f32 OuterRadius = 0.0f;
        /// @brief The world-space region the source influences.
        Region Bounds;
        /// @brief Higher wins where regions overlap.
        i32 Priority = 0;
        /// @brief Metres over which this source fades in from its region boundary inward.
        f32 BlendWidth = 0.0f;
    };

    /// @brief Whether @p point lies inside @p region.
    /// @param region  The region to test against.
    /// @param point   The point, in the same frame as the region.
    /// @return True when the point is inside or exactly on the boundary.
    [[nodiscard]] VE_API bool Contains(const Region& region, vec3 point);

    /// @brief Composes a set of world-space gravity sources into the acceleration at a point.
    ///
    /// The pure core of the gravity system, free of any scene, transform, or physics type: the
    /// caller resolves each source to world space and this function returns the acceleration a body
    /// at @p position feels, in metres per second squared. It is the single evaluator both the
    /// physics step and a consumer's up-vector query call, so a dynamic body and a query in the same
    /// place agree by construction.
    ///
    /// Composition is by descending priority: the highest-priority source whose region contains the
    /// point wins outright once the point is more than its BlendWidth inside the boundary. Within a
    /// source's blend band its contribution fades in from the boundary, and the remainder is filled
    /// by the next source down — so a body crossing a seam blends rather than snaps. A point reached
    /// by no source returns zero: free-fall is a real state, not a hidden default.
    ///
    /// @param sources   The world-space sources to compose; order within a priority is preserved.
    /// @param position  The world-space point to evaluate at.
    /// @return The gravitational acceleration at @p position, or zero when no source reaches it.
    [[nodiscard]] VE_API vec3 EvaluateGravity(std::span<const GravitySourceInstance> sources,
                                              vec3 position);
}

VE_ENUM(::Veng::RegionShape, 0x195E4461D012F2EEULL)
VE_ENUMERATOR(Box)
VE_ENUMERATOR(Sphere)
VE_ENUMERATOR(Cylinder)
VE_ENUM_END();

VE_REFLECT(::Veng::Region, 0x59936AF46498ED1EULL)
VE_FIELD(Shape, .DisplayName = "Shape")
VE_FIELD(Center, .DisplayName = "Center")
VE_FIELD(Orientation, .DisplayName = "Orientation")
VE_FIELD(HalfExtents, .DisplayName = "Half Extents",
         .Tooltip = "Box half sizes; sphere radius in x; cylinder radius in x, half height in y")
VE_REFLECT_END();

VE_ENUM(::Veng::GravityKind, 0xF62D9492D3590D93ULL)
VE_ENUMERATOR(Uniform)
VE_ENUMERATOR(Radial)
VE_ENUMERATOR(Axial)
VE_ENUM_END();

VE_REFLECT(::Veng::GravitySource, 0xE57BBD7A52F5449FULL)
VE_FIELD(Kind, .DisplayName = "Kind", .Tooltip = "Uniform, Radial or Axial field shape")
VE_FIELD(Direction, .DisplayName = "Direction",
         .Tooltip = "Uniform: local down vector; Axial: local spin axis")
VE_FIELD(Magnitude, .DisplayName = "Magnitude",
         .Tooltip = "Full-strength acceleration in m/s^2; authored, never derived from a spin rate")
VE_FIELD(InnerRadius, .DisplayName = "Inner Radius",
         .Tooltip = "Radial/Axial: radius below which the field is zero")
VE_FIELD(OuterRadius, .DisplayName = "Outer Radius",
         .Tooltip = "Radial/Axial: radius at which the field reaches full strength")
VE_FIELD(Bounds, .DisplayName = "Bounds")
VE_FIELD(Priority, .DisplayName = "Priority", .Tooltip = "Higher wins where regions overlap")
VE_FIELD(BlendWidth, .DisplayName = "Blend Width",
         .Tooltip = "Metres over which the source fades in from its boundary")
VE_REFLECT_END();
