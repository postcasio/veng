// The vehicle seam: entering and leaving a Vehicle through the builtin VehicleSystem. Seat
// placement is mesh sockets; entering disables the character's controller, parents it to the seat,
// re-points the driver seat's possession at the vehicle and swaps its input context; leaving is the
// exact inverse, validated against the exit socket and seeding the vehicle's velocity. LocalControl
// follows through the generic engine reconcile, with no vehicle code in the marker path. Pure CPU —
// a headless Scene, an AssetManager for the socketed mesh, and a solver; no GPU.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Input.h>
#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/CharacterMovementSystem.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/PoseResolver.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Interaction.h>
#include <Veng/Scene/LocalControl.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Sockets.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Scene/Vehicle.h>
#include <Veng/Scene/VehicleSystem.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A solver frame anchored away from the Transform chain's origin. The magnitude is irrelevant to
    // the mechanism — it need only exceed the exit capsule, so that a validation run in the wrong
    // frame misses what stands in the right one.
    constexpr dvec3 SolverOrigin(1000.0, 0.0, -2000.0);

    struct ContextStorage
    {
        Input HeadlessInput{nullptr};
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
            };
        }
    };

    // A vehicle mesh carrying a "Seat" and an "Exit" socket at authored local positions. The list is
    // sorted by name ("Exit" < "Seat"), as the cook emits and Mesh::FindSocket's binary search needs.
    Ref<Mesh> VehicleMesh(const vec3 seatLocal, const vec3 exitLocal)
    {
        return Mesh::Create(MeshInfo{
            .Name = "vehicle",
            .Sockets =
                {
                    MeshSocket{.Name = "Exit", .Position = exitLocal},
                    MeshSocket{.Name = "Seat", .Position = seatLocal},
                },
        });
    }

    struct VehicleScene
    {
        Renderer::Context Context;
        TaskSystem Tasks;
        TypeRegistry Types;
        Unique<AssetManager> Assets;
        Unique<Scene> World;

        // Two resident input contexts (empty, distinct resources) a controlling seat holds before and
        // while driving. Adopted rather than Load-ed so their identity is a stable resource pointer,
        // not an async resolution that may fail and empty the handle.
        AssetHandle<InputMappingContext> CharacterContext;
        AssetHandle<InputMappingContext> VehicleContext;

        VehicleScene()
        {
            RegisterBuiltinTypes(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));
            CharacterContext =
                Assets->Adopt<InputMappingContext>(InputMappingContext::Create({}, {}));
            VehicleContext =
                Assets->Adopt<InputMappingContext>(InputMappingContext::Create({}, {}));
        }

        Entity AddFloor()
        {
            const Entity floor = World->CreateEntity();
            World->Add<Transform>(floor, Transform{.Position = vec3(0.0f, -0.5f, 0.0f)});
            World->Add<RigidBody>(
                floor, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
            World->Add<Collider>(
                floor, Collider{.Shape = ColliderShape::Box, .Extents = vec3(20.0f, 0.5f, 20.0f)});
            return floor;
        }

        void AddUniformGravity()
        {
            const Entity field = World->CreateEntity();
            World->Add<Transform>(field, Transform{});
            World->Add<GravitySource>(field,
                                      GravitySource{.Kind = GravityKind::Uniform,
                                                    .Direction = vec3(0.0f, -1.0f, 0.0f),
                                                    .Magnitude = 9.81f,
                                                    .Bounds = Region{.HalfExtents = vec3(100.0f)}});
        }

        // A vehicle at `position` with one seat entity. Returns { vehicle, seat }.
        std::array<Entity, 2> AddVehicle(const vec3 position, const vec3 seatLocal,
                                         const vec3 exitLocal, const bool driver,
                                         const bool withBody)
        {
            const Entity vehicle = World->CreateEntity();
            World->Add<Transform>(vehicle, Transform{.Position = position});
            World->Add<MeshRenderer>(vehicle, MeshRenderer{.Mesh = Assets->Adopt<Mesh>(
                                                               VehicleMesh(seatLocal, exitLocal))});
            if (withBody)
            {
                World->Add<RigidBody>(vehicle, RigidBody{.Motion = MotionType::Kinematic,
                                                         .Layer = PhysicsLayer::Moving});
                World->Add<Collider>(vehicle,
                                     Collider{.Shape = ColliderShape::Box, .Extents = vec3(1.0f)});
            }

            const Entity seat = World->CreateEntity();
            World->Add<VehicleSeat>(seat, VehicleSeat{.Socket = "Seat",
                                                      .IsDriver = driver,
                                                      .ExitSocket = "Exit",
                                                      .Context = VehicleContext});
            World->Add<Vehicle>(vehicle, Vehicle{.Seats = {seat}});
            return {vehicle, seat};
        }

        Entity AddCharacter(const vec3 position)
        {
            const Entity character = World->CreateEntity();
            World->Add<Transform>(character, Transform{.Position = position});
            World->Add<CharacterController>(character, CharacterController{});
            return character;
        }

        // A controlling seat possessing `pawn`, holding just the character input context.
        Entity AddControllingSeat(const Entity pawn)
        {
            const Entity seat = World->CreateEntity();
            World->Add<Possesses>(seat, Possesses{.Pawn = pawn});
            World->Add<InputContextStack>(seat, InputContextStack{.Active = {CharacterContext}});
            return seat;
        }

        // Where the engine last reported a resolved placement, and for whom — the record a consumer
        // holding its authority outside the Transform keeps.
        dvec3 PlacedPosition{0.0};
        Entity PlacedEntity = Entity::Null;

        // Projects the Transform chain into the offset solver frame and takes the write-back over,
        // so the engine never touches the character's f32 Transform.
        void InstallOffsetResolver()
        {
            World->SetPhysicsPoseResolver(CreateUnique<PhysicsPoseResolver>(PhysicsPoseResolver{
                .Resolve =
                    [](const Scene& scene, const Entity entity, const mat4& localOffset)
                {
                    PhysicsPose pose = DefaultResolvePhysicsPose(scene, entity, localOffset);
                    pose.Position += SolverOrigin;
                    return pose;
                },
                .Place =
                    [this](Scene&, const Entity entity, const PhysicsPose& pose)
                {
                    PlacedEntity = entity;
                    PlacedPosition = pose.Position;
                },
            }));
        }

        // A solid box standing at a position in the solver's frame, its Transform bound to nothing so
        // PhysicsPose is the only channel to the solver.
        Entity AddSolverFrameObstruction(const dvec3 solverPosition, const vec3 halfExtents)
        {
            const Entity entity = World->CreateEntity();
            World->Add<RigidBody>(entity, RigidBody{.Motion = MotionType::Static,
                                                    .Layer = PhysicsLayer::Static,
                                                    .SyncTransform = false});
            World->Add<Collider>(entity,
                                 Collider{.Shape = ColliderShape::Box, .Extents = halfExtents});
            World->Add<PhysicsPose>(entity, PhysicsPose{.Position = solverPosition});
            return entity;
        }

        void Interact(const Entity vehicle, const Entity interactor)
        {
            World->Add<InteractRequest>(vehicle, InteractRequest{.Interactor = interactor});
            ContextStorage storage;
            VehicleSystem system;
            system.OnUpdate(*World, FixedStep, storage.Make());
        }
    };
}

