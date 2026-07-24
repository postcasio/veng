// Client-side prediction and reconciliation for a character controller, on the deterministic step.
// A predicted character's per-tick physics state is saved, restored on a correction, and the
// recorded input replayed forward — the kinematic world re-driven from each replayed tick so the
// character re-collides with the configuration it was actually in. Pure CPU: a headless Scene and a
// solver, the builtin CharacterMovementSystem + PhysicsSystem driven by hand, no Context, no GPU.

#include <doctest/doctest.h>

#include <Veng/Input.h>
#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/CharacterMovementSystem.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/Gravity.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/RemoteCharacterBodySystem.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A SystemContext over headless storage the systems never dereference. Tick and IsReplay are set
    // per call; the default Role (Server) makes a Server-tier entity authoritative, which is what a
    // single-peer prediction test wants.
    struct ContextStorage
    {
        Input HeadlessInput{nullptr};
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make(const u64 tick, const bool replay)
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Tick = tick,
                .IsReplay = replay,
            };
        }
    };

    CharacterController DefaultController()
    {
        return CharacterController{
            .Radius = 0.3f,
            .Height = 1.8f,
            .StepHeight = 0.3f,
            .MaxSlopeAngle = glm::radians(45.0f),
            .WalkSpeed = 4.0f,
            .RunSpeed = 7.0f,
            .JumpImpulse = 5.0f,
            .AirControl = 0.2f,
        };
    }

    // One tick's recorded state: the input that produced it plus the character's whole rollback
    // state — the capsule blob (pose, velocity, ground contact), its reconciled Transform, and its
    // CharacterState (the Up and AirTime the next tick reads back).
    struct Recorded
    {
        u64 Tick = 0;
        Intent Command;
        vector<u8> Capsule;
        Transform CharTransform;
        CharacterState State;
    };

    // A predicted character on a flat static floor under a uniform down field — the world tests 1
    // and 2 roll back inside. The character carries the Predicted marker, so the mover re-seats its
    // capsule from the reconciled Transform each tick.
    struct FlatWorld
    {
        TypeRegistry Types;
        Unique<Scene> World;
        Entity Pawn;
        CharacterController Controller = DefaultController();

        FlatWorld()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));

            const Entity field = World->CreateEntity();
            World->Add<Transform>(field, Transform{});
            World->Add<GravitySource>(field,
                                      GravitySource{.Kind = GravityKind::Uniform,
                                                    .Direction = vec3(0.0f, -1.0f, 0.0f),
                                                    .Magnitude = 9.81f,
                                                    .Bounds = Region{.HalfExtents = vec3(100.0f)}});

            const Entity floor = World->CreateEntity();
            World->Add<Transform>(floor, Transform{.Position = vec3(0.0f, -0.5f, 0.0f)});
            World->Add<RigidBody>(
                floor, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
            World->Add<Collider>(
                floor, Collider{.Shape = ColliderShape::Box, .Extents = vec3(20.0f, 0.5f, 20.0f)});

            Pawn = World->CreateEntity();
            World->Add<Transform>(Pawn, Transform{.Position = vec3(0.0f, 0.1f, 0.0f)});
            World->Add<CharacterController>(Pawn, Controller);
            World->Add<Intent>(Pawn, Intent{});
            World->Add<Predicted>(Pawn);
        }

        PhysicsWorld& Physics() { return *World->GetPhysicsWorld(); }
    };

    // Advances one client tick: stamps the input, runs the character mover, then steps the physics
    // world (the real Sim order for a scene naming the mover before PhysicsSystem is the reverse, but
    // this fixed order is what forward and replay share, which is all determinism needs).
    void Tick(Scene& scene, const Entity pawn, PhysicsSystem& physics,
              CharacterMovementSystem& mover, ContextStorage& storage, const Intent& command,
              const u64 tick, const bool replay)
    {
        scene.Get<Intent>(pawn) = command;
        physics.OnUpdate(scene, FixedStep, storage.Make(tick, replay));
        mover.OnUpdate(scene, FixedStep, storage.Make(tick, replay));
    }

    // Captures the character's whole rollback state after a tick.
    Recorded Capture(Scene& scene, const Entity pawn, const u64 tick, const Intent& command)
    {
        return Recorded{
            .Tick = tick,
            .Command = command,
            .Capsule = scene.GetPhysicsWorld()->SaveCharacterState(pawn),
            .CharTransform = scene.Get<Transform>(pawn),
            .State = scene.Get<CharacterState>(pawn),
        };
    }

    // Restores the character's whole rollback state — the authoritative pose and CharacterState onto
    // the components (the mover re-seats the capsule pose from the Transform), and the capsule blob
    // for the velocity and ground contact the components do not carry. Ensures the capsule and the
    // CharacterState channel exist first, so a fresh world seated straight at a state (no prior tick
    // to create either) restores exactly as a warmed one does.
    void Restore(Scene& scene, const Entity pawn, const Recorded& state)
    {
        PhysicsWorld& world = *scene.GetPhysicsWorld();
        if (!world.HasCharacter(pawn))
        {
            world.CreateCharacter(pawn, scene.Get<CharacterController>(pawn),
                                  PhysicsPose{.Position = dvec3(state.CharTransform.Position),
                                              .Rotation = state.CharTransform.Rotation});
        }
        if (!scene.Has<CharacterState>(pawn))
        {
            scene.Add<CharacterState>(pawn);
        }
        scene.Get<Transform>(pawn) = state.CharTransform;
        scene.Get<CharacterState>(pawn) = state.State;
        REQUIRE(world.RestoreCharacterState(pawn, state.Capsule).has_value());
    }

    // A varied but deterministic input tape: it strafes, runs, and jumps so the trajectory is not a
    // straight line and a mispredicted branch genuinely diverges.
    Intent InputAt(const u64 tick)
    {
        Intent command;
        command.Move = vec3(std::sin(static_cast<f32>(tick) * 0.11f) * 0.5f, 0.0f, 1.0f);
        if (tick % 5 == 0)
        {
            command.Actions |= static_cast<u32>(CharacterAction::Run);
        }
        if (tick % 23 == 0)
        {
            command.Actions |= static_cast<u32>(CharacterAction::Jump);
        }
        return command;
    }
}

