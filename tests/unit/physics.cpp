// Physics: the rigid-body module's world/body model, the layer table, the fixed step's
// component reconciliation, the PhysicsPose/SyncTransform seam, the replay gate, the
// StateRecorder round-trip, and the world PhysicsSystem creates from authored PhysicsSettings. Pure CPU — a headless Scene with no Context and no GPU.

#include <doctest/doctest.h>

#include <algorithm>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/Queries.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinSystems.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include "support/TestServices.h"

using namespace Veng;

namespace
{
    // The Sim tick the module is arranged around; every step below uses it so a step count is a
    // simulated duration.
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A scene with a physics world, the builtin types registered, and nothing else.
    struct PhysicsFixture
    {
        TypeRegistry Types;
        Unique<Scene> World;

        PhysicsFixture()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));
        }

        PhysicsWorld& Physics() const { return *World->GetPhysicsWorld(); }

        // A static ground box centred at the origin, its top face at y = 0.
        Entity SpawnGround() const
        {
            const Entity ground = World->CreateEntity();
            World->Add<Transform>(ground, Transform{.Position = vec3(0.0f, -1.0f, 0.0f)});
            World->Add<RigidBody>(
                ground, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
            World->Add<Collider>(
                ground, Collider{.Shape = ColliderShape::Box, .Extents = vec3(20.0f, 1.0f, 20.0f)});
            return ground;
        }

        // A 1 m dynamic cube dropped from height, with unit half extents of 0.5.
        Entity SpawnBox(const vec3 position, const bool syncTransform = true) const
        {
            const Entity box = World->CreateEntity();
            World->Add<Transform>(box, Transform{.Position = position});
            World->Add<RigidBody>(box, RigidBody{.Motion = MotionType::Dynamic,
                                                 .Mass = 10.0f,
                                                 .SyncTransform = syncTransform});
            World->Add<Collider>(
                box,
                Collider{.Shape = ColliderShape::Box, .Extents = vec3(0.5f), .Restitution = 0.0f});
            if (!syncTransform)
            {
                World->Add<PhysicsPose>(box, PhysicsPose{.Position = dvec3(position)});
            }
            return box;
        }

        void Step(const u32 count) const
        {
            for (u32 i = 0; i < count; ++i)
            {
                StepPhysics(*World, FixedStep);
            }
        }

        // Steps as the world drive does, advancing the scene's change tick before each step so an
        // edit is stamped with the tick it was made in.
        void Tick(const u32 count) const
        {
            for (u32 i = 0; i < count; ++i)
            {
                World->SetChangeTick(++SimTick);
                StepPhysics(*World, FixedStep);
            }
        }

        // The change tick the next Tick stamps from.
        mutable u64 SimTick = Scene::MinChangeTick;
    };
}

TEST_CASE("the default collision matrix is symmetric and leaves Query colliding with nothing")
{
    const CollisionMatrix matrix = DefaultCollisionMatrix();

    CHECK(IsSymmetric(matrix));
    CHECK(LayersCollide(matrix, PhysicsLayer::Static, PhysicsLayer::Moving));
    CHECK(LayersCollide(matrix, PhysicsLayer::Moving, PhysicsLayer::Character));
    CHECK(LayersCollide(matrix, PhysicsLayer::Trigger, PhysicsLayer::Character));

    // Static geometry does not collide with static geometry, and a Query body is only ever found
    // by a query.
    CHECK_FALSE(LayersCollide(matrix, PhysicsLayer::Static, PhysicsLayer::Static));
    CHECK_FALSE(LayersCollide(matrix, PhysicsLayer::Static, PhysicsLayer::Trigger));
    for (u32 other = 0; other < PhysicsLayerCount; ++other)
    {
        CHECK_FALSE(LayersCollide(matrix, PhysicsLayer::Query, static_cast<PhysicsLayer>(other)));
    }

    // Debris meets the world, itself and triggers, and never a mover or a character, which reach
    // it only through their own queries.
    CHECK(LayersCollide(matrix, PhysicsLayer::Debris, PhysicsLayer::Static));
    CHECK(LayersCollide(matrix, PhysicsLayer::Debris, PhysicsLayer::Debris));
    CHECK(LayersCollide(matrix, PhysicsLayer::Debris, PhysicsLayer::Trigger));
    CHECK_FALSE(LayersCollide(matrix, PhysicsLayer::Debris, PhysicsLayer::Moving));
    CHECK_FALSE(LayersCollide(matrix, PhysicsLayer::Debris, PhysicsLayer::Character));
}

