#include <Veng/Physics/CharacterMovementSystem.h>

#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace Veng
{
    namespace
    {
        /// @brief Fastest the capsule reorients toward a new up, in radians per second.
        ///
        /// A per-tick cap on how far the up may swing. Ordinary walking around a curved habitat
        /// changes the up far slower than this, so the cap never engages there and the up is exact;
        /// it engages only for a large field discontinuity that survives the field's own blend,
        /// slewing it smoothly rather than snapping.
        constexpr f32 MaxReorientRate = 4.0f;

        /// @brief One character the system simulates this tick, snapshotted to avoid repeated lookups.
        struct Simulated
        {
            /// @brief The entity owning the capsule.
            Entity Owner;
            /// @brief Its CharacterController settings this tick.
            CharacterController Controller;
            /// @brief Its world pose at the start of the tick (last tick's resolved capsule pose).
            PhysicsPose Pose;
            /// @brief Its Intent this tick, or a zero command when it carries none.
            Intent Command;
            /// @brief Last tick's resolved up, or zero when the character has no state yet.
            vec3 LastUp;
            /// @brief Last tick's accumulated air time.
            f32 LastAirTime = 0.0f;
            /// @brief Whether the entity already carries a CharacterState.
            bool HasState = false;
            /// @brief Whether the entity is a rolled-back predicted character.
            bool Predicted = false;
        };

        /// @brief Rotates @p from toward @p to by at most @p maxAngle radians.
        ///
        /// Snaps to @p to when they are within @p maxAngle (the ordinary case), else takes a
        /// @p maxAngle step along the shortest arc.
        /// @param from      The current unit up.
        /// @param to        The target unit up.
        /// @param maxAngle  The largest rotation permitted this tick.
        /// @return The new unit up.
        [[nodiscard]] vec3 RotateTowards(const vec3 from, const vec3 to, const f32 maxAngle)
        {
            const vec3 a = glm::normalize(from);
            const vec3 b = glm::normalize(to);
            const f32 angle = std::acos(glm::clamp(glm::dot(a, b), -1.0f, 1.0f));
            if (angle <= maxAngle || angle < 1.0e-6f)
            {
                return b;
            }
            const vec3 axis = glm::cross(a, b);
            const f32 axisLength = glm::length(axis);
            if (axisLength < 1.0e-6f)
            {
                // Antiparallel: no unique shortest arc, so snapping is as good as any choice and
                // this only arises when the field itself flips, which the caller does not author.
                return b;
            }
            return glm::normalize(glm::angleAxis(maxAngle, axis / axisLength) * a);
        }
    }

    void CharacterMovementSystem::OnUpdate(Scene& scene, const f32 delta,
                                           const SystemContext& context)
    {
        PhysicsWorld* world = scene.GetPhysicsWorld();
        if (world == nullptr)
        {
            return;
        }

        vector<GravitySourceInstance> sources;
        GatherGravitySources(scene, sources);

        // Gather through the const view: adding a CharacterState is a structural change and so
        // cannot happen inside the iteration that finds the characters needing one.
        const Scene& readScene = scene;
        vector<Simulated> simulated;
        for (auto [entity, transform, controller] :
             readScene.View<Transform, CharacterController>())
        {
            // A disabled controller is not simulated: it is left out of the simulated set, so the
            // orphan sweep below releases its capsule and its Transform is never overwritten — the
            // state a seated character (parented to its vehicle) needs.
            if (!controller.Enabled)
            {
                continue;
            }
            // Advance only the characters this peer owns — a client never fights the snapshot
            // stream for a Server/Remote-tier character.
            if (!HasAuthority(context, scene, entity))
            {
                continue;
            }
            Simulated entry{
                .Owner = entity,
                .Controller = controller,
                .Pose = PhysicsPose{.Position = dvec3(transform.Position),
                                    .Rotation = transform.Rotation},
                .Predicted = readScene.Has<Predicted>(entity),
            };
            if (const auto* command = readScene.TryGet<Intent>(entity))
            {
                entry.Command = *command;
            }
            if (const auto* state = readScene.TryGet<CharacterState>(entity))
            {
                entry.LastUp = state->Up;
                entry.LastAirTime = state->AirTime;
                entry.HasState = true;
            }
            simulated.emplace_back(entry);
        }

        for (const Simulated& entry : simulated)
        {
            if (!entry.HasState)
            {
                scene.Add<CharacterState>(entry.Owner);
            }
        }

        // The components are the authority and the capsules are their shadow: bring each simulated
        // character's capsule into line, then destroy the capsules whose entity no longer owns one.
        for (const Simulated& entry : simulated)
        {
            world->CreateCharacter(entry.Owner, entry.Controller, entry.Pose);
        }

        vector<Entity> resident;
        world->GetCharacterEntities(resident);
        for (const Entity entity : resident)
        {
            const bool active = std::ranges::any_of(simulated, [entity](const Simulated& entry)
                                                    { return entry.Owner == entity; });
            if (!active)
            {
                world->DestroyCharacter(entity);
            }
        }

        for (const Simulated& entry : simulated)
        {
            const vec3 position = vec3(entry.Pose.Position);
            const vec3 acceleration = EvaluateGravity(sources, position);
            const f32 gravityMagnitude = glm::length(acceleration);

            // The up is the negated gravity; with no source the character keeps its last up and
            // floats. A never-yet-resolved character seeds its up from its authored orientation.
            const vec3 lastUp = entry.HasState && glm::length(entry.LastUp) > 1.0e-4f
                                    ? glm::normalize(entry.LastUp)
                                    : glm::normalize(entry.Pose.Rotation * vec3(0.0f, 1.0f, 0.0f));
            const vec3 targetUp =
                gravityMagnitude > 1.0e-6f ? -acceleration / gravityMagnitude : lastUp;
            const vec3 up = RotateTowards(lastUp, targetUp, MaxReorientRate * delta);

            const bool running =
                (entry.Command.Actions & static_cast<u32>(CharacterAction::Run)) != 0;
            const f32 speed = running ? entry.Controller.RunSpeed : entry.Controller.WalkSpeed;
            // Move is pawn-local; rotate it into the character's facing, then UpdateCharacter
            // projects it onto the ground plane defined by up.
            const vec3 desired = (entry.Pose.Rotation * entry.Command.Move) * speed;
            const bool jump =
                (entry.Command.Actions & static_cast<u32>(CharacterAction::Jump)) != 0;

            // A predicted character's Transform is reconciled against the server: on a correction the
            // reconcile writes the authoritative pose here and clears nothing on the capsule, so the
            // capsule is re-seated onto it before this tick's sweep. In steady prediction the
            // Transform already equals the capsule's own pose (the mover wrote it last tick), so the
            // re-seat is idempotent; after a rollback restore it is what makes the correction take.
            if (entry.Predicted)
            {
                world->SetCharacterPose(entry.Owner, entry.Pose);
            }

            const CharacterMoveResult result =
                world->UpdateCharacter(entry.Owner,
                                       CharacterMoveInput{
                                           .Up = up,
                                           .DesiredPlanarVelocity = desired,
                                           .Jump = jump,
                                           .JumpSpeed = entry.Controller.JumpImpulse,
                                           .GravityMagnitude = gravityMagnitude,
                                           .AirControl = entry.Controller.AirControl,
                                       },
                                       delta);

            auto& state = scene.Get<CharacterState>(entry.Owner);
            state.Grounded = result.Grounded;
            state.GroundNormal = result.GroundNormal;
            state.Up = up;
            const vec3 verticalVelocity = up * glm::dot(result.LinearVelocity, up);
            state.PlanarSpeed = glm::length(result.LinearVelocity - verticalVelocity);
            state.VerticalSpeed = glm::dot(result.LinearVelocity, up);
            state.AirTime = result.Grounded ? 0.0f : entry.LastAirTime + delta;
            state.GroundEntity = result.GroundEntity;

            auto& transform = scene.Get<Transform>(entry.Owner);
            transform.Position = vec3(result.Position);
            transform.Rotation = result.Rotation;
        }
    }

    void CharacterMovementSystem::OnStop(Scene& scene, const SystemContext&)
    {
        PhysicsWorld* world = scene.GetPhysicsWorld();
        if (world == nullptr)
        {
            return;
        }
        vector<Entity> resident;
        world->GetCharacterEntities(resident);
        for (const Entity entity : resident)
        {
            world->DestroyCharacter(entity);
        }
    }
}
