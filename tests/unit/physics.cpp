// Physics: the rigid-body module's world/body model, the layer table, the fixed step's
// component reconciliation, the PhysicsPose/SyncTransform seam, the replay gate, and the
// StateRecorder round-trip. Pure CPU — a headless Scene with no Context and no GPU.

#include <doctest/doctest.h>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

using namespace Veng;

namespace
{
    // The Sim tick the module is arranged around; every step below uses it so a step count is a
    // simulated duration.
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // A SystemContext the physics system never dereferences: it reads IsReplay and Debug and
    // touches neither the Input, the TaskSystem, nor the AssetManager.
    struct ContextStorage
    {
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};
        alignas(16) unsigned char AssetsBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(TasksBytes),
            };
        }
    };

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
    ContextStorage storage;
    const SystemContext live = storage.Make();
    SystemContext replay = storage.Make();
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