TEST_CASE("a dynamic box dropped onto a static plane comes to rest on it")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    const Entity box = fixture.SpawnBox(vec3(0.0f, 4.0f, 0.0f));

    // Two seconds is far more than the ~0.9 s free fall from 4 m, so the box has landed and any
    // residual bounce has damped out.
    fixture.Step(120);

    const Transform& transform = fixture.World->Get<Transform>(box);
    // The cube's half extent is 0.5 and the ground's top face is at y = 0.
    CHECK(transform.Position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(transform.Position.x == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(transform.Position.z == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("Collider::Rotation turns a primitive onto the axis it names")
{
    const PhysicsFixture fixture;

    // A capsule of radius 0.5 and cylinder half-height 3 stands about local Y; a 90-degree turn
    // about X lays it along Z instead. Kinematic, so the solver holds it where it is placed.
    const Entity body = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(body, Transform{});
    fixture.World->Add<RigidBody>(
        body, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
    fixture.World->Add<Collider>(body,
                                 Collider{.Shape = ColliderShape::Capsule,
                                          .Extents = vec3(0.5f, 3.0f, 0.0f),
                                          .Rotation = quat(0.7071068f, 0.7071068f, 0.0f, 0.0f)});
    fixture.Step(1);

    const Collider probe{.Shape = ColliderShape::Sphere, .Extents = vec3(0.1f)};
    vector<Entity> hits;

    // 2.5 m down the Z axis is inside the laid-down capsule; the same distance up Y — the axis the
    // capsule stood on before the turn — is now well outside its 0.5 m radius.
    Overlap(&fixture.Physics(), probe, PhysicsPose{.Position = dvec3(0.0, 0.0, 2.5)}, {}, hits);
    CHECK_FALSE(hits.empty());

    hits.clear();
    Overlap(&fixture.Physics(), probe, PhysicsPose{.Position = dvec3(0.0, 2.5, 0.0)}, {}, hits);
    CHECK(hits.empty());
}

TEST_CASE("the reconcile pass creates a body per physics entity and destroys an orphan's")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    const Entity box = fixture.SpawnBox(vec3(0.0f, 4.0f, 0.0f));

    fixture.Step(1);
    CHECK(fixture.Physics().GetBodyCount() == 2);
    CHECK(fixture.Physics().HasBody(box));

    // The component is the authority and the body is its shadow.
    (void)fixture.World->Remove<Collider>(box);
    fixture.Step(1);
    CHECK(fixture.Physics().GetBodyCount() == 1);
    CHECK_FALSE(fixture.Physics().HasBody(box));
}

TEST_CASE("the step adds a PhysicsPose to every physics entity and keeps it authoritative")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    const Entity box = fixture.SpawnBox(vec3(0.0f, 4.0f, 0.0f));

    CHECK_FALSE(fixture.World->Has<PhysicsPose>(box));
    fixture.Step(10);
    REQUIRE(fixture.World->Has<PhysicsPose>(box));

    const PhysicsPose& pose = fixture.World->Get<PhysicsPose>(box);
    const Transform& transform = fixture.World->Get<Transform>(box);
    CHECK(pose.Position.y < 4.0);
    // With SyncTransform on, the two channels agree.
    CHECK(static_cast<f32>(pose.Position.y) == doctest::Approx(transform.Position.y));
}

TEST_CASE("clearing SyncTransform binds the solver to PhysicsPose and leaves Transform alone")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    const Entity box = fixture.SpawnBox(vec3(0.0f, 4.0f, 0.0f), /*syncTransform=*/false);

    fixture.Step(10);

    // The consumer's own per-frame pass owns the Transform; the step never touched it.
    const Transform& transform = fixture.World->Get<Transform>(box);
    CHECK(transform.Position.y == doctest::Approx(4.0f));

    // PhysicsPose is the whole channel, and it advanced.
    const PhysicsPose& pose = fixture.World->Get<PhysicsPose>(box);
    CHECK(pose.Position.y < 4.0);

    // Writing PhysicsPose is how such a consumer places the body: the next step reads it back.
    fixture.World->Get<PhysicsPose>(box).Position = dvec3(0.0, 9.0, 0.0);
    fixture.World->Get<RigidBody>(box).Motion = MotionType::Kinematic;
    fixture.Step(1);
    CHECK(fixture.World->Get<PhysicsPose>(box).Position.y == doctest::Approx(9.0).epsilon(0.01));
}

TEST_CASE("a kinematic body follows its Transform and never falls")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    const Entity mover = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(mover, Transform{.Position = vec3(0.0f, 5.0f, 0.0f)});
    fixture.World->Add<RigidBody>(mover, RigidBody{.Motion = MotionType::Kinematic});
    fixture.World->Add<Collider>(mover, Collider{.Extents = vec3(0.5f)});

    fixture.Step(30);
    CHECK(fixture.World->Get<Transform>(mover).Position.y == doctest::Approx(5.0f).epsilon(0.01));

    // The owner drives it: the target Transform is the input and the body follows.
    fixture.World->Get<Transform>(mover).Position = vec3(0.0f, 5.0f, 2.0f);
    fixture.Step(2);
    CHECK(fixture.World->Get<Transform>(mover).Position.z == doctest::Approx(2.0f).epsilon(0.05));
}

