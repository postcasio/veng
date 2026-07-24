#include <Veng/Scene/VehicleSystem.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/Queries.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Interaction.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Sockets.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Scene/Vehicle.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace Veng
{
    namespace
    {
        /// @brief The outcome of handling one InteractRequest, applied with the request idiom.
        enum class HandleOutcome
        {
            /// @brief Carried out; the request is removed.
            Handled,
            /// @brief The operation failed; the request is marked Failed and held a frame.
            Failed,
        };

        /// @brief Solid layers an exit is validated against — a character may not land inside these.
        constexpr u32 SolidLayers = (1u << static_cast<u32>(PhysicsLayer::Static)) |
                                    (1u << static_cast<u32>(PhysicsLayer::Moving));

        /// @brief Extracts a rotation from a world matrix, tolerating a uniform scale on its basis.
        [[nodiscard]] quat RotationOf(const mat4& world)
        {
            return glm::quat_cast(mat3(glm::normalize(vec3(world[0])),
                                       glm::normalize(vec3(world[1])),
                                       glm::normalize(vec3(world[2]))));
        }

        /// @brief The seat entity of @p vehicle occupied by @p character, or Null when it occupies none.
        [[nodiscard]] Entity OccupiedSeat(const Scene& scene, const Vehicle& vehicle,
                                          const Entity character)
        {
            for (const Entity seat : vehicle.Seats)
            {
                if (seat.IsNull() || !scene.IsAlive(seat))
                {
                    continue;
                }
                const auto* data = scene.TryGet<VehicleSeat>(seat);
                if (data != nullptr && data->Occupant == character)
                {
                    return seat;
                }
            }
            return Entity::Null;
        }

        /// @brief The first empty seat of @p vehicle in preference order, or Null when all are taken.
        [[nodiscard]] Entity FreeSeat(const Scene& scene, const Vehicle& vehicle)
        {
            for (const Entity seat : vehicle.Seats)
            {
                if (seat.IsNull() || !scene.IsAlive(seat))
                {
                    continue;
                }
                const auto* data = scene.TryGet<VehicleSeat>(seat);
                if (data != nullptr && data->Occupant.IsNull())
                {
                    return seat;
                }
            }
            return Entity::Null;
        }

        /// @brief The seat whose Possesses names @p pawn, or Null — the seat controlling that pawn.
        [[nodiscard]] Entity ControllingSeat(const Scene& scene, const Entity pawn)
        {
            for (auto [entity, possesses] : scene.View<Possesses>())
            {
                if (possesses.Pawn == pawn)
                {
                    return entity;
                }
            }
            return Entity::Null;
        }

        HandleOutcome Enter(Scene& scene, const Entity vehicleEntity, const Entity seatEntity,
                            const Entity character, string& error)
        {
            // Snapshot the seat fields before any structural change, so no reference is held across
            // the parenting and physics edits below.
            const VehicleSeat& seat = scene.Get<VehicleSeat>(seatEntity);
            const string socketName = seat.Socket;
            const bool isDriver = seat.IsDriver;
            const AssetHandle<InputMappingContext> context = seat.Context;

            // 1. Disable the controller and remove its capsule.
            const bool hasController = scene.Has<CharacterController>(character);
            if (hasController)
            {
                scene.Get<CharacterController>(character).Enabled = false;
            }
            PhysicsWorld* world = scene.GetPhysicsWorld();
            if (world != nullptr && world->HasCharacter(character))
            {
                world->DestroyCharacter(character);
            }

            // 2. Parent the character to the vehicle at the seat's socket.
            if (!AttachToSocket(scene, character, vehicleEntity, socketName))
            {
                // Leave the character no worse than it started: re-enable so it is not stranded.
                if (hasController)
                {
                    scene.Get<CharacterController>(character).Enabled = true;
                }
                error = "seat socket '" + socketName + "' unavailable";
                return HandleOutcome::Failed;
            }

            // 3. Set the occupant.
            scene.Get<VehicleSeat>(seatEntity).Occupant = character;

            Seated seated{.Vehicle = vehicleEntity, .Seat = seatEntity};

            // 4 & 5. A driver seat takes control of the vehicle and its input scheme.
            if (isDriver)
            {
                const Entity controlling = ControllingSeat(scene, character);
                if (!controlling.IsNull())
                {
                    seated.ControllingSeat = controlling;
                    scene.Get<Possesses>(controlling).Pawn = vehicleEntity;

                    if (scene.Has<InputContextStack>(controlling))
                    {
                        auto& stack = scene.Get<InputContextStack>(controlling);
                        // Snapshot the whole stack so exit restores it verbatim, then pop the
                        // character's top context and push the vehicle's.
                        seated.StashedContexts = stack.Active;
                        if (!stack.Active.empty())
                        {
                            stack.Active.pop_back();
                        }
                        // Push the vehicle's context when the seat authors one — named by AssetId, or
                        // a resident runtime handle whose id is zero. An unset (empty) handle is
                        // neither, so a seat leaving the scheme unchanged pushes nothing.
                        if (context.Id().Value != 0 || context.IsLoaded())
                        {
                            stack.Active.push_back(context);
                        }
                    }
                }
            }

            scene.Add<Seated>(character, std::move(seated));
            return HandleOutcome::Handled;
        }

        HandleOutcome Exit(Scene& scene, const Entity vehicleEntity, const Entity seatEntity,
                           const Entity character, string& error)
        {
            const string exitName = scene.Get<VehicleSeat>(seatEntity).ExitSocket;
            const MeshSocket* exitSocket = FindMeshSocket(scene, vehicleEntity, exitName);
            if (exitSocket == nullptr)
            {
                error = "exit socket '" + exitName + "' unavailable";
                return HandleOutcome::Failed;
            }

            const mat4 vehicleWorld = WorldMatrix(scene, vehicleEntity);
            const mat4 socketLocal = glm::translate(mat4(1.0f), exitSocket->Position) *
                                     glm::mat4_cast(exitSocket->Rotation);
            const mat4 exitWorld = vehicleWorld * socketLocal;
            const vec3 exitPosition = vec3(exitWorld[3]);
            const quat exitRotation = RotationOf(exitWorld);
            const vec3 exitUp = exitRotation * vec3(0.0f, 1.0f, 0.0f);

            PhysicsWorld* world = scene.GetPhysicsWorld();

            // Validate before performing: a blocked exit socket reports rather than placing the
            // character inside geometry. The character's own capsule is swept at the exit pose,
            // ignoring the vehicle it is leaving and the character itself.
            if (world != nullptr && scene.Has<CharacterController>(character))
            {
                const auto& controller = scene.Get<CharacterController>(character);
                const f32 cylinderHalf =
                    std::max(0.0f, controller.Height * 0.5f - controller.Radius);
                const Collider capsule{.Shape = ColliderShape::Capsule,
                                       .Extents = vec3(controller.Radius, cylinderHalf, 0.0f)};
                // The controller capsule stands on the entity origin; the query capsule is centred, so
                // it is lifted half a height along the exit up to line the two up.
                const PhysicsPose at{.Position =
                                         dvec3(exitPosition + exitUp * (controller.Height * 0.5f)),
                                     .Rotation = exitRotation};
                const std::array<Entity, 2> ignore{vehicleEntity, character};
                vector<Entity> hits;
                if (Overlap(world, capsule, at,
                            QueryFilter{.Layers = SolidLayers, .Ignore = ignore}, hits) > 0)
                {
                    error = "exit blocked";
                    return HandleOutcome::Failed;
                }
            }

            // Perform the exit — the exact inverse of entry, in reverse order.
            Seated seated;
            const bool hadSeated = scene.Has<Seated>(character);
            if (hadSeated)
            {
                seated = scene.Get<Seated>(character);
            }

            const bool controllingAlive =
                !seated.ControllingSeat.IsNull() && scene.IsAlive(seated.ControllingSeat);

            // 5'. Restore the input context.
            if (controllingAlive && scene.Has<InputContextStack>(seated.ControllingSeat))
            {
                scene.Get<InputContextStack>(seated.ControllingSeat).Active =
                    seated.StashedContexts;
            }
            // 4'. Restore possession.
            if (controllingAlive && scene.Has<Possesses>(seated.ControllingSeat))
            {
                scene.Get<Possesses>(seated.ControllingSeat).Pawn = character;
            }
            // 3'. Clear the occupant.
            scene.Get<VehicleSeat>(seatEntity).Occupant = Entity::Null;
            // 2'. Detach and place at the exit socket.
            scene.SetParent(character, Entity::Null);
            Transform& transform = scene.Has<Transform>(character)
                                       ? scene.Get<Transform>(character)
                                       : scene.Add<Transform>(character);
            transform.Position = exitPosition;
            transform.Rotation = exitRotation;
            transform.Scale = vec3(1.0f);
            // 1'. Re-enable the controller, seeded with the vehicle's current velocity so leaving a
            //     moving vehicle carries its motion rather than starting from rest.
            if (scene.Has<CharacterController>(character))
            {
                auto& controller = scene.Get<CharacterController>(character);
                controller.Enabled = true;
                if (world != nullptr)
                {
                    const vec3 vehicleVelocity = world->GetLinearVelocity(vehicleEntity);
                    world->CreateCharacter(
                        character, controller,
                        PhysicsPose{.Position = dvec3(exitPosition), .Rotation = exitRotation});
                    world->SetCharacterVelocity(character, vehicleVelocity);
                }
            }

            if (hadSeated)
            {
                scene.Remove<Seated>(character);
            }
            return HandleOutcome::Handled;
        }

        HandleOutcome Handle(Scene& scene, const Entity vehicleEntity, const Entity interactor,
                             string& error)
        {
            if (interactor.IsNull() || !scene.IsAlive(interactor))
            {
                error = "interact request names no live interactor";
                return HandleOutcome::Failed;
            }

            const Vehicle& vehicle = scene.Get<Vehicle>(vehicleEntity);
            const Entity occupied = OccupiedSeat(scene, vehicle, interactor);
            if (!occupied.IsNull())
            {
                return Exit(scene, vehicleEntity, occupied, interactor, error);
            }

            const Entity free = FreeSeat(scene, vehicle);
            if (free.IsNull())
            {
                error = "vehicle has no free seat";
                return HandleOutcome::Failed;
            }
            return Enter(scene, vehicleEntity, free, interactor, error);
        }
    }

    void VehicleSystem::OnUpdate(Scene& scene, f32 /*delta*/, const SystemContext& /*context*/)
    {
        // Gather the vehicles carrying a request before touching anything: enter/exit make structural
        // changes, which must not run inside a live query.
        vector<Entity> holders;
        for (auto [entity, vehicle, request] : scene.View<Vehicle, InteractRequest>())
        {
            holders.push_back(entity);
        }

        for (const Entity holder : holders)
        {
            if (!scene.IsAlive(holder) || !scene.Has<InteractRequest>(holder))
            {
                continue;
            }

            // A held-Failed request has had its one-frame observation window; retire it now.
            if (scene.Get<InteractRequest>(holder).Status == RequestStatus::Failed)
            {
                scene.Remove<InteractRequest>(holder);
                continue;
            }

            const Entity interactor = scene.Get<InteractRequest>(holder).Interactor;
            string error;
            const HandleOutcome outcome = Handle(scene, holder, interactor, error);

            // Enter/exit may have removed the vehicle's request through a structural change; re-check.
            if (!scene.IsAlive(holder) || !scene.Has<InteractRequest>(holder))
            {
                continue;
            }
            switch (outcome)
            {
            case HandleOutcome::Handled:
            {
                scene.Remove<InteractRequest>(holder);
                break;
            }
            case HandleOutcome::Failed:
            {
                auto& request = scene.Get<InteractRequest>(holder);
                request.Status = RequestStatus::Failed;
                request.Error = std::move(error);
                break;
            }
            }
        }
    }
}