TEST_CASE("Entering seats the character, disables its controller, and drives possession and input")
{
    VehicleScene fixture;
    Scene& world = *fixture.World;
    const auto [vehicle, seat] = fixture.AddVehicle(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f),
                                                    vec3(0.0f, -0.9f, 0.0f), true, false);
    const Entity character = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
    const Entity controlling = fixture.AddControllingSeat(character);

    fixture.Interact(vehicle, character);

    // The request was handled (removed), the seat is occupied, and the character is parented to the
    // vehicle at the seat socket's world place.
    CHECK_FALSE(world.Has<InteractRequest>(vehicle));
    CHECK(world.Get<VehicleSeat>(seat).Occupant == character);
    CHECK(world.GetParent(character) == vehicle);
    CHECK(world.Has<Seated>(character));
    const vec3 seatWorld = vec3(WorldMatrix(world, character)[3]);
    CHECK(seatWorld.y == doctest::Approx(1.5f).epsilon(0.001));

    // The controller is disabled and possession + input context have swapped to the vehicle.
    CHECK_FALSE(world.Get<CharacterController>(character).Enabled);
    CHECK(world.Get<Possesses>(controlling).Pawn == vehicle);
    const auto& active = world.Get<InputContextStack>(controlling).Active;
    REQUIRE(active.size() == 1);
    CHECK(active[0].Get() == fixture.VehicleContext.Get());
}

