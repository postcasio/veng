#pragma once

#include <Veng/Veng.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class PhysicsWorld;

    /// @brief Every PhysicsLayer, as a QueryFilter::Layers mask.
    inline constexpr u32 AllPhysicsLayers = (1U << PhysicsLayerCount) - 1U;

    /// @brief What a spatial query is allowed to report.
    ///
    /// A query reads no ambient state: what it may hit is stated here, at the call. The world's
    /// CollisionMatrix governs *simulation*, not queries — a PhysicsLayer::Query body collides
    /// with nothing and is still findable, which is what makes that layer useful.
    struct QueryFilter
    {
        /// @brief Bit per PhysicsLayer; a body is reported only when its layer's bit is set.
        u32 Layers = AllPhysicsLayers;
        /// @brief Bodies to skip — typically the querying entity and whatever it carries.
        ///
        /// A non-owning view: it must outlive the call, which is trivially true for the caller's
        /// own local array. Linear-scanned, so it is meant for a handful of entities.
        std::span<const Entity> Ignore;
        /// @brief Whether a sensor body may be reported.
        ///
        /// Cleared by default: a query asking "what would I hit" wants solid geometry, and a
        /// trigger volume is not that. Set it to sweep for trigger volumes deliberately.
        bool IncludeSensors = false;
    };

    /// @brief Where a ray first met a body.
    struct RayHit
    {
        /// @brief The entity whose body was hit.
        Entity Body;
        /// @brief The hit point, in the physics world's frame.
        dvec3 Position{0.0};
        /// @brief The surface normal at the hit point, pointing out of the hit body.
        vec3 Normal = vec3(0.0f);
        /// @brief Distance along the ray in metres, in [0, maxDistance].
        f32 Distance = 0.0f;
        /// @brief Distance as a fraction of maxDistance, in [0, 1].
        f32 Fraction = 0.0f;
    };

    /// @brief Where a swept shape first met a body.
    struct ShapeHit
    {
        /// @brief The entity whose body was hit.
        Entity Body;
        /// @brief The contact point, in the physics world's frame.
        dvec3 Position{0.0};
        /// @brief The contact normal at that point, pointing out of the hit body.
        vec3 Normal = vec3(0.0f);
        /// @brief Fraction of the swept displacement travelled before contact, in [0, 1].
        ///
        /// The displacement is `to - from.Position`, so the pose the sweep stops at is
        /// `from.Position + (to - from.Position) * Fraction` at `from.Rotation`. A shape already
        /// overlapping something at `from` reports 0.
        f32 Fraction = 0.0f;
    };

    /// @brief Casts a ray and returns the nearest body it meets.
    ///
    /// A pure query: it mutates nothing, so a View-phase consumer may call it safely.
    /// @param world        The world to query; **null returns nullopt**, so a scene with optional
    ///                     physics degrades quietly rather than asserting.
    /// @param origin       Ray origin, in the physics world's frame.
    /// @param direction    Ray direction; need not be normalized, and a zero direction finds nothing.
    /// @param maxDistance  How far along @p direction to look, in metres.
    /// @param filter       Which bodies may be reported.
    /// @return The nearest hit, or nullopt when the ray reaches @p maxDistance unobstructed.
    [[nodiscard]] VE_API optional<RayHit> Raycast(const PhysicsWorld* world, dvec3 origin,
                                                  vec3 direction, f32 maxDistance,
                                                  const QueryFilter& filter = {});

    /// @brief Sweeps a shape from a pose to a position and returns the first body it meets.
    ///
    /// The primitive under a kinematic mover: sweep the body's own Collider along the frame's
    /// motion and read back where it stops. The shape is not rotated during the sweep —
    /// @p from's rotation holds for the whole displacement.
    ///
    /// A pure query: it mutates nothing, so a View-phase consumer may call it safely.
    /// @param world   The world to query; **null returns nullopt**.
    /// @param shape   The shape to sweep, in the same vocabulary a body's Collider uses. A
    ///                ColliderShape::Mesh shape whose geometry is a triangle mesh cannot be swept
    ///                (a concave shape has no sweep) and is a fatal assert.
    /// @param from    The pose the sweep starts at, in the physics world's frame.
    /// @param to      The position the sweep ends at, in the physics world's frame.
    /// @param filter  Which bodies may be reported.
    /// @return The first contact along the sweep, or nullopt when it completes unobstructed.
    [[nodiscard]] VE_API optional<ShapeHit> ShapeCast(const PhysicsWorld* world,
                                                      const Collider& shape,
                                                      const PhysicsPose& from, dvec3 to,
                                                      const QueryFilter& filter = {});

    /// @brief Collects every body whose shape intersects one placed at a pose.
    ///
    /// Reports bodies genuinely intersecting the volume; a body merely touching it from outside
    /// is not one. A pure query: it mutates nothing, so a View-phase consumer may call it safely.
    /// @param world   The world to query; **null fills nothing and returns 0**.
    /// @param shape   The shape to test with, in the same vocabulary a body's Collider uses.
    /// @param at      Where to place it, in the physics world's frame.
    /// @param filter  Which bodies may be reported.
    /// @param out     Destination, cleared then filled in ascending entity slot order.
    /// @return The number of entities written to @p out.
    VE_API usize Overlap(const PhysicsWorld* world, const Collider& shape, const PhysicsPose& at,
                         const QueryFilter& filter, vector<Entity>& out);
}