TEST_CASE("The same input history replayed from the same restored state is bit-identical")
{
    // Settle a character, capture its state at C, then replay the same inputs from that state twice.
    // The step is deterministic, so the two replays must land bit-for-bit identically — the property
    // reconciliation converges on rather than oscillates around.
    FlatWorld sim;
    PhysicsSystem physics;
    CharacterMovementSystem mover;
    ContextStorage storage;

    for (u64 tick = 1; tick <= 40; ++tick)
    {
        Tick(*sim.World, sim.Pawn, physics, mover, storage, InputAt(tick), tick, false);
    }

    constexpr u64 Restart = 40;
    const Recorded anchor = Capture(*sim.World, sim.Pawn, Restart, InputAt(Restart));

    const auto replayFrom = [&](const Recorded& state) -> vec3
    {
        Restore(*sim.World, sim.Pawn, state);
        for (u64 tick = Restart + 1; tick <= Restart + 30; ++tick)
        {
            Tick(*sim.World, sim.Pawn, physics, mover, storage, InputAt(tick), tick, true);
        }
        return sim.World->Get<Transform>(sim.Pawn).Position;
    };

    const vec3 first = replayFrom(anchor);
    const vec3 second = replayFrom(anchor);

    CHECK(first.x == second.x);
    CHECK(first.y == second.y);
    CHECK(first.z == second.z);
}

TEST_CASE("A correction with a perturbed server state re-simulates to a fresh run from it exactly")
{
    // Sixty ticks forward, then a correction at tick 30 to a deliberately perturbed pose: the replay
    // restores the perturbed state and re-runs 31..60. A second world seated at the same perturbed
    // state and run the same inputs is the fresh reference; the two must match bit-for-bit, which is
    // what makes a rollback converge to the server rather than drift.
    constexpr u64 Ticks = 60;
    constexpr u64 Correction = 30;

    FlatWorld sim;
    PhysicsSystem physics;
    CharacterMovementSystem mover;
    ContextStorage storage;

    vector<Recorded> history;
    for (u64 tick = 1; tick <= Ticks; ++tick)
    {
        const Intent command = InputAt(tick);
        Tick(*sim.World, sim.Pawn, physics, mover, storage, command, tick, false);
        history.push_back(Capture(*sim.World, sim.Pawn, tick, command));
    }

    // The perturbed "server state" at the correction tick: the recorded state, shoved sideways and
    // given a different velocity, so restoring it genuinely changes the branch.
    Recorded perturbed = history[Correction - 1];
    perturbed.CharTransform.Position += vec3(0.75f, 0.0f, -0.4f);
    {
        FlatWorld seed;
        // Seat a scratch capsule at the perturbed pose to mint a matching capsule blob (the pose and
        // a small forward velocity), so the two branches start from the identical restored state.
        seed.World->Get<Transform>(seed.Pawn) = perturbed.CharTransform;
        seed.Physics().CreateCharacter(
            seed.Pawn, seed.Controller,
            PhysicsPose{.Position = dvec3(perturbed.CharTransform.Position),
                        .Rotation = perturbed.CharTransform.Rotation});
        seed.Physics().SetCharacterVelocity(seed.Pawn, vec3(0.5f, 0.0f, 0.5f));
        perturbed.Capsule = seed.Physics().SaveCharacterState(seed.Pawn);
    }

    // The fresh reference: a clean world seated at the perturbed state, run 31..60 continuously.
    vec3 fresh;
    {
        FlatWorld reference;
        PhysicsSystem refPhysics;
        CharacterMovementSystem refMover;
        ContextStorage refStorage;
        Restore(*reference.World, reference.Pawn, perturbed);
        for (u64 tick = Correction + 1; tick <= Ticks; ++tick)
        {
            Tick(*reference.World, reference.Pawn, refPhysics, refMover, refStorage, InputAt(tick),
                 tick, false);
        }
        fresh = reference.World->Get<Transform>(reference.Pawn).Position;
    }

    // The rollback: restore the perturbed state in the original world, replay 31..60.
    Restore(*sim.World, sim.Pawn, perturbed);
    for (u64 tick = Correction + 1; tick <= Ticks; ++tick)
    {
        Tick(*sim.World, sim.Pawn, physics, mover, storage, InputAt(tick), tick, true);
    }
    const vec3 replayed = sim.World->Get<Transform>(sim.Pawn).Position;

    CHECK(replayed.x == fresh.x);
    CHECK(replayed.y == fresh.y);
    CHECK(replayed.z == fresh.z);
}