TEST_CASE(
    "Entering moves LocalControl to the vehicle through the generic reconcile, with no vehicle "
    "code in the marker path")
{
    VehicleScene fixture;
    Scene& world = *fixture.World;
    const auto [vehicle, seat] = fixture.AddVehicle(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f),
                                                    vec3(0.0f, -0.9f, 0.0f), true, false);
    const Entity character = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
    const Entity controlling = fixture.AddControllingSeat(character);
    const std::array<Entity, 1> presenting{controlling};

    // Before entering, the marker resolves to the character. ReconcileLocalControl is the engine's
    // whole marker lifecycle; the VehicleSystem never touches it.
    ReconcileLocalControl(world, presenting);
    CHECK(ResolveLocalControlledPawn(world, controlling) == character);

    fixture.Interact(vehicle, character);

    // The only thing that changed is the seat's Possesses; running the same generic reconcile now
    // marks the vehicle. That is the whole claim: possession moved, so the marker moved.
    ReconcileLocalControl(world, presenting);
    CHECK(ResolveLocalControlledPawn(world, controlling) == vehicle);
}

TEST_CASE("An enter/exit round trip restores the character to a grounded state at the exit socket")
{
    VehicleScene fixture;
    Scene& world = *fixture.World;
    const Entity floor = fixture.AddFloor();
    fixture.AddUniformGravity();
    const auto [vehicle, seat] = fixture.AddVehicle(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f),
                                                    vec3(0.0f, -0.9f, 0.0f), true, false);
    const Entity character = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
    const Entity controlling = fixture.AddControllingSeat(character);

    fixture.Interact(vehicle, character);
    REQUIRE(world.Get<VehicleSeat>(seat).Occupant == character);

    // Interact again: the character occupies a seat, so this leaves the vehicle.
    fixture.Interact(vehicle, character);

    // The exit was handled and every entry step is undone.
    CHECK_FALSE(world.Has<InteractRequest>(vehicle));
    CHECK(world.Get<VehicleSeat>(seat).Occupant.IsNull());
    CHECK_FALSE(world.Has<Seated>(character));
    CHECK(world.GetParent(character).IsNull());
    CHECK(world.Get<CharacterController>(character).Enabled);
    CHECK(world.Get<Possesses>(controlling).Pawn == character);

    // The input context is restored to exactly what it was.
    const auto& active = world.Get<InputContextStack>(controlling).Active;
    REQUIRE(active.size() == 1);
    CHECK(active[0].Get() == fixture.CharacterContext.Get());

    // Placed at the exit socket world position (0, 0.1, 0) beside the hull.
    const vec3 exitWorld = world.Get<Transform>(character).Position;
    CHECK(exitWorld.y == doctest::Approx(0.1f).epsilon(0.05));

    // Re-enabled, it settles onto the floor: a valid grounded state, not stuck in geometry.
    ContextStorage storage;
    CharacterMovementSystem characters;
    for (u32 tick = 0; tick < 40; ++tick)
    {
        StepPhysics(world, FixedStep);
        characters.OnUpdate(world, FixedStep, storage.Make());
    }
    const auto& state = world.Get<CharacterState>(character);
    CHECK(state.Grounded);
    CHECK(state.GroundEntity == floor);
}