TEST_CASE("a resting body costs the step nothing: a field of sleepers pays only for what moves")
{
    const PhysicsFixture fixture;

    // A field of 2,000 dynamic bodies created asleep, spaced so none touches another, beside one
    // awake body falling clear of them and a trigger volume with no Sensor component.
    constexpr u32 Sleepers = 2000;
    vector<Entity> field;
    field.reserve(Sleepers);
    for (u32 i = 0; i < Sleepers; ++i)
    {
        const Entity body = fixture.World->CreateEntity();
        fixture.World->Add<Transform>(
            body, Transform{.Position = vec3(static_cast<f32>(i % 50) * 3.0f, 0.0f,
                                             static_cast<f32>(i / 50) * 3.0f)});
        fixture.World->Add<RigidBody>(body, RigidBody{.StartAsleep = true});
        fixture.World->Add<Collider>(body, Collider{.Extents = vec3(0.5f)});
        field.push_back(body);
    }
    const Entity awake = fixture.SpawnBox(vec3(-50.0f, 10.0f, -50.0f));
    const Entity trigger = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(trigger, Transform{.Position = vec3(-80.0f, 0.0f, 0.0f)});
    fixture.World->Add<RigidBody>(
        trigger, RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Trigger});
    fixture.World->Add<Collider>(trigger, Collider{.Extents = vec3(2.0f)});

    fixture.Tick(1);
    REQUIRE(fixture.Physics().GetBodyCount() == Sleepers + 2);
    CHECK_FALSE(fixture.Physics().IsBodyActive(field.front()));
    // The whole world arrived at once, so the first step builds its broad phase whole.
    CHECK(fixture.Physics().GetStepStats().OptimizedBroadPhase);
    // A body created asleep is never read back, so its PhysicsPose is seeded at creation.
    CHECK(fixture.World->Get<PhysicsPose>(field.back()).Position ==
          dvec3(fixture.World->Get<Transform>(field.back()).Position));

    // Once the bodies exist, a tick reconciles nothing, pushes nothing, and reads back only the
    // one body that moves.
    u32 reconciled = 0;
    u32 pushed = 0;
    u32 readMin = ~0u;
    u32 readMax = 0;
    for (u32 i = 0; i < 30; ++i)
    {
        fixture.Tick(1);
        const PhysicsStepStats& stats = fixture.Physics().GetStepStats();
        reconciled += stats.Reconciled;
        pushed += stats.Pushed;
        readMin = std::min(readMin, stats.ReadBack);
        readMax = std::max(readMax, stats.ReadBack);
    }
    CHECK(reconciled == 0);
    CHECK(pushed == 0);
    CHECK(readMin == 1);
    CHECK(readMax == 1);
    CHECK(fixture.World->Get<PhysicsPose>(awake).Position.y < 10.0);

    // A body joining the field is absorbed by the incremental tree update, not a whole rebuild.
    const Entity joining = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(joining, Transform{.Position = vec3(-20.0f, 0.0f, -20.0f)});
    fixture.World->Add<RigidBody>(joining, RigidBody{.StartAsleep = true});
    fixture.World->Add<Collider>(joining, Collider{.Extents = vec3(0.5f)});
    fixture.Tick(1);
    CHECK(fixture.Physics().HasBody(joining));
    CHECK(fixture.Physics().GetStepStats().Reconciled == 1);
    CHECK_FALSE(fixture.Physics().GetStepStats().OptimizedBroadPhase);
    fixture.Tick(1);
    (void)fixture.World->Remove<RigidBody>(joining);
    fixture.Tick(1);

    // An edit reconciles the one entity it touched, and a removal destroys the one body it named,
    // neither walking the field.
    fixture.World->Get<RigidBody>(field[7]).Mass = 20.0f;
    fixture.Tick(1);
    CHECK(fixture.Physics().GetStepStats().Reconciled == 1);
    CHECK_FALSE(fixture.Physics().GetStepStats().FullReconcile);
    fixture.Tick(1);

    (void)fixture.World->Remove<Collider>(field[9]);
    fixture.Tick(1);
    CHECK(fixture.Physics().GetStepStats().Reconciled == 1);
    CHECK_FALSE(fixture.Physics().GetStepStats().FullReconcile);
    CHECK_FALSE(fixture.Physics().HasBody(field[9]));
    CHECK(fixture.Physics().GetBodyCount() == Sleepers + 1);

    fixture.World->DestroyEntity(field[11]);
    fixture.Tick(1);
    CHECK_FALSE(fixture.Physics().GetStepStats().FullReconcile);
    CHECK(fixture.Physics().GetBodyCount() == Sleepers);
}