TEST_CASE("A character on a kinematic platform re-simulates against the platform's replayed pose")
{
    // A character rides a translating kinematic platform. Corrected twenty ticks in the past, the
    // replay re-drives the platform from each replayed tick, so the character re-collides with the
    // surface where it actually was. The failure mode is precise: a replay that leaves the platform
    // at its *current* pose reconciles the character to a surface it never stood on — that branch is
    // computed too and must land far away.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);
    scene->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));
    PhysicsWorld& world = *scene->GetPhysicsWorld();

    const Entity field = scene->CreateEntity();
    scene->Add<Transform>(field, Transform{});
    scene->Add<GravitySource>(field, GravitySource{.Kind = GravityKind::Uniform,
                                                   .Direction = vec3(0.0f, -1.0f, 0.0f),
                                                   .Magnitude = 9.81f,
                                                   .Bounds = Region{.HalfExtents = vec3(100.0f)}});

    // The platform is deterministic-from-tick: its pose is a pure function of the tick number, so a
    // replay reproduces it exactly by re-driving it, storing nothing.
    const Entity platform = scene->CreateEntity();
    const auto platformPose = [](const u64 tick)
    { return vec3(static_cast<f32>(tick) * FixedStep * 1.5f, 0.0f, 0.0f); };
    scene->Add<Transform>(platform, Transform{.Position = platformPose(0)});
    scene->Add<RigidBody>(
        platform, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
    scene->Add<Collider>(platform,
                         Collider{.Shape = ColliderShape::Box, .Extents = vec3(3.0f, 0.25f, 3.0f)});

    const CharacterController controller = DefaultController();
    const Entity pawn = scene->CreateEntity();
    scene->Add<Transform>(pawn, Transform{.Position = vec3(0.0f, 0.30f, 0.0f)});
    scene->Add<CharacterController>(pawn, controller);
    scene->Add<Intent>(pawn, Intent{});
    scene->Add<Predicted>(pawn);

    PhysicsSystem physics;
    CharacterMovementSystem mover;
    ContextStorage storage;

    const auto drivePlatform = [&](const u64 tick)
    { scene->Get<Transform>(platform).Position = platformPose(tick); };

    // Settle the character onto the stationary platform (its pose held at tick 0).
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        drivePlatform(0);
        physics.OnUpdate(*scene, FixedStep, storage.Make(tick, false));
        mover.OnUpdate(*scene, FixedStep, storage.Make(tick, false));
    }

    constexpr u64 Base = 40;
    constexpr u64 Ticks = 60;
    constexpr u64 Correction = 40; // twenty ticks before the end

    vector<Recorded> history;
    for (u64 tick = Base + 1; tick <= Base + Ticks; ++tick)
    {
        drivePlatform(tick);
        physics.OnUpdate(*scene, FixedStep, storage.Make(tick, false));
        mover.OnUpdate(*scene, FixedStep, storage.Make(tick, false));
        history.push_back(Capture(*scene, pawn, tick, Intent{}));
    }
    const u64 lastTick = Base + Ticks;
    const vec3 forwardEnd = scene->Get<Transform>(pawn).Position;

    const u64 correctTick = Base + Correction;
    const Recorded& anchor = history[Correction - 1];

    // The correct replay: prime the platform to its corrected-tick pose, restore the character, then
    // replay to the end re-driving the platform each tick — this reproduces the forward branch.
    const auto replay = [&](const bool redrivePlatform) -> vec3
    {
        world.SetBodyPose(platform, PhysicsPose{.Position = dvec3(platformPose(correctTick))});
        scene->Get<Transform>(platform).Position = platformPose(correctTick);
        Restore(*scene, pawn, anchor);
        for (u64 tick = correctTick + 1; tick <= lastTick; ++tick)
        {
            // The bug the test guards: a replay that fails to re-drive kinematic bodies leaves the
            // platform frozen at its corrected-tick pose while the character walks off the end it
            // was actually carried past.
            if (redrivePlatform)
            {
                drivePlatform(tick);
            }
            physics.OnUpdate(*scene, FixedStep, storage.Make(tick, true));
            mover.OnUpdate(*scene, FixedStep, storage.Make(tick, true));
        }
        return scene->Get<Transform>(pawn).Position;
    };

    const vec3 correct = replay(true);
    const vec3 frozen = replay(false);

    // Re-driving reproduces the forward branch: the character ends where it actually was.
    CHECK(glm::length(correct - forwardEnd) < 0.05f);
    // Leaving the platform at its corrected-tick pose strands the character a whole correction
    // window behind — the surface moved ~0.5 m over the twenty replayed ticks and it did not follow.
    CHECK(glm::length(frozen - forwardEnd) > 0.3f);
}

