#include <Veng/Scene/InteractionSystem.h>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/PoseResolver.h>
#include <Veng/Physics/Queries.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Interaction.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Veng
{
    namespace
    {
        /// @brief One interactor resolved this tick, snapshotted before any component is written.
        struct ResolvedInteractor
        {
            /// @brief The entity carrying the Interactor.
            Entity Owner;
            /// @brief The best Interactable found, or Entity::Null when none qualifies.
            Entity Focused;
        };

        /// @brief Radius of the probe that finds the bodies enclosing an interactor, in metres.
        ///
        /// Small enough that only a body actually containing the interactor's origin reports, and
        /// nonzero because a degenerate shape has no overlap.
        constexpr f32 EnclosureProbeRadius = 0.01f;
    }

    void InteractionSystem::OnUpdate(Scene& scene, f32 /*delta*/, const SystemContext& /*context*/)
    {
        const PhysicsWorld* world = scene.GetPhysicsWorld();

        // Resolve against const reads first, then write Focused: writing a field is not a structural
        // change, but gathering keeps the query and the write from interleaving.
        const Scene& readScene = scene;
        vector<ResolvedInteractor> resolved;
        vector<Entity> candidates;
        vector<Entity> enclosing;
        for (auto [entity, transform, interactor] : readScene.View<Transform, Interactor>())
        {
            Entity best = Entity::Null;
            f32 bestAngle = 0.0f;
            f32 bestDistance = 0.0f;

            if (world != nullptr)
            {
                // Every pose is resolved in the physics world's frame, so the sweep, the cone and the
                // range filters all agree with the space the overlap test runs in — including when
                // the consumer's Transform chain is offset from the solver's origin.
                const PhysicsPose interactorPose = ResolvePhysicsPose(readScene, entity);
                const vec3 forward =
                    glm::normalize(interactorPose.Rotation * vec3(0.0f, 0.0f, -1.0f));

                // A sphere of the interactor's Reach gathers every nearby body; the cone and
                // per-interactable Range filters narrow it. The interactor itself is ignored so a
                // pawn's own collider is never its own candidate.
                const std::array<Entity, 1> ignore{entity};
                const Collider sphere{.Shape = ColliderShape::Sphere,
                                      .Extents = vec3(interactor.Reach)};
                Overlap(world, sphere, PhysicsPose{.Position = interactorPose.Position},
                        QueryFilter{.Ignore = ignore, .IncludeSensors = true}, candidates);

                // The bodies enclosing the interactor, resolved at most once per interactor and only
                // when the cone actually rejects something — see the cone test below.
                bool probedEnclosure = false;

                for (const Entity candidate : candidates)
                {
                    const auto* interactable = readScene.TryGet<Interactable>(candidate);
                    if (interactable == nullptr || !interactable->Enabled)
                    {
                        continue;
                    }

                    // Differenced at double precision before narrowing: an offset frame puts both
                    // poses far from the origin, where the separation is the only quantity f32 can
                    // still carry.
                    const PhysicsPose candidatePose = ResolvePhysicsPose(readScene, candidate);
                    const vec3 toCandidate = vec3(candidatePose.Position - interactorPose.Position);
                    const f32 distance = glm::length(toCandidate);
                    if (distance > interactable->Range)
                    {
                        continue;
                    }

                    // A candidate all but on top of the interactor is trivially in cone; otherwise its
                    // bearing must fall within the half-angle.
                    f32 angle = 0.0f;
                    if (distance > 1.0e-4f)
                    {
                        const vec3 direction = toCandidate / distance;
                        angle = std::acos(glm::clamp(glm::dot(forward, direction), -1.0f, 1.0f));
                        if (angle > interactor.ConeAngle)
                        {
                            // A body the interactor stands *inside* is trivially in cone too, for the
                            // same reason a coincident one is: the bearing is measured to the
                            // candidate's origin, which for an enclosing body is an interior point
                            // that may lie anywhere — including straight behind. Without this an
                            // interactable large enough to be entered is unfocusable from within it at
                            // any orientation. It keeps its true bearing for the ranking below, so
                            // anything genuinely looked at still wins over the room one is in.
                            if (!probedEnclosure)
                            {
                                const Collider probe{.Shape = ColliderShape::Sphere,
                                                     .Extents = vec3(EnclosureProbeRadius)};
                                Overlap(world, probe,
                                        PhysicsPose{.Position = interactorPose.Position},
                                        QueryFilter{.Ignore = ignore, .IncludeSensors = true},
                                        enclosing);
                                probedEnclosure = true;
                            }
                            if (std::ranges::find(enclosing, candidate) == enclosing.end())
                            {
                                continue;
                            }
                        }
                    }

                    // Best is the smallest bearing; distance breaks a tie, so the thing most directly
                    // looked at wins over a closer one off to the side.
                    const bool better =
                        best.IsNull() || angle < bestAngle - 1.0e-4f ||
                        (std::abs(angle - bestAngle) <= 1.0e-4f && distance < bestDistance);
                    if (better)
                    {
                        best = candidate;
                        bestAngle = angle;
                        bestDistance = distance;
                    }
                }
            }

            resolved.emplace_back(ResolvedInteractor{.Owner = entity, .Focused = best});
        }

        for (const ResolvedInteractor& entry : resolved)
        {
            scene.Get<Interactor>(entry.Owner).Focused = entry.Focused;
        }
    }
}