TEST_CASE("an unmoved kinematic body stays where it was last pushed, then sleeps")
{
    const PhysicsFixture fixture;
    const Entity mover = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(mover, Transform{.Position = vec3(0.0f, 5.0f, 0.0f)});
    fixture.World->Add<RigidBody>(mover, RigidBody{.Motion = MotionType::Kinematic});
    fixture.World->Add<Collider>(mover, Collider{.Extents = vec3(0.5f)});
    fixture.Tick(1);

    // The owner drives it for a few ticks, then leaves it.
    for (u32 i = 1; i <= 5; ++i)
    {
        fixture.World->Get<Transform>(mover).Position =
            vec3(0.0f, 5.0f, 0.1f * static_cast<f32>(i));
        fixture.Tick(1);
        CHECK(fixture.Physics().GetStepStats().Pushed == 1);
    }

    // The first still tick pushes once more to the same pose, which stops the body; the velocity
    // that reached the last target would otherwise carry it on.
    fixture.Tick(1);
    CHECK(fixture.Physics().GetStepStats().Pushed == 1);
    const PhysicsPose rest = *fixture.Physics().GetBodyPose(mover);

    u32 pushed = 0;
    f64 drift = 0.0;
    for (u32 i = 0; i < 60; ++i)
    {
        fixture.Tick(1);
        pushed += fixture.Physics().GetStepStats().Pushed;
        drift = std::max(
            drift, glm::length(fixture.Physics().GetBodyPose(mover)->Position - rest.Position));
    }
    CHECK(pushed == 0);
    CHECK(drift == 0.0);
    CHECK(glm::length(fixture.Physics().GetLinearVelocity(mover)) == 0.0f);
    // A second of stillness is past the solver's sleep time.
    CHECK_FALSE(fixture.Physics().IsBodyActive(mover));

    // Moving it again wakes it and it follows.
    fixture.World->Get<Transform>(mover).Position = vec3(0.0f, 5.0f, 2.0f);
    fixture.Tick(2);
    CHECK(fixture.World->Get<Transform>(mover).Position.z == doctest::Approx(2.0f).epsilon(0.01));
}

TEST_CASE("a body that comes to rest during a step has its final pose read back")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    const Entity box = fixture.SpawnBox(vec3(0.2f, 2.0f, -0.1f));

    u32 ticks = 0;
    do
    {
        fixture.Tick(1);
    } while (fixture.Physics().IsBodyActive(box) && ++ticks < 600);
    REQUIRE_FALSE(fixture.Physics().IsBodyActive(box));

    // The step that put it to sleep read it back, and nothing is read once it sleeps.
    const PhysicsPose body = *fixture.Physics().GetBodyPose(box);
    CHECK(fixture.World->Get<PhysicsPose>(box).Position == body.Position);
    CHECK(fixture.World->Get<PhysicsPose>(box).Rotation == body.Rotation);
    CHECK(fixture.World->Get<Transform>(box).Position == vec3(body.Position));
    fixture.Tick(1);
    CHECK(fixture.Physics().GetStepStats().ReadBack == 0);
}

