// Lag compensation: the view tick a client stamps on its input, the server's pose history of the
// bodies that opt in, and the rewind scope a query made against a client's view runs inside. Pure
// CPU — headless scenes with a physics world, no transport and no device.

#include <doctest/doctest.h>

#include <Veng/Net/InputFeed.h>
#include <Veng/Net/LagCompensation.h>
#include <Veng/Net/Replication.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Physics/Queries.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinSystems.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SystemRegistry.h>

#include <glm/gtc/quaternion.hpp>

#include <cmath>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;

    // One wire step of the view delay, the precision the stamp survives to.
    constexpr f64 WireStep = 1.0 / static_cast<f64>(InputViewDelayStepsPerTick);

    // The systems under test read only the scene, the tick and the role; the service references are
    // never dereferenced.
    struct ContextStorage
    {
        alignas(16) unsigned char Bytes[64]{};

        SystemContext Make(const u64 tick, const NetRole role = NetRole::Server)
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(Bytes),
                .Input = *reinterpret_cast<Input*>(Bytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(Bytes),
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(Bytes),
                .Localization = *reinterpret_cast<Localization::Localization*>(Bytes),
                .Tick = tick,
                .Role = role,
            };
        }
    };

    ActionState SomeInput()
    {
        ActionState state;
        state.Actions = {ActionSample{
            .Id = ActionId{0xA1}, .Value = vec2(1.0f, 0.0f), .Phase = ActionPhase::Ongoing}};
        return state;
    }

    // A server scene with a physics world whose kinematic boxes follow scripted paths, recorded by the
    // real PoseHistorySystem after each step.
    struct RewindFixture
    {
        TypeRegistry Types;
        Unique<Scene> World;
        PoseHistorySystem Recorder;
        ContextStorage Context;
        u64 Tick = 0;

        RewindFixture()
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{}));
        }

        PhysicsWorld& Physics() const { return *World->GetPhysicsWorld(); }

        Entity SpawnBox(const vec3 position, const bool compensated) const
        {
            const Entity box = World->CreateEntity();
            World->Add<Transform>(box, Transform{.Position = position});
            World->Add<RigidBody>(box, RigidBody{.Motion = MotionType::Kinematic});
            World->Add<Collider>(box, Collider{.Shape = ColliderShape::Box, .Extents = vec3(0.5f)});
            if (compensated)
            {
                World->Add<LagCompensated>(box);
            }
            return box;
        }

        // Advances one tick: every box's kinematic target is set by @p place, then the world steps and
        // the history records the post-step state, as a level naming the two systems in order does.
        template <class Place>
        void Advance(Place&& place)
        {
            ++Tick;
            place(Tick);
            StepPhysics(*World, FixedStep);
            Recorder.OnUpdate(*World, FixedStep, Context.Make(Tick));
        }

        const PoseSample& SampleAt(const Entity entity, const u64 tick) const
        {
            for (const PoseSample& sample : World->GetPoseHistory()->GetSamples(entity))
            {
                if (sample.Tick == tick)
                {
                    return sample;
                }
            }
            FAIL("no sample recorded at tick " << tick);
            static const PoseSample none;
            return none;
        }
    };

    // A ray straight down through x at z = 0, reporting the body it meets first.
    Entity HitBelow(const PhysicsWorld& world, const f64 x)
    {
        const optional<RayHit> hit =
            Raycast(&world, dvec3(x, 10.0, 0.0), vec3(0.0f, -1.0f, 0.0f), 20.0f);
        return hit ? hit->Body : Entity::Null;
    }
}

TEST_CASE("A client's stamped view tick reaches the server's seat to within a wire step")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    // Stamped, encoded, decoded and consumed at its own tick, as the scheduled feed does.
    InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    constexpr f64 viewTick = 93.3;
    send.Stamp(99, SomeInput(), 92.0);
    send.Stamp(100, SomeInput(), viewTick);

    const Result<InputPacket> decoded = DecodeInputPacket(send.Encode(0, types), types);
    REQUIRE(decoded.has_value());
    InputJitterBuffer jitter;
    jitter.Ingest(*decoded);
    REQUIRE(jitter.ConsumeForTick(100).has_value());

    const Unique<Scene> scene = Scene::Create(types);
    const Entity seat = scene->CreateEntity();
    scene->Add<InputViewDelay>(seat).Ticks = jitter.GetLastViewDelayTicks();
    CHECK(std::abs(SeatViewTick(*scene, seat, 100) - viewTick) <= WireStep);

    // A coasted tick keeps the delay of the input it duplicates.
    REQUIRE(jitter.ConsumeForTick(101).has_value());
    CHECK(std::abs((101.0 - jitter.GetLastViewDelayTicks()) - (viewTick + 1.0)) <= WireStep);

    // A seat fed locally draws the present.
    const Entity local = scene->CreateEntity();
    CHECK(SeatViewTick(*scene, local, 100) == 100.0);
}

