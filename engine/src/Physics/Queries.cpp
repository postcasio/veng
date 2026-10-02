#include <Veng/Physics/Queries.h>

#include <Veng/Assert.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Physics/PhysicsWorld.h>

#include "PhysicsInternal.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Veng
{
    namespace
    {
        /// @brief Rejects a body whose object layer is not in the filter's mask.
        class LayerMaskFilter final : public JPH::ObjectLayerFilter
        {
        public:
            /// @brief Builds the filter over a QueryFilter's layer mask.
            /// @param mask  Bit per PhysicsLayer.
            explicit LayerMaskFilter(const u32 mask) : m_Mask(mask) {}

            /// @brief Whether bodies on @p layer may be reported.
            /// @param layer  The object layer being tested.
            [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer layer) const override
            {
                return (m_Mask & (1U << static_cast<u32>(layer))) != 0;
            }

        private:
            /// @brief Bit per PhysicsLayer.
            u32 m_Mask;
        };

        /// @brief Rejects the filter's ignored bodies, and sensors unless they were asked for.
        class QueryBodyFilter final : public JPH::BodyFilter
        {
        public:
            /// @brief Builds the filter, resolving the ignore list to solver handles up front.
            /// @param native  The world's backend state, for the entity → body map.
            /// @param filter  The query's filter.
            QueryBodyFilter(const PhysicsWorld::Native& native, const QueryFilter& filter)
                : m_IncludeSensors(filter.IncludeSensors)
            {
                for (const Entity entity : filter.Ignore)
                {
                    const auto found = native.Bodies.find(entity);
                    if (found != native.Bodies.end())
                    {
                        Ignore(found->second.Id);
                    }
                }
            }

            /// @brief Adds a body to the ignore list.
            /// @param bodyId  The body the query skips.
            void Ignore(const JPH::BodyID& bodyId)
            {
                const u32 value = bodyId.GetIndexAndSequenceNumber();
                if (m_InlineCount < m_Inline.size())
                {
                    m_Inline[m_InlineCount++] = value;
                }
                else
                {
                    m_Spill.emplace_back(value);
                }
            }

            /// @brief Whether a body may be reported, by handle alone.
            /// @param bodyId  The body being tested.
            [[nodiscard]] bool ShouldCollide(const JPH::BodyID& bodyId) const override
            {
                const u32 value = bodyId.GetIndexAndSequenceNumber();
                for (usize i = 0; i < m_InlineCount; ++i)
                {
                    if (m_Inline[i] == value)
                    {
                        return false;
                    }
                }
                return std::ranges::find(m_Spill, value) == m_Spill.end();
            }

            /// @brief Whether a body may be reported, once the solver holds it.
            /// @param body  The body being tested.
            [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override
            {
                return m_IncludeSensors || !body.IsSensor();
            }

        private:
            /// @brief How many ignored bodies are held without allocating.
            static constexpr usize InlineIgnore = 4;

            /// @brief Index-and-sequence values of the first InlineIgnore bodies the query skips.
            std::array<u32, InlineIgnore> m_Inline{};
            /// @brief How many entries of m_Inline are in use.
            usize m_InlineCount = 0;
            /// @brief Index-and-sequence values of any further bodies the query skips.
            vector<u32> m_Spill;
            /// @brief Whether a sensor body may be reported.
            bool m_IncludeSensors;
        };

        /// @brief The solver shape a query's Collider describes, built for the duration of one query.
        ///
        /// A primitive is built in place, embedded, so it costs neither a heap allocation nor a
        /// cache entry; cooked geometry is resolved through the world's shape cache, so a query
        /// naming a body's own collider reuses the shape the body was built with.
        class QueryShape
        {
        public:
            /// @brief Builds the shape a query's Collider describes, asserting it is castable.
            /// @param world     The world whose shape cache resolves cooked geometry.
            /// @param shape     The collider to build.
            /// @param sweeping  Whether the shape is about to be swept, which a concave shape
            ///                  cannot be.
            QueryShape(const PhysicsWorld& world, const Collider& shape, const bool sweeping)
            {
                if (shape.Shape == ColliderShape::Mesh)
                {
                    if (sweeping)
                    {
                        const CollisionShape* geometry = shape.Geometry.Get();
                        VE_ASSERT(geometry == nullptr || !geometry->ContainsTriangleMesh(),
                                  "ShapeCast was given a triangle-mesh CollisionShape; a concave "
                                  "shape has no sweep, so only a convex hull, a primitive, or a "
                                  "compound of those can be cast");
                    }
                    m_Cached = world.GetNative().Shapes.Acquire(shape, world.GetStepCount());
                    m_Shape = m_Cached.GetPtr();
                    return;
                }

                const JPH::Shape* primitive = BuildPrimitive(shape);
                // A primitive is re-placed when the collider carries a local offset or
                // orientation, exactly as a body's shape is.
                if (shape.Offset != vec3(0.0f) || shape.Rotation != quat(1.0f, 0.0f, 0.0f, 0.0f))
                {
                    m_Placed.emplace(Detail::ToJolt(shape.Offset), Detail::ToJolt(shape.Rotation),
                                     primitive);
                    m_Placed->SetEmbedded();
                    m_Shape = &*m_Placed;
                    return;
                }
                m_Shape = primitive;
            }

            /// @brief The shape is embedded in this object, so it never moves.
            QueryShape(const QueryShape&) = delete;
            /// @brief The shape is embedded in this object, so it never moves.
            QueryShape& operator=(const QueryShape&) = delete;

            /// @brief The built shape, or null when a ColliderShape::Mesh collider has no geometry.
            [[nodiscard]] const JPH::Shape* Get() const { return m_Shape; }

        private:
            /// @brief Builds the primitive @p shape names in place, with the bounds BuildShape
            ///        enforces through the solver's settings.
            /// @return The embedded primitive.
            [[nodiscard]] const JPH::Shape* BuildPrimitive(const Collider& shape)
            {
                switch (shape.Shape)
                {
                case ColliderShape::Box:
                {
                    // Jolt refuses a box thinner than its convex radius, so clamp the half extents
                    // up to the default radius rather than asserting inside the solver.
                    const vec3 half = glm::max(shape.Extents, vec3(JPH::cDefaultConvexRadius));
                    m_Box.emplace(Detail::ToJolt(half));
                    m_Box->SetEmbedded();
                    return &*m_Box;
                }
                case ColliderShape::Sphere:
                    VE_ASSERT(shape.Extents.x > 0.0f, "Collider shape is not buildable: a sphere "
                                                      "needs a positive radius");
                    m_Sphere.emplace(shape.Extents.x);
                    m_Sphere->SetEmbedded();
                    return &*m_Sphere;
                case ColliderShape::Capsule:
                    VE_ASSERT(shape.Extents.x > 0.0f && shape.Extents.y >= 0.0f,
                              "Collider shape is not buildable: a capsule needs a positive radius "
                              "and a non-negative half height");
                    // A capsule with no cylinder is a sphere, as the solver's settings make it.
                    if (shape.Extents.y == 0.0f)
                    {
                        m_Sphere.emplace(shape.Extents.x);
                        m_Sphere->SetEmbedded();
                        return &*m_Sphere;
                    }
                    m_Capsule.emplace(shape.Extents.y, shape.Extents.x);
                    m_Capsule->SetEmbedded();
                    return &*m_Capsule;
                case ColliderShape::Mesh:
                    break;
                }
                VE_ASSERT(false, "Unmapped ColliderShape {}", static_cast<u32>(shape.Shape));
                return nullptr;
            }

            /// @brief A box primitive, when the collider is one.
            optional<JPH::BoxShape> m_Box;
            /// @brief A sphere primitive, when the collider is one (or a capsule with no cylinder).
            optional<JPH::SphereShape> m_Sphere;
            /// @brief A capsule primitive, when the collider is one.
            optional<JPH::CapsuleShape> m_Capsule;
            /// @brief The primitive under its local offset and orientation; declared after the
            ///        primitives so it releases its reference to one before that one is destroyed.
            optional<JPH::RotatedTranslatedShape> m_Placed;
            /// @brief Cooked geometry, shared through the world's shape cache.
            JPH::RefConst<JPH::Shape> m_Cached;
            /// @brief The shape the query runs with.
            const JPH::Shape* m_Shape = nullptr;
        };

        /// @brief Least penetration an Overlap counts as an intersection, in metres.
        ///
        /// Two shapes placed exactly face to face report a penetration of a few times 1e-8 from
        /// float rounding alone, so a strict "greater than zero" test reports a neighbour resting
        /// against the volume as inside it. A tenth of a millimetre is far below any collision
        /// detail and far above that noise.
        constexpr f32 OverlapPenetrationEpsilon = 1.0e-4f;

        /// @brief Resolves a solver body handle back to the entity that owns it.
        /// @param native  The world's backend state.
        /// @param bodyId  The body to resolve.
        /// @return The owning entity, or Entity::Null when the body is not one of this world's.
        [[nodiscard]] Entity OwnerOf(const PhysicsWorld::Native& native, const JPH::BodyID& bodyId)
        {
            const auto found = native.BodyOwners.find(bodyId.GetIndexAndSequenceNumber());
            return found == native.BodyOwners.end() ? Entity::Null : found->second;
        }

        /// @brief Sweeps a built shape and resolves the first hit to its entity.
        /// @param native  The world's backend state.
        /// @param built   The shape to sweep.
        /// @param from    The pose the sweep starts at.
        /// @param to      The position the sweep ends at.
        /// @param bodies  Which bodies may be reported.
        /// @param filter  The query's filter, for its layer mask.
        /// @return The first contact, or nullopt when the sweep completes unobstructed.
        [[nodiscard]] optional<ShapeHit> CastShape(const PhysicsWorld::Native& native,
                                                   const JPH::Shape& built, const PhysicsPose& from,
                                                   const dvec3 to, const QueryBodyFilter& bodies,
                                                   const QueryFilter& filter)
        {
            const dvec3 displacement = to - from.Position;
            const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(
                Detail::ToJolt(from.Rotation), Detail::ToJolt(from.Position));
            const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
                &built, JPH::Vec3::sOne(), start, Detail::ToJolt(vec3(displacement)));

            // Hits come back relative to the sweep's start, which keeps the arithmetic near the
            // origin in a world whose extent outruns f32.
            const JPH::RVec3 baseOffset = Detail::ToJolt(from.Position);
            JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
            const LayerMaskFilter layers(filter.Layers);
            native.System.GetNarrowPhaseQuery().CastShape(
                cast, JPH::ShapeCastSettings{}, baseOffset, collector, {}, layers, bodies);
            if (!collector.HadHit())
            {
                return std::nullopt;
            }

            const JPH::ShapeCastResult& hit = collector.mHit;
            const Entity owner = OwnerOf(native, hit.mBodyID2);
            if (owner.IsNull())
            {
                return std::nullopt;
            }

            // mPenetrationAxis points from the swept shape into the body it met, so the outward
            // normal of the hit body is its negation.
            const vec3 axis = Detail::FromJolt(hit.mPenetrationAxis);
            const vec3 normal =
                glm::length(axis) > 0.0f ? -glm::normalize(axis) : vec3(0.0f, 1.0f, 0.0f);

            return ShapeHit{
                .Body = owner,
                .Position = from.Position + dvec3(Detail::FromJolt(hit.mContactPointOn2)),
                .Normal = normal,
                .Fraction = hit.mFraction,
            };
        }

        /// @brief Collects the owner of every body the broad phase reports, through a query filter.
        class BoundsCollector final : public JPH::CollideShapeBodyCollector
        {
        public:
            /// @brief Builds the collector.
            /// @param native  The world's backend state, for body locks and owners.
            /// @param bodies  Which bodies may be reported.
            /// @param out     Destination for each reported body's entity.
            BoundsCollector(const PhysicsWorld::Native& native, const QueryBodyFilter& bodies,
                            vector<Entity>& out)
                : m_Native(native), m_Bodies(bodies), m_Out(out)
            {
            }

            /// @brief Records one body whose bounds reach the query volume.
            /// @param bodyId  The body the broad phase reported.
            void AddHit(const JPH::BodyID& bodyId) override
            {
                if (!m_Bodies.ShouldCollide(bodyId))
                {
                    return;
                }
                {
                    const JPH::BodyLockRead lock(m_Native.System.GetBodyLockInterface(), bodyId);
                    if (!lock.Succeeded() || !m_Bodies.ShouldCollideLocked(lock.GetBody()))
                    {
                        return;
                    }
                }
                const Entity owner = OwnerOf(m_Native, bodyId);
                if (!owner.IsNull())
                {
                    m_Out.emplace_back(owner);
                }
            }

        private:
            /// @brief The world's backend state.
            const PhysicsWorld::Native& m_Native;
            /// @brief Which bodies may be reported.
            const QueryBodyFilter& m_Bodies;
            /// @brief Destination for each reported body's entity.
            vector<Entity>& m_Out;
        };
    }

    optional<RayHit> Raycast(const PhysicsWorld* world, const dvec3 origin, const vec3 direction,
                             const f32 maxDistance, const QueryFilter& filter)
    {
        VE_PROFILE_SCOPE("Physics/Raycast");
        if (world == nullptr || maxDistance <= 0.0f || glm::length(direction) <= 0.0f)
        {
            return std::nullopt;
        }

        const PhysicsWorld::Native& native = world->GetNative();
        const vec3 unit = glm::normalize(direction);
        const JPH::RRayCast ray{Detail::ToJolt(origin), Detail::ToJolt(unit * maxDistance)};

        JPH::RayCastResult hit;
        const LayerMaskFilter layers(filter.Layers);
        const QueryBodyFilter bodies(native, filter);
        if (!native.System.GetNarrowPhaseQuery().CastRay(ray, hit, {}, layers, bodies))
        {
            return std::nullopt;
        }

        const Entity owner = OwnerOf(native, hit.mBodyID);
        if (owner.IsNull())
        {
            return std::nullopt;
        }

        const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
        vec3 normal = -unit;
        const JPH::BodyLockRead lock(native.System.GetBodyLockInterfaceNoLock(), hit.mBodyID);
        if (lock.Succeeded())
        {
            normal = Detail::FromJolt(
                lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
        }

        return RayHit{
            .Body = owner,
            .Position = Detail::FromJolt(point),
            .Normal = normal,
            .Distance = hit.mFraction * maxDistance,
            .Fraction = hit.mFraction,
        };
    }

    optional<ShapeHit> ShapeCast(const PhysicsWorld* world, const Collider& shape,
                                 const PhysicsPose& from, const dvec3 to, const QueryFilter& filter)
    {
        VE_PROFILE_SCOPE("Physics/ShapeCast");
        if (world == nullptr)
        {
            return std::nullopt;
        }

        const QueryShape built(*world, shape, /*sweeping=*/true);
        if (built.Get() == nullptr)
        {
            return std::nullopt;
        }

        const PhysicsWorld::Native& native = world->GetNative();
        const QueryBodyFilter bodies(native, filter);
        return CastShape(native, *built.Get(), from, to, bodies, filter);
    }

    optional<ShapeHit> ShapeCast(const PhysicsWorld* world, const Entity body,
                                 const PhysicsPose& from, const dvec3 to, const QueryFilter& filter)
    {
        VE_PROFILE_SCOPE("Physics/ShapeCast");
        if (world == nullptr)
        {
            return std::nullopt;
        }

        const PhysicsWorld::Native& native = world->GetNative();
        const auto found = native.Bodies.find(body);
        if (found == native.Bodies.end())
        {
            return std::nullopt;
        }
        const Collider& collider = found->second.Shape;
        VE_ASSERT(collider.Shape != ColliderShape::Mesh || collider.Geometry.Get() == nullptr ||
                      !collider.Geometry.Get()->ContainsTriangleMesh(),
                  "ShapeCast was given entity {}, whose body is a triangle-mesh CollisionShape; a "
                  "concave shape has no sweep, so only a convex hull, a primitive, or a compound "
                  "of those can be cast",
                  body.Index);

        // The shape is taken under the body's lock and held by reference, so the sweep itself runs
        // unlocked, as every other query does.
        JPH::RefConst<JPH::Shape> shape;
        {
            const JPH::BodyLockRead lock(native.System.GetBodyLockInterface(), found->second.Id);
            if (!lock.Succeeded())
            {
                return std::nullopt;
            }
            shape = lock.GetBody().GetShape();
        }

        QueryBodyFilter bodies(native, filter);
        bodies.Ignore(found->second.Id);
        return CastShape(native, *shape, from, to, bodies, filter);
    }

    usize Overlap(const PhysicsWorld* world, const Collider& shape, const PhysicsPose& at,
                  const QueryFilter& filter, vector<Entity>& out)
    {
        VE_PROFILE_SCOPE("Physics/Overlap");
        out.clear();
        if (world == nullptr)
        {
            return 0;
        }

        const QueryShape query(*world, shape, /*sweeping=*/false);
        const JPH::Shape* built = query.Get();
        if (built == nullptr)
        {
            return 0;
        }

        const PhysicsWorld::Native& native = world->GetNative();
        const JPH::RMat44 transform =
            JPH::RMat44::sRotationTranslation(Detail::ToJolt(at.Rotation),
                                              Detail::ToJolt(at.Position)) *
            JPH::Mat44::sTranslation(built->GetCenterOfMass());

        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
        const LayerMaskFilter layers(filter.Layers);
        const QueryBodyFilter bodies(native, filter);
        native.System.GetNarrowPhaseQuery().CollideShape(
            built, JPH::Vec3::sOne(), transform, JPH::CollideShapeSettings{},
            Detail::ToJolt(at.Position), collector, {}, layers, bodies);

        for (const JPH::CollideShapeResult& hit : collector.mHits)
        {
            // A body touching the volume from outside is not inside it; only genuine
            // intersection counts as an overlap.
            if (hit.mPenetrationDepth <= OverlapPenetrationEpsilon)
            {
                continue;
            }
            const Entity owner = OwnerOf(native, hit.mBodyID2);
            if (!owner.IsNull())
            {
                out.emplace_back(owner);
            }
        }

        std::ranges::sort(out, [](const Entity a, const Entity b) { return a.Index < b.Index; });
        // The solver reports one hit per touching sub-shape pair, so a body meeting the volume
        // along several faces arrives more than once.
        out.erase(std::ranges::unique(out).begin(), out.end());
        return out.size();
    }

    usize OverlapBounds(const PhysicsWorld* world, const dvec3 center, const f32 radius,
                        const QueryFilter& filter, vector<Entity>& out)
    {
        VE_PROFILE_SCOPE("Physics/OverlapBounds");
        out.clear();
        if (world == nullptr || radius < 0.0f)
        {
            return 0;
        }

        // The broad phase keeps its bounds in f32, rounded outward. The centre rounds to the
        // nearest f32, so the radius grows by that rounding's distance: a body whose bounds
        // reach the exact sphere is never lost to it.
        const vec3 nearest = vec3(center);
        const f64 rounding = glm::length(dvec3(nearest) - center);
        const f32 reach = std::nextafter(static_cast<f32>(static_cast<f64>(radius) + rounding),
                                         std::numeric_limits<f32>::infinity());

        const PhysicsWorld::Native& native = world->GetNative();
        const QueryBodyFilter bodies(native, filter);
        const LayerMaskFilter layers(filter.Layers);
        BoundsCollector collector(native, bodies, out);
        native.System.GetBroadPhaseQuery().CollideSphere(Detail::ToJolt(nearest), reach, collector,
                                                         {}, layers);

        std::ranges::sort(out, [](const Entity a, const Entity b) { return a.Index < b.Index; });
        return out.size();
    }
}