TEST_CASE("an impulse wakes a sleeping body and changes its velocity by impulse over mass")
{
    const PhysicsFixture fixture;
    fixture.Physics().SetGravity(vec3(0.0f));
    const Entity body = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(body);
    fixture.World->Add<RigidBody>(body, RigidBody{.Mass = 10.0f, .StartAsleep = true});
    fixture.World->Add<Collider>(body,
                                 Collider{.Shape = ColliderShape::Sphere, .Extents = vec3(0.5f)});
    fixture.Tick(1);
    REQUIRE(fixture.Physics().HasBody(body));
    CHECK_FALSE(fixture.Physics().IsBodyActive(body));

    fixture.Physics().AddImpulse(body, vec3(20.0f, 0.0f, 0.0f));
    CHECK(fixture.Physics().IsBodyActive(body));
    CHECK(fixture.Physics().GetLinearVelocity(body).x == doctest::Approx(2.0f));
    CHECK(glm::length(fixture.Physics().GetAngularVelocity(body)) == 0.0f);

    // Off the centre of mass, the same impulse turns it too.
    fixture.Physics().AddImpulseAt(body, vec3(0.0f, 0.0f, 10.0f), dvec3(0.5, 0.0, 0.0));
    CHECK(fixture.Physics().GetLinearVelocity(body).z == doctest::Approx(1.0f));
    CHECK(glm::length(fixture.Physics().GetAngularVelocity(body)) > 0.0f);

    fixture.Tick(1);
    CHECK(fixture.World->Get<PhysicsPose>(body).Position.x > 0.0);
}

TEST_CASE("a sleeping body wakes on request, and an impulse moves no kinematic or static body")
{
    const PhysicsFixture fixture;
    fixture.Physics().SetGravity(vec3(0.0f));
    const Entity sleeper = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(sleeper);
    fixture.World->Add<RigidBody>(sleeper, RigidBody{.StartAsleep = true});
    fixture.World->Add<Collider>(sleeper);
    const Entity wall = fixture.SpawnGround();
    fixture.Tick(1);

    CHECK_FALSE(fixture.Physics().IsBodyActive(sleeper));
    fixture.Physics().ActivateBody(sleeper);
    CHECK(fixture.Physics().IsBodyActive(sleeper));

    fixture.Physics().AddImpulse(wall, vec3(100.0f, 0.0f, 0.0f));
    fixture.Physics().ActivateBody(wall);
    CHECK_FALSE(fixture.Physics().IsBodyActive(wall));
    CHECK(glm::length(fixture.Physics().GetLinearVelocity(wall)) == 0.0f);
}

TEST_CASE("Debris is solver-free against a mover, and still meets the world and other debris")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    const auto spawnDebris = [&](const vec3 position, const bool asleep)
    {
        const Entity debris = fixture.World->CreateEntity();
        fixture.World->Add<Transform>(debris, Transform{.Position = position});
        fixture.World->Add<RigidBody>(
            debris, RigidBody{.Layer = PhysicsLayer::Debris, .Mass = 5.0f, .StartAsleep = asleep});
        fixture.World->Add<Collider>(debris, Collider{.Extents = vec3(0.5f)});
        return debris;
    };
    const Entity resting = spawnDebris(vec3(0.0f, 0.5f, 0.0f), true);

    // A kinematic mover on Moving swept straight through the resting body never touches it.
    const Entity mover = fixture.World->CreateEntity();
    fixture.World->Add<Transform>(mover, Transform{.Position = vec3(-4.0f, 0.5f, 0.0f)});
    fixture.World->Add<RigidBody>(mover, RigidBody{.Motion = MotionType::Kinematic});
    // Clear of the ground beneath it, so a sweep starts touching nothing.
    fixture.World->Add<Collider>(mover, Collider{.Extents = vec3(0.4f)});
    fixture.Tick(1);
    const PhysicsPose before = *fixture.Physics().GetBodyPose(resting);
    for (u32 i = 1; i <= 16; ++i)
    {
        fixture.World->Get<Transform>(mover).Position =
            vec3(-4.0f + 0.5f * static_cast<f32>(i), 0.5f, 0.0f);
        fixture.Tick(1);
    }
    CHECK(fixture.Physics().GetBodyPose(resting)->Position == before.Position);

    // The mover finds it through a query that names the layer, and only through one.
    const Entity self[] = {mover};
    const optional<ShapeHit> named =
        ShapeCast(&fixture.Physics(), mover, PhysicsPose{.Position = dvec3(-4.0, 0.5, 0.0)},
                  dvec3(4.0, 0.5, 0.0), QueryFilter{.Ignore = self});
    REQUIRE(named.has_value());
    CHECK(named->Body == resting);
    const optional<ShapeHit> unnamed =
        ShapeCast(&fixture.Physics(), mover, PhysicsPose{.Position = dvec3(-4.0, 0.5, 0.0)},
                  dvec3(4.0, 0.5, 0.0),
                  QueryFilter{.Layers = AllPhysicsLayers & ~PhysicsLayerBit(PhysicsLayer::Debris),
                              .Ignore = self});
    CHECK_FALSE(unnamed.has_value());

    // A second piece dropped onto the first lands on it rather than passing through to the ground.
    const Entity dropped = spawnDebris(vec3(0.0f, 3.0f, 0.0f), false);
    fixture.Tick(120);
    CHECK(fixture.World->Get<Transform>(dropped).Position.y == doctest::Approx(1.5f).epsilon(0.05));
}