TEST_CASE("A view delay is carried as non-negative and bounded by the wire")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    InputSendBuffer send(InputSendBuffer::Settings{.Redundancy = 3});
    send.Stamp(10, SomeInput(), 11.5);     // a view ahead of its input reads as the present
    send.Stamp(11, SomeInput(), -10000.0); // a view older than the wire carries clamps
    send.Stamp(12, SomeInput());           // no view drawn in the past

    const Result<InputPacket> decoded = DecodeInputPacket(send.Encode(0, types), types);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->Inputs.size() == 3);
    CHECK(decoded->Inputs[0].ViewDelayTicks == 0.0f);
    CHECK(decoded->Inputs[1].ViewDelayTicks == MaxInputViewDelayTicks);
    CHECK(decoded->Inputs[2].ViewDelayTicks == 0.0f);
}

TEST_CASE("A client stamps the tick its remote-interpolation clock is drawing")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    RegisterBuiltinSystems(systems);
    const Unique<Scene> scene = Scene::Create(types);

    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat);
    scene->Add<SeatInput>(seat);
    scene->Add<PlayerInput>(seat).State = SomeInput();

    // No simulation, so nothing is drawn in the past: the view is the present.
    InputSendBuffer send;
    StampLocalSeatInput(send, *scene, 10);
    REQUIRE(send.GetWindow().size() == 1);
    CHECK(send.GetWindow().back().ViewDelayTicks == 0.0f);

    scene->SetSimulation(CreateUnique<SceneSimulation>(
        systems, vector<SystemId>{SystemIdOf<RemoteInterpolationSystem>()}));
    auto* interpolation = scene->GetSimulation()->FindSystem<RemoteInterpolationSystem>();
    REQUIRE(interpolation != nullptr);

    const Entity remote = scene->CreateEntity();
    scene->Add<Transform>(remote);
    auto& samples = scene->Add<RemoteInterpolation>(remote).Samples;
    for (u64 t = 0; t <= 8; t += 2)
    {
        samples.push_back(RemoteSample{.ServerTick = t});
    }
    ContextStorage context;
    interpolation->OnUpdate(*scene, 0.0f, context.Make(0, NetRole::Client));
    const optional<f64> drawn = RemotePlaybackTick(*scene);
    REQUIRE(drawn.has_value());

    StampLocalSeatInput(send, *scene, 11);
    CHECK(11.0 - send.GetWindow().back().ViewDelayTicks == doctest::Approx(*drawn));
}

TEST_CASE("The pose history blends snapshot-tick samples and holds no more than its reach")
{
    RewindFixture fixture;
    const Entity box = fixture.SpawnBox(vec3(0.0f), true);

    // Odd ticks jump aside, so a blend that used them would land far from one that skips them.
    const auto zigzag = [&](const u64 tick)
    {
        const f32 aside = tick % 2 == 1 ? 5.0f : 0.0f;
        fixture.World->Get<Transform>(box).Position = vec3(static_cast<f32>(tick), aside, 0.0f);
    };
    for (u32 i = 0; i < 40; ++i)
    {
        fixture.Advance(zigzag);
    }

    const PoseHistory& history = *fixture.World->GetPoseHistory();
    const std::span<const PoseSample> ring = history.GetSamples(box);
    REQUIRE_FALSE(ring.empty());
    CHECK(history.GetMaxRewindTicks() == 15);
    CHECK(ring.back().Tick - ring.front().Tick <= history.GetMaxRewindTicks());

    const PoseSample& lo = fixture.SampleAt(box, 32);
    const PoseSample& hi = fixture.SampleAt(box, 34);
    for (const f64 viewTick : {32.0, 32.5, 33.0, 33.75})
    {
        const optional<RewoundBodyPose> pose = history.RewoundPose(box, viewTick);
        REQUIRE(pose.has_value());
        const f64 alpha = (viewTick - 32.0) / 2.0;
        const dvec3 expected = glm::mix(lo.Pose.Position, hi.Pose.Position, alpha);
        CHECK(glm::length(pose->Pose.Position - expected) < 1e-9);
        CHECK(glm::length(pose->LinearVelocity - glm::mix(lo.LinearVelocity, hi.LinearVelocity,
                                                          static_cast<f32>(alpha))) < 1e-4f);
    }

    // A destroyed entity leaves nothing behind once the next tick closes.
    fixture.World->DestroyEntity(box);
    fixture.Advance([](u64) {});
    CHECK(history.GetEntityCount() == 0);
}