TEST_CASE("A blocked exit socket refuses the exit and reports rather than placing the character in "
          "geometry")
{
    VehicleScene fixture;
    Scene& world = *fixture.World;
    const auto [vehicle, seat] = fixture.AddVehicle(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f),
                                                    vec3(0.0f, -1.0f, 0.0f), true, false);
    const Entity character = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
    const Entity controlling = fixture.AddControllingSeat(character);

    fixture.Interact(vehicle, character);
    REQUIRE(world.Get<VehicleSeat>(seat).Occupant == character);

    // A solid wall exactly where the exit socket sits (world (0,0,0)), filling the character's
    // stand-up volume.
    const Entity wall = world.CreateEntity();
    world.Add<Transform>(wall, Transform{.Position = vec3(0.0f, 1.0f, 0.0f)});
    world.Add<RigidBody>(wall,
                         RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
    world.Add<Collider>(wall, Collider{.Shape = ColliderShape::Box, .Extents = vec3(1.5f)});
    StepPhysics(world, FixedStep);

    fixture.Interact(vehicle, character);

    // The exit failed and reported; the request is held one frame carrying the reason, and the
    // character is still seated exactly as it was — not teleported into the wall.
    REQUIRE(world.Has<InteractRequest>(vehicle));
    CHECK(world.Get<InteractRequest>(vehicle).Status == RequestStatus::Failed);
    CHECK_FALSE(world.Get<InteractRequest>(vehicle).Error.empty());
    CHECK(world.Get<VehicleSeat>(seat).Occupant == character);
    CHECK(world.GetParent(character) == vehicle);
    CHECK_FALSE(world.Get<CharacterController>(character).Enabled);
    CHECK(world.Get<Possesses>(controlling).Pawn == vehicle);

    // The held failure is retired on the next drain, so the stamper may retry.
    ContextStorage storage;
    VehicleSystem system;
    system.OnUpdate(world, FixedStep, storage.Make());
    CHECK_FALSE(world.Has<InteractRequest>(vehicle));
}

TEST_CASE("Exiting a moving vehicle transfers its velocity with no positional jump")
{
    VehicleScene fixture;
    Scene& world = *fixture.World;
    // No floor and no gravity: the character is in free-fall on exit, so the seeded velocity is
    // carried forward undisturbed and is measurable.
    const auto [vehicle, seat] = fixture.AddVehicle(vec3(0.0f, 5.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f),
                                                    vec3(2.0f, 0.0f, 0.0f), true, true);
    const Entity character = fixture.AddCharacter(vec3(5.0f, 5.0f, 0.0f));
    // Ballistic airborne (no air control) so the seeded velocity is carried undamped and measurable
    // in one tick rather than being blended toward a zero move command.
    world.Get<CharacterController>(character).AirControl = 0.0f;
    fixture.AddControllingSeat(character);

    // Bring the vehicle body into the world, then give it a horizontal velocity.
    StepPhysics(world, FixedStep);
    PhysicsWorld& physics = *world.GetPhysicsWorld();
    constexpr vec3 VehicleVelocity(3.0f, 0.0f, 0.0f);
    physics.SetLinearVelocity(vehicle, VehicleVelocity);

    fixture.Interact(vehicle, character);
    REQUIRE(world.Get<VehicleSeat>(seat).Occupant == character);

    fixture.Interact(vehicle, character);
    CHECK(world.Get<VehicleSeat>(seat).Occupant.IsNull());

    // The exit placed the character at the exit socket world pose (2, 5, 0), the hull at (0,5,0)
    // plus the socket's local (2,0,0).
    const vec3 exitPosition = world.Get<Transform>(character).Position;
    CHECK(exitPosition.x == doctest::Approx(2.0f).epsilon(0.001));
    CHECK(exitPosition.y == doctest::Approx(5.0f).epsilon(0.001));

    // One character tick: the re-created capsule keeps the seeded velocity (free-fall, gravity zero),
    // so the resolved planar speed matches the vehicle's, and the character has not jumped.
    ContextStorage storage;
    CharacterMovementSystem characters;
    characters.OnUpdate(world, FixedStep, storage.Make());

    const auto& state = world.Get<CharacterState>(character);
    CHECK(state.PlanarSpeed == doctest::Approx(glm::length(VehicleVelocity)).epsilon(0.1));
    const vec3 moved = world.Get<Transform>(character).Position - exitPosition;
    CHECK(glm::length(moved) < 0.1f);
}