TEST_CASE("stepping the same scene twice produces bit-identical body poses")
{
    const PhysicsFixture first;
    first.SpawnGround();
    first.SpawnBox(vec3(0.3f, 4.0f, -0.2f));
    first.SpawnBox(vec3(-0.4f, 6.0f, 0.1f));
    first.Step(90);

    const PhysicsFixture second;
    second.SpawnGround();
    second.SpawnBox(vec3(0.3f, 4.0f, -0.2f));
    second.SpawnBox(vec3(-0.4f, 6.0f, 0.1f));
    second.Step(90);

    CHECK(first.Physics().HashPoses() == second.Physics().HashPoses());
}

TEST_CASE("StateRecorder save then restore reproduces the saved branch exactly")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    fixture.SpawnBox(vec3(0.2f, 3.0f, 0.1f));

    // Land the box and settle into contact, so the saved state carries contacts and sleep, not
    // only a pose.
    fixture.Step(60);

    const vector<u8> saved = fixture.Physics().SaveState();
    fixture.Step(30);
    const u64 branch = fixture.Physics().HashPoses();

    const VoidResult restored = fixture.Physics().RestoreState(saved);
    REQUIRE(restored.has_value());
    fixture.Step(30);

    CHECK(fixture.Physics().HashPoses() == branch);
}

TEST_CASE("RestoreState reports unreadable bytes rather than asserting")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    fixture.Step(1);

    const vector<u8> garbage(8, 0xAB);
    const VoidResult restored = fixture.Physics().RestoreState(garbage);
    CHECK_FALSE(restored.has_value());
}

TEST_CASE("a mispredict of N replayed ticks advances the physics world by exactly one step")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();
    fixture.SpawnBox(vec3(0.0f, 4.0f, 0.0f));

    PhysicsSystem system;
    TestSupport::TestServices services;
    const SystemContext live = services.Make();
    SystemContext replay = services.Make();
    replay.IsReplay = true;

    const u64 before = fixture.Physics().GetStepCount();
    for (u32 i = 0; i < 8; ++i)
    {
        system.OnUpdate(*fixture.World, FixedStep, replay);
    }
    CHECK(fixture.Physics().GetStepCount() == before);

    system.OnUpdate(*fixture.World, FixedStep, live);
    CHECK(fixture.Physics().GetStepCount() == before + 1);
}

TEST_CASE("the pose hash is a determinism golden, regenerated only on a solver bump")
{
    // A fixed fixture stepped a fixed number of ticks hashes to a fixed value. The solver is built
    // CROSS_PLATFORM_DETERMINISTIC, so this must hold on every host — a mismatch is either a
    // solver version change (regenerate the constant, in the same commit that moves the pin) or a
    // real loss of determinism.
    constexpr u64 GoldenHash = 0xA8DA716C1D93F6FCULL;

    const PhysicsFixture fixture;
    fixture.SpawnGround();
    fixture.SpawnBox(vec3(0.25f, 3.0f, -0.125f));
    fixture.SpawnBox(vec3(-0.125f, 5.0f, 0.25f));
    fixture.Step(120);

    const u64 hash = fixture.Physics().HashPoses();
    INFO("pose hash = " << hash);
    CHECK(hash == GoldenHash);
}