TEST_CASE("A client neither records nor replays a pose history")
{
    RewindFixture fixture;
    fixture.SpawnBox(vec3(0.0f), true);
    StepPhysics(*fixture.World, FixedStep);
    fixture.Recorder.OnUpdate(*fixture.World, FixedStep, fixture.Context.Make(1, NetRole::Client));
    CHECK(fixture.World->GetPoseHistory() == nullptr);
}

TEST_CASE("A rewind scope hits what the client saw and restores every body exactly")
{
    RewindFixture fixture;
    const Entity target = fixture.SpawnBox(vec3(0.0f), true);
    const Entity shooter = fixture.SpawnBox(vec3(0.0f, 0.0f, 10.0f), true);
    const Entity unmarked = fixture.SpawnBox(vec3(0.0f, 0.0f, -10.0f), false);

    // Every box slides along +x at half a metre a tick, each on its own z.
    const auto slide = [&](const u64 tick)
    {
        const f32 x = 0.5f * static_cast<f32>(tick);
        fixture.World->Get<Transform>(target).Position = vec3(x, 0.0f, 0.0f);
        fixture.World->Get<Transform>(shooter).Position = vec3(x, 0.0f, 10.0f);
        fixture.World->Get<Transform>(unmarked).Position = vec3(x, 0.0f, -10.0f);
    };
    for (u32 i = 0; i < 40; ++i)
    {
        fixture.Advance(slide);
    }

    PhysicsWorld& world = fixture.Physics();
    const vector<u8> targetBefore = world.SaveBodyState(target);
    const PhysicsPose shooterBefore = *world.GetBodyPose(shooter);
    const PhysicsPose unmarkedBefore = *world.GetBodyPose(unmarked);
    const vec3 velocityBefore = world.GetLinearVelocity(target);

    // Where the target stood at tick 30: the present has it five metres further on.
    const f64 seenAt = fixture.SampleAt(target, 30).Pose.Position.x;
    CHECK(HitBelow(world, seenAt) == Entity::Null);
    {
        const RewindScope scope(*fixture.World, world, 30.0, shooter);
        CHECK(scope.GetViewTick() == 30.0);
        CHECK(HitBelow(world, seenAt) == target);

        // Only the marked, unexcluded body moved.
        CHECK(scope.GetMovedCount() == 1);
        CHECK(world.GetBodyPose(shooter)->Position == shooterBefore.Position);
        CHECK(world.GetBodyPose(unmarked)->Position == unmarkedBefore.Position);
    }
    CHECK(HitBelow(world, seenAt) == Entity::Null);
    CHECK(world.SaveBodyState(target) == targetBefore);
    CHECK(world.GetLinearVelocity(target) == velocityBefore);
}

TEST_CASE("A rewind older than the reach stops at the reach, and the present moves nothing")
{
    RewindFixture fixture;
    const Entity target = fixture.SpawnBox(vec3(0.0f), true);
    for (u32 i = 0; i < 40; ++i)
    {
        fixture.Advance(
            [&](const u64 tick)
            { fixture.World->Get<Transform>(target).Position.x = static_cast<f32>(tick); });
    }

    PhysicsWorld& world = fixture.Physics();
    const u64 reach = fixture.World->GetPoseHistory()->GetMaxRewindTicks();
    {
        const RewindScope scope(*fixture.World, world, 0.0, Entity::Null);
        CHECK(scope.GetViewTick() == static_cast<f64>(fixture.Tick - reach));
        CHECK(world.GetBodyPose(target)->Position ==
              fixture.SampleAt(target, fixture.Tick - reach).Pose.Position);
    }
    {
        const RewindScope scope(*fixture.World, world, static_cast<f64>(fixture.Tick) + 3.0,
                                Entity::Null);
        CHECK(scope.GetMovedCount() == 0);
    }
}