TEST_CASE("A local character is blocked by a remote character's kinematic collision proxy")
{
    // The local character walks straight at a remote one. The remote character is interpolated, not
    // simulated, so it carries no capsule — but RemoteCharacterBodySystem gives it a kinematic body
    // matching its capsule, and the local character's own capsule is blocked by it. Neither pushes
    // the other: the remote body is kinematic and holds its interpolated pose.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);
    scene->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));

    const Entity field = scene->CreateEntity();
    scene->Add<Transform>(field, Transform{});
    scene->Add<GravitySource>(field, GravitySource{.Kind = GravityKind::Uniform,
                                                   .Direction = vec3(0.0f, -1.0f, 0.0f),
                                                   .Magnitude = 9.81f,
                                                   .Bounds = Region{.HalfExtents = vec3(100.0f)}});

    const Entity floor = scene->CreateEntity();
    scene->Add<Transform>(floor, Transform{.Position = vec3(0.0f, -0.5f, 0.0f)});
    scene->Add<RigidBody>(floor,
                          RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
    scene->Add<Collider>(
        floor, Collider{.Shape = ColliderShape::Box, .Extents = vec3(20.0f, 0.5f, 20.0f)});

    const CharacterController controller = DefaultController();

    // The local character, simulated here (default Server authority), walking +Z.
    const Entity local = scene->CreateEntity();
    scene->Add<Transform>(local, Transform{.Position = vec3(0.0f, 0.1f, 0.0f)});
    scene->Add<CharacterController>(local, controller);
    scene->Add<Intent>(local, Intent{.Move = vec3(0.0f, 0.0f, 1.0f)});

    // The remote character, mirrored (Tier::Remote) so this peer does not simulate it, standing a
    // little way ahead. Its Transform is held fixed here (a stand-in for the interpolated pose).
    const Entity remote = scene->CreateEntity();
    constexpr f32 RemoteZ = 2.0f;
    scene->Add<Transform>(remote, Transform{.Position = vec3(0.0f, 0.1f, RemoteZ)});
    scene->Add<CharacterController>(remote, controller);
    scene->Add<Authority>(remote, Authority{.Tier = Tier::Remote});

    RemoteCharacterBodySystem proxies;
    PhysicsSystem physics;
    CharacterMovementSystem mover;
    ContextStorage storage;

    for (u64 tick = 1; tick <= 200; ++tick)
    {
        proxies.OnUpdate(*scene, FixedStep, storage.Make(tick, false));
        physics.OnUpdate(*scene, FixedStep, storage.Make(tick, false));
        mover.OnUpdate(*scene, FixedStep, storage.Make(tick, false));
    }

    // The proxy grew and the remote character is never simulated (no capsule).
    CHECK(scene->Has<RigidBody>(remote));
    CHECK(scene->GetPhysicsWorld()->HasBody(remote));
    CHECK_FALSE(scene->GetPhysicsWorld()->HasCharacter(remote));

    // The local character stopped short: blocked roughly two radii before the remote body, never
    // reaching its position and never pushing it through.
    const f32 localZ = scene->Get<Transform>(local).Position.z;
    CHECK(localZ < RemoteZ - controller.Radius);
    CHECK(localZ > RemoteZ - 3.0f * controller.Radius);
    CHECK(scene->Get<Transform>(remote).Position.z == doctest::Approx(RemoteZ));
}