TEST_CASE(
    "An offset pose resolver exits in the solver's frame and reports where the character landed")
{
    VehicleScene fixture;
    Scene& world = *fixture.World;
    fixture.InstallOffsetResolver();
    const auto [vehicle, seat] = fixture.AddVehicle(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f),
                                                    vec3(2.0f, -1.0f, 0.0f), true, false);
    const Entity character = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
    fixture.AddControllingSeat(character);

    fixture.Interact(vehicle, character);
    REQUIRE(world.Get<VehicleSeat>(seat).Occupant == character);
    fixture.Interact(vehicle, character);
    REQUIRE(world.Get<VehicleSeat>(seat).Occupant.IsNull());

    // The hull at (0, 1, 0) plus the exit socket's local (2, -1, 0), projected onto the solver's
    // origin — one resolved pose, reported back at full precision.
    CHECK(fixture.PlacedEntity == character);
    CHECK(fixture.PlacedPosition.x == doctest::Approx(SolverOrigin.x + 2.0).epsilon(1.0e-9));
    CHECK(fixture.PlacedPosition.y == doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(fixture.PlacedPosition.z == doctest::Approx(SolverOrigin.z).epsilon(1.0e-9));

    // The consumer's hook owns the write-back, so the engine left the f32 Transform on the seat
    // socket's place that entry gave it.
    CHECK(world.Get<Transform>(character).Position.y == doctest::Approx(0.5f).epsilon(0.001));

    // The capsule was re-created at the resolved pose rather than skipped.
    CHECK(world.GetPhysicsWorld()->HasCharacter(character));
}

TEST_CASE("The exit is validated in the frame the resolver names, not the Transform chain's")
{
    // Installed, the capsule is swept where the character would actually appear, so an obstruction
    // standing in the solver's frame is found and the exit is refused.
    {
        VehicleScene fixture;
        Scene& world = *fixture.World;
        fixture.InstallOffsetResolver();
        const auto [vehicle, seat] = fixture.AddVehicle(
            vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f), vec3(0.0f, -1.0f, 0.0f), true, false);
        const Entity character = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
        fixture.AddControllingSeat(character);
        fixture.AddSolverFrameObstruction(SolverOrigin, vec3(1.5f));
        StepPhysics(world, FixedStep);

        fixture.Interact(vehicle, character);
        REQUIRE(world.Get<VehicleSeat>(seat).Occupant == character);
        fixture.Interact(vehicle, character);

        REQUIRE(world.Has<InteractRequest>(vehicle));
        CHECK(world.Get<InteractRequest>(vehicle).Status == RequestStatus::Failed);
        CHECK(world.Get<VehicleSeat>(seat).Occupant == character);
    }

    // The same scene with nothing installed sweeps the Transform chain's place, where the solver
    // holds nothing, and lets the exit through — the gap the seam closes.
    {
        VehicleScene fixture;
        Scene& world = *fixture.World;
        const auto [vehicle, seat] = fixture.AddVehicle(
            vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f), vec3(0.0f, -1.0f, 0.0f), true, false);
        const Entity character = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
        fixture.AddControllingSeat(character);
        fixture.AddSolverFrameObstruction(SolverOrigin, vec3(1.5f));
        StepPhysics(world, FixedStep);

        fixture.Interact(vehicle, character);
        REQUIRE(world.Get<VehicleSeat>(seat).Occupant == character);
        fixture.Interact(vehicle, character);

        CHECK_FALSE(world.Has<InteractRequest>(vehicle));
        CHECK(world.Get<VehicleSeat>(seat).Occupant.IsNull());
    }
}

TEST_CASE("Boarding a vehicle whose seats are all taken fails and reports")
{
    VehicleScene fixture;
    Scene& world = *fixture.World;
    const auto [vehicle, seat] = fixture.AddVehicle(vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.5f, 0.0f),
                                                    vec3(0.0f, -0.9f, 0.0f), true, false);
    const Entity first = fixture.AddCharacter(vec3(5.0f, 0.1f, 0.0f));
    fixture.AddControllingSeat(first);
    fixture.Interact(vehicle, first);
    REQUIRE(world.Get<VehicleSeat>(seat).Occupant == first);

    // A second character finds the only seat taken.
    const Entity second = fixture.AddCharacter(vec3(-5.0f, 0.1f, 0.0f));
    fixture.Interact(vehicle, second);

    REQUIRE(world.Has<InteractRequest>(vehicle));
    CHECK(world.Get<InteractRequest>(vehicle).Status == RequestStatus::Failed);
    CHECK(world.Get<VehicleSeat>(seat).Occupant == first);
    CHECK_FALSE(world.Has<Seated>(second));
}