TEST_CASE("a scene with no physics world steps as a no-op")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{.Position = vec3(0.0f, 4.0f, 0.0f)});
    scene->Add<RigidBody>(entity);
    scene->Add<Collider>(entity);

    StepPhysics(*scene, FixedStep);

    CHECK(scene->GetPhysicsWorld() == nullptr);
    CHECK(scene->Get<Transform>(entity).Position.y == doctest::Approx(4.0f));
    CHECK_FALSE(scene->Has<PhysicsPose>(entity));
}

TEST_CASE("stopping a world's physics with many bodies and constraints leaves none behind")
{
    const PhysicsFixture fixture;
    fixture.SpawnGround();

    // Enough bodies that the batched removal covers a real set, every one of them dynamic so the
    // constraints between neighbours are live.
    constexpr u32 BoxCount = 64;
    vector<Entity> boxes;
    for (u32 i = 0; i < BoxCount; ++i)
    {
        boxes.push_back(fixture.SpawnBox(
            vec3(static_cast<f32>(i % 8) * 2.0f, 4.0f, static_cast<f32>(i / 8) * 2.0f)));
    }
    for (u32 i = 1; i < BoxCount; i += 2)
    {
        fixture.World->Add<FixedConstraint>(boxes[i], FixedConstraint{.Target = boxes[i - 1]});
    }

    fixture.Step(1);
    REQUIRE(fixture.Physics().GetBodyCount() == BoxCount + 1);
    REQUIRE(fixture.Physics().GetConstraintCount() == BoxCount / 2);

    TestSupport::TestServices services;
    PhysicsSystem system;
    system.OnStop(*fixture.World, services.Make());

    CHECK(fixture.Physics().GetBodyCount() == 0);
    CHECK(fixture.Physics().GetConstraintCount() == 0);
    CHECK_FALSE(fixture.Physics().HasBody(boxes.front()));

    // The world outlives the stop: the next step builds every body and constraint again.
    fixture.Step(1);
    CHECK(fixture.Physics().GetBodyCount() == BoxCount + 1);
    CHECK(fixture.Physics().GetConstraintCount() == BoxCount / 2);
}

namespace
{
    // A scene with the builtin types and no physics world, ready to run PhysicsSystem alone.
    struct AuthoredFixture
    {
        TypeRegistry Types;
        SystemRegistry Systems;
        TestSupport::TestServices Services;
        Unique<Scene> World;

        AuthoredFixture()
        {
            RegisterBuiltinTypes(Types);
            RegisterBuiltinSystems(Systems);
            World = Scene::Create(Types);
        }

        // Attaches a simulation running PhysicsSystem only, and starts it.
        void Start()
        {
            World->SetSimulation(CreateUnique<SceneSimulation>(
                Systems, vector<SystemId>{SystemIdOf<PhysicsSystem>()}));
            World->StartSimulation(Services.Make());
        }
    };
}

TEST_CASE("a level running PhysicsSystem simulates with no physics world installed by code")
{
    AuthoredFixture fixture;
    Scene& scene = *fixture.World;

    const Entity ground = scene.CreateEntity();
    scene.Add<Transform>(ground, Transform{.Position = vec3(0.0f, -1.0f, 0.0f)});
    scene.Add<RigidBody>(ground,
                         RigidBody{.Motion = MotionType::Static, .Layer = PhysicsLayer::Static});
    scene.Add<Collider>(ground, Collider{.Extents = vec3(20.0f, 1.0f, 20.0f)});
    const Entity box = scene.CreateEntity();
    scene.Add<Transform>(box, Transform{.Position = vec3(0.0f, 4.0f, 0.0f)});
    scene.Add<RigidBody>(box);
    scene.Add<Collider>(box);

    fixture.Start();
    REQUIRE(scene.GetPhysicsWorld() != nullptr);

    const SystemContext context = fixture.Services.Make();
    for (u32 i = 0; i < 10; ++i)
    {
        scene.TickSimulation(FixedStep, context);
    }
    REQUIRE(scene.Has<PhysicsPose>(box));
    CHECK(scene.Get<PhysicsPose>(box).Position.y < 4.0);
}

