#include <Veng/Physics/Queries.h>

#include <Veng/Assert.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Physics/PhysicsWorld.h>

#include "PhysicsInternal.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

#include <algorithm>

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
                m_Ignore.reserve(filter.Ignore.size());
                for (const Entity entity : filter.Ignore)
                {
                    const auto found = native.Bodies.find(entity);
                    if (found != native.Bodies.end())
                    {
                        m_Ignore.emplace_back(found->second.Id.GetIndexAndSequenceNumber());
                    }
                }
            }

            /// @brief Whether a body may be reported, by handle alone.
            /// @param bodyId  The body being tested.
            [[nodiscard]] bool ShouldCollide(const JPH::BodyID& bodyId) const override
            {
                return std::ranges::find(m_Ignore, bodyId.GetIndexAndSequenceNumber()) ==
                       m_Ignore.end();
            }

            /// @brief Whether a body may be reported, once the solver holds it.
            /// @param body  The body being tested.
            [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override
            {
                return m_IncludeSensors || !body.IsSensor();
            }

        private:
            /// @brief Index-and-sequence values of the bodies the query skips.
            vector<u32> m_Ignore;
            /// @brief Whether a sensor body may be reported.
            bool m_IncludeSensors;
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

        /// @brief Builds the solver shape a query's Collider describes, asserting it is castable.
        /// @param shape     The collider to build.
        /// @param sweeping  Whether the shape is about to be swept, which a concave shape cannot be.
        /// @return The built shape, or null when a ColliderShape::Mesh collider has no geometry.
        [[nodiscard]] JPH::RefConst<JPH::Shape> BuildQueryShape(const Collider& shape,
                                                                const bool sweeping)
        {
            if (sweeping && shape.Shape == ColliderShape::Mesh)
            {
                const CollisionShape* geometry = shape.Geometry.Get();
                VE_ASSERT(geometry == nullptr || !geometry->ContainsTriangleMesh(),
                          "ShapeCast was given a triangle-mesh CollisionShape; a concave shape has "
                          "no sweep, so only a convex hull, a primitive, or a compound of those "
                          "can be cast");
            }
            return Detail::BuildShape(shape);
        }
    }

    optional<RayHit> Raycast(const PhysicsWorld* world, const dvec3 origin, const vec3 direction,
                             const f32 maxDistance, const QueryFilter& filter)
    {
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
        if (world == nullptr)
        {
            return std::nullopt;
        }

        const JPH::RefConst<JPH::Shape> built = BuildQueryShape(shape, /*sweeping=*/true);
        if (built == nullptr)
        {
            return std::nullopt;
        }

        const PhysicsWorld::Native& native = world->GetNative();
        const dvec3 displacement = to - from.Position;
        const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(Detail::ToJolt(from.Rotation),
                                                                    Detail::ToJolt(from.Position));
        const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
            built, JPH::Vec3::sOne(), start, Detail::ToJolt(vec3(displacement)));

        // Hits come back relative to the sweep's start, which keeps the arithmetic near the origin
        // in a world whose extent outruns f32.
        const JPH::RVec3 baseOffset = Detail::ToJolt(from.Position);
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        const LayerMaskFilter layers(filter.Layers);
        const QueryBodyFilter bodies(native, filter);
        native.System.GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings{}, baseOffset,
                                                      collector, {}, layers, bodies);
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

    usize Overlap(const PhysicsWorld* world, const Collider& shape, const PhysicsPose& at,
                  const QueryFilter& filter, vector<Entity>& out)
    {
        out.clear();
        if (world == nullptr)
        {
            return 0;
        }

        const JPH::RefConst<JPH::Shape> built = BuildQueryShape(shape, /*sweeping=*/false);
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
}