TEST_CASE("authored PhysicsSettings reach the world PhysicsSystem creates")
{
    AuthoredFixture fixture;
    const Entity settings = fixture.World->CreateEntity();
    fixture.World->Add<PhysicsSettings>(
        settings, PhysicsSettings{
                      .Gravity = vec3(0.0f, -1.5f, 0.0f),
                      .Collisions = {PhysicsCollisionRule{
                          .A = PhysicsLayer::Moving, .B = PhysicsLayer::Moving, .Collide = false}},
                      .MaxBodies = 7,
                      .DebugDraw = true,
                  });
    fixture.Start();

    const PhysicsWorld* world = fixture.World->GetPhysicsWorld();
    REQUIRE(world != nullptr);
    const PhysicsWorldInfo& info = world->GetInfo();
    CHECK(info.MaxBodies == 7);
    CHECK(info.Gravity.y == doctest::Approx(-1.5f));
    CHECK(world->IsDebugDrawEnabled());
    CHECK_FALSE(LayersCollide(info.Matrix, PhysicsLayer::Moving, PhysicsLayer::Moving));

    // Every row the rule leaves alone, and the rest of the Moving row, match the default.
    const CollisionMatrix defaults = DefaultCollisionMatrix();
    const u32 movingBit = PhysicsLayerBit(PhysicsLayer::Moving);
    u32 differing = 0;
    for (u32 row = 0; row < PhysicsLayerCount; ++row)
    {
        differing |= (info.Matrix.Rows[row] ^ defaults.Rows[row]) &
                     (row == static_cast<u32>(PhysicsLayer::Moving) ? ~movingBit : ~0U);
    }
    CHECK(differing == 0);
}

TEST_CASE("a collision rule sets its pair in both directions")
{
    const PhysicsWorldInfo info = ToPhysicsWorldInfo(PhysicsSettings{
        .Collisions =
            {
                PhysicsCollisionRule{
                    .A = PhysicsLayer::Character, .B = PhysicsLayer::Trigger, .Collide = false},
                PhysicsCollisionRule{
                    .A = PhysicsLayer::Query, .B = PhysicsLayer::Static, .Collide = true},
                PhysicsCollisionRule{
                    .A = PhysicsLayer::Moving, .B = PhysicsLayer::Character, .Collide = false},
            },
    });

    CHECK(IsSymmetric(info.Matrix));
    CHECK_FALSE(LayersCollide(info.Matrix, PhysicsLayer::Character, PhysicsLayer::Trigger));
    CHECK_FALSE(LayersCollide(info.Matrix, PhysicsLayer::Trigger, PhysicsLayer::Character));
    CHECK(LayersCollide(info.Matrix, PhysicsLayer::Static, PhysicsLayer::Query));
    CHECK(LayersCollide(info.Matrix, PhysicsLayer::Query, PhysicsLayer::Static));
    CHECK_FALSE(LayersCollide(info.Matrix, PhysicsLayer::Character, PhysicsLayer::Moving));
}

TEST_CASE("a world installed before start is the one the scene keeps")
{
    AuthoredFixture fixture;
    fixture.World->Add<PhysicsSettings>(fixture.World->CreateEntity(),
                                        PhysicsSettings{.MaxBodies = 7});
    fixture.World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{.MaxBodies = 32}));
    const PhysicsWorld* installed = fixture.World->GetPhysicsWorld();

    fixture.Start();

    CHECK(fixture.World->GetPhysicsWorld() == installed);
    CHECK(installed->GetInfo().MaxBodies == 32);
    CHECK(&EnsurePhysicsWorld(*fixture.World) == installed);
}

TEST_CASE("a zero budget in authored settings falls back to the default")
{
    AuthoredFixture fixture;
    fixture.World->Add<PhysicsSettings>(
        fixture.World->CreateEntity(),
        PhysicsSettings{.MaxBodies = 0, .MaxBodyPairs = 0, .MaxContactConstraints = 0});

    const PhysicsWorldInfo defaults;
    const PhysicsWorldInfo& info = EnsurePhysicsWorld(*fixture.World).GetInfo();
    CHECK(info.MaxBodies == defaults.MaxBodies);
    CHECK(info.MaxBodyPairs == defaults.MaxBodyPairs);
    CHECK(info.MaxContactConstraints == defaults.MaxContactConstraints);
}
