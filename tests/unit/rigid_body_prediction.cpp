// Rollback of a predicted rigid body. A body marked Predicted has its solver state saved after every
// step; a rollback restores it to the authoritative tick and the replay re-simulates it against the
// world, while every body nobody predicts is put back exactly as the replay found it. Pure CPU: a
// headless Scene and a solver, PhysicsSystem driven by hand under a velocity controller, no GPU.

#include <doctest/doctest.h>

#include <Veng/Net/PredictionHistory.h>
#include <Veng/Net/Reconciliation.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsSystem.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include "support/TestServices.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <vector>

using namespace Veng;

namespace
{
    constexpr f32 FixedStep = 1.0f / 60.0f;
    constexpr f32 Speed = 10.0f;
    constexpr ActionId MoveAction{0x27C7B51A09132217ULL};

    PlayerInput InputOf(const vec2 command)
    {
        PlayerInput input;
        input.State.Actions = {
            ActionSample{.Id = MoveAction, .Value = command, .Phase = ActionPhase::Ongoing}};
        return input;
    }

    vec2 CommandOf(const PlayerInput& input)
    {
        return input.GetValue(MoveAction);
    }

    // A varied tape, so a replayed path is not a straight line.
    vec2 Weave(const u64 tick)
    {
        return {std::sin(static_cast<f32>(tick) * 0.3f) * 0.5f, 1.0f};
    }

    // Counts the replayed ticks a rollback flags as its first.
    struct ReplayStartCounter final : SceneSystem
    {
        u32 Starts = 0;

        void OnUpdate(Scene&, const f32, const SystemContext& context) override
        {
            Starts += context.IsReplayStart ? 1u : 0u;
        }
    };

    // A weightless world holding one vehicle — a 1 kg sphere whose velocity a controller sets from
    // the tick's command before each step — and any loose spheres a case adds. Two rigs built the same
    // way hold the same bodies under the same ids, which is what lets one stand for a fresh run of
    // the other.
    struct Rig
    {
        TypeRegistry Types;
        Unique<Scene> World;
        PhysicsSystem Physics;
        Entity Vehicle;

        explicit Rig(const bool predicted = true)
        {
            RegisterBuiltinTypes(Types);
            World = Scene::Create(Types);
            World->SetPhysicsWorld(PhysicsWorld::Create(PhysicsWorldInfo{.Gravity = vec3(0.0f)}));
            Vehicle = Spawn(vec3(0.0f), predicted);
        }

        Entity Spawn(const vec3 position, const bool predicted) const
        {
            const Entity entity = World->CreateEntity();
            World->Add<Transform>(entity, Transform{.Position = position});
            World->Add<RigidBody>(entity, RigidBody{.Motion = MotionType::Dynamic, .Mass = 1.0f});
            World->Add<Collider>(entity,
                                 Collider{.Shape = ColliderShape::Sphere, .Extents = vec3(0.5f)});
            if (predicted)
            {
                World->Add<Predicted>(entity);
            }
            return entity;
        }

        PhysicsWorld& Solver() const { return *World->GetPhysicsWorld(); }

        // Creates the bodies the spawns describe, at rest.
        void Start(TestSupport::TestServices& services) { Step(services, 0, vec2(0.0f)); }

        // One tick: on a rollback's first replayed tick, seat the vehicle on its authoritative
        // Transform, as a consumer whose authoritative state is a replicated component does; then
        // set its velocity from the command and step.
        void Step(TestSupport::TestServices& services, const u64 tick, const vec2 command,
                  const bool replay = false, const bool start = false)
        {
            SystemContext context = services.Make();
            context.Tick = tick;
            context.IsReplay = replay;
            context.IsReplayStart = start;
            if (start)
            {
                const Transform& seat = World->Get<Transform>(Vehicle);
                Solver().SetBodyPose(Vehicle, PhysicsPose{.Position = dvec3(seat.Position),
                                                          .Rotation = seat.Rotation});
            }
            Solver().SetLinearVelocity(Vehicle, vec3(command.x, 0.0f, command.y) * Speed);
            Physics.OnUpdate(*World, FixedStep, context);
        }
    };

    vector<u8> SerializeTransform(const TypeRegistry& types, const Transform& transform)
    {
        vector<u8> out;
        WriteFields(out, &transform, types.Info(TypeIdOf<Transform>()), types);
        return out;
    }
}

TEST_CASE("A predicted body replayed twice from its restored state ends bit-identical")
{
    TestSupport::TestServices services;
    Rig rig;
    rig.Start(services);
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        rig.Step(services, tick, Weave(tick));
    }
    REQUIRE(rig.Solver().HasRecordedState(rig.Vehicle, 20));
    const vector<u8> live = rig.Solver().SaveBodyState(rig.Vehicle);

    const auto replayFrom20 = [&]
    {
        BeginPhysicsReplay(*rig.World, 20);
        for (u64 tick = 21; tick <= 40; ++tick)
        {
            rig.Step(services, tick, Weave(tick), true);
        }
        EndPhysicsReplay(*rig.World);
        return rig.Solver().SaveBodyState(rig.Vehicle);
    };

    const vector<u8> first = replayFrom20();
    const vector<u8> second = replayFrom20();
    CHECK(first == second);
    // The same inputs from the same state reproduce the prediction they were recorded from.
    CHECK(first == live);
}

TEST_CASE("A mispredicted body converges on a fresh simulation from the authoritative state")
{
    // The client strafes for six ticks the server never saw. At C the server's pose disagrees, so
    // the reconcile rolls back: the body is restored to C, seated on the authoritative Transform on
    // the first replayed tick, and replayed. A fresh world seated at the same state and run the same
    // inputs is the reference, and the two agree bit for bit.
    constexpr u64 Consumed = 25;
    constexpr u64 Now = 40;
    const auto truth = [](const u64 tick) { return Weave(tick); };
    const auto predicted = [&](const u64 tick)
    { return tick >= 8 && tick <= 13 ? vec2(1.0f, 0.0f) : truth(tick); };

    TestSupport::TestServices services;
    Rig server;
    server.Start(services);
    for (u64 tick = 1; tick <= Consumed; ++tick)
    {
        server.Step(services, tick, truth(tick));
    }
    const Transform authoritative = server.World->Get<Transform>(server.Vehicle);

    Rig client;
    client.Start(services);
    Net::PredictionHistory history;
    history.Track(client.Vehicle);
    for (u64 tick = 1; tick <= Now; ++tick)
    {
        client.Step(services, tick, predicted(tick));
        history.Record(tick, InputOf(predicted(tick)), *client.World);
    }
    const vec3 mispredicted = client.World->Get<Transform>(client.Vehicle).Position;

    PredictedRecord record;
    record.Entity = client.Vehicle;
    record.Components.push_back(PredictedRecord::Component{
        .Type = TypeIdOf<Transform>(), .Bytes = SerializeTransform(client.Types, authoritative)});

    ReplayStartCounter counter;
    const Net::ReplayTick replay =
        [&](Scene& scene, const u64 tick, const PlayerInput& input, const bool replayStart)
    {
        SystemContext context = services.Make();
        context.IsReplay = true;
        context.IsReplayStart = replayStart;
        counter.OnUpdate(scene, FixedStep, context);
        client.Step(services, tick, CommandOf(input), true, replayStart);
    };
    const Net::ReconcileResult result =
        Net::Reconcile(*client.World, history, std::span(&record, 1), Consumed, replay,
                       Net::ReconcileTolerances{});

    REQUIRE(result.Corrected);
    CHECK(result.ReplayedTicks == Now - Consumed);
    CHECK(counter.Starts == 1);
    CHECK_FALSE(client.Solver().IsReplaying());

    Rig fresh;
    fresh.Start(services);
    fresh.Solver().SetBodyPose(fresh.Vehicle, PhysicsPose{.Position = dvec3(authoritative.Position),
                                                          .Rotation = authoritative.Rotation});
    for (u64 tick = Consumed + 1; tick <= Now; ++tick)
    {
        fresh.Step(services, tick, truth(tick));
    }

    const optional<PhysicsPose> replayed = client.Solver().GetBodyPose(client.Vehicle);
    const optional<PhysicsPose> reference = fresh.Solver().GetBodyPose(fresh.Vehicle);
    REQUIRE(replayed.has_value());
    REQUIRE(reference.has_value());
    CHECK(replayed->Position == reference->Position);
    CHECK(replayed->Rotation == reference->Rotation);
    CHECK(client.Solver().GetLinearVelocity(client.Vehicle) ==
          fresh.Solver().GetLinearVelocity(fresh.Vehicle));
    // The correction moved the body: the strafe was not a no-op the replay merely repeated.
    CHECK(glm::length(mispredicted - client.World->Get<Transform>(client.Vehicle).Position) > 0.5f);
}

TEST_CASE("A body nobody predicts ends a replay where a twin with no replay has it")
{
    TestSupport::TestServices services;
    Rig rolled;
    Rig twin;
    const Entity drifter = rolled.Spawn(vec3(5.0f, 0.0f, 0.0f), false);
    REQUIRE(twin.Spawn(vec3(5.0f, 0.0f, 0.0f), false) == drifter);
    for (Rig* rig : {&rolled, &twin})
    {
        rig->Start(services);
        rig->Solver().SetLinearVelocity(drifter, vec3(0.0f, 0.0f, 1.0f));
        for (u64 tick = 1; tick <= 30; ++tick)
        {
            rig->Step(services, tick, Weave(tick));
        }
    }

    BeginPhysicsReplay(*rolled.World, 20);
    for (u64 tick = 21; tick <= 30; ++tick)
    {
        rolled.Step(services, tick, Weave(tick), true);
    }
    EndPhysicsReplay(*rolled.World);

    CHECK(rolled.Solver().SaveBodyState(drifter) == twin.Solver().SaveBodyState(drifter));
    CHECK(rolled.World->Get<Transform>(drifter).Position ==
          twin.World->Get<Transform>(drifter).Position);
    CHECK(rolled.World->Get<PhysicsPose>(drifter).Position ==
          twin.World->Get<PhysicsPose>(drifter).Position);

    // Live ticks after the rollback keep the two in step.
    for (Rig* rig : {&rolled, &twin})
    {
        for (u64 tick = 31; tick <= 35; ++tick)
        {
            rig->Step(services, tick, Weave(tick));
        }
    }
    CHECK(rolled.Solver().SaveBodyState(drifter) == twin.Solver().SaveBodyState(drifter));
    CHECK(rolled.Solver().SaveBodyState(rolled.Vehicle) ==
          twin.Solver().SaveBodyState(twin.Vehicle));
}

TEST_CASE("A body the replay strikes is put back as it stood before the replay")
{
    // Live, the vehicle waits ten ticks and then drives away from the loose body. The replay drives
    // it straight into the loose body instead, as a correction would; once the replay ends, the
    // loose body is back where the live ticks left it, untouched.
    TestSupport::TestServices services;
    Rig rig;
    const Entity loose = rig.Spawn(vec3(0.0f, 0.0f, 2.5f), false);
    rig.Start(services);
    for (u64 tick = 1; tick <= 30; ++tick)
    {
        rig.Step(services, tick, tick <= 10 ? vec2(0.0f) : vec2(1.0f, 0.0f));
    }
    const vector<u8> before = rig.Solver().SaveBodyState(loose);
    const Transform transformBefore = rig.World->Get<Transform>(loose);

    BeginPhysicsReplay(*rig.World, 10);
    f32 struck = 0.0f;
    for (u64 tick = 11; tick <= 30; ++tick)
    {
        rig.Step(services, tick, vec2(0.0f, 1.0f), true);
        struck = std::max(struck, glm::length(rig.Solver().GetLinearVelocity(loose)));
    }
    EndPhysicsReplay(*rig.World);

    CHECK(struck > 1.0f);
    CHECK(rig.Solver().SaveBodyState(loose) == before);
    CHECK(rig.World->Get<Transform>(loose).Position == transformBefore.Position);
    CHECK(rig.World->Get<Transform>(loose).Rotation == transformBefore.Rotation);
}

TEST_CASE("A struck body marked Predicted rolls back with the vehicle")
{
    // The client strafes clear of the loose body the server's vehicle drives into after C. The
    // rollback restores both predicted bodies to C and replays, so the vehicle strikes the loose
    // body as the server's did; a fresh world started from the same state at C and run the same
    // inputs agrees with both bodies bit for bit.
    constexpr u64 Consumed = 15;
    constexpr u64 Now = 40;
    const auto truth = [](const u64 tick)
    { return tick <= Consumed ? vec2(0.0f) : vec2(0.0f, 1.0f); };
    const auto predicted = [&](const u64 tick)
    { return tick >= 3 && tick <= 10 ? vec2(1.0f, 0.0f) : truth(tick); };
    const vec3 looseAt(0.0f, 0.0f, 3.0f);

    TestSupport::TestServices services;
    Rig server;
    (void)server.Spawn(looseAt, false);
    server.Start(services);
    for (u64 tick = 1; tick <= Consumed; ++tick)
    {
        server.Step(services, tick, truth(tick));
    }
    const Transform authoritative = server.World->Get<Transform>(server.Vehicle);

    Rig client;
    const Entity loose = client.Spawn(looseAt, true);
    client.Start(services);
    Net::PredictionHistory history;
    history.Track(client.Vehicle);
    vector<u8> vehicleAtC;
    vector<u8> looseAtC;
    for (u64 tick = 1; tick <= Now; ++tick)
    {
        client.Step(services, tick, predicted(tick));
        history.Record(tick, InputOf(predicted(tick)), *client.World);
        if (tick == Consumed)
        {
            vehicleAtC = client.Solver().SaveBodyState(client.Vehicle);
            looseAtC = client.Solver().SaveBodyState(loose);
        }
    }
    // Live, the strafe carried the vehicle clear: the loose body was never struck.
    REQUIRE(client.Solver().GetLinearVelocity(loose) == vec3(0.0f));

    PredictedRecord record;
    record.Entity = client.Vehicle;
    record.Components.push_back(PredictedRecord::Component{
        .Type = TypeIdOf<Transform>(), .Bytes = SerializeTransform(client.Types, authoritative)});
    const Net::ReplayTick replay =
        [&](Scene&, const u64 tick, const PlayerInput& input, const bool replayStart)
    { client.Step(services, tick, CommandOf(input), true, replayStart); };
    REQUIRE(Net::Reconcile(*client.World, history, std::span(&record, 1), Consumed, replay,
                           Net::ReconcileTolerances{})
                .Corrected);

    Rig fresh;
    REQUIRE(fresh.Spawn(looseAt, true) == loose);
    fresh.Start(services);
    REQUIRE(fresh.Solver().RestoreBodyState(fresh.Vehicle, vehicleAtC).has_value());
    REQUIRE(fresh.Solver().RestoreBodyState(loose, looseAtC).has_value());
    fresh.Solver().SetBodyPose(fresh.Vehicle, PhysicsPose{.Position = dvec3(authoritative.Position),
                                                          .Rotation = authoritative.Rotation});
    for (u64 tick = Consumed + 1; tick <= Now; ++tick)
    {
        fresh.Step(services, tick, truth(tick));
    }

    CHECK(glm::length(client.Solver().GetLinearVelocity(loose)) > 1.0f);
    CHECK(client.Solver().SaveBodyState(loose) == fresh.Solver().SaveBodyState(loose));
    CHECK(client.Solver().SaveBodyState(client.Vehicle) ==
          fresh.Solver().SaveBodyState(fresh.Vehicle));
}

TEST_CASE("The step count counts live steps across any number of rollbacks")
{
    TestSupport::TestServices services;
    Rig rig;
    (void)rig.Spawn(vec3(4.0f, 0.0f, 0.0f), false);
    rig.Start(services);
    u64 live = 1;
    u64 tick = 0;
    for (const u64 window : {3u, 10u, 1u, 17u})
    {
        for (u64 i = 0; i < 20; ++i)
        {
            ++tick;
            rig.Step(services, tick, Weave(tick));
            ++live;
        }
        BeginPhysicsReplay(*rig.World, tick - window);
        CHECK(rig.Solver().IsReplaying());
        for (u64 replayed = tick - window + 1; replayed <= tick; ++replayed)
        {
            rig.Step(services, replayed, Weave(replayed), true);
        }
        EndPhysicsReplay(*rig.World);
        CHECK_FALSE(rig.Solver().IsReplaying());
        CHECK(rig.Solver().GetStepCount() == live);
    }
}

TEST_CASE("A scene with nothing predicted keeps no record and steps once per mispredict")
{
    TestSupport::TestServices services;
    Rig rig(false);
    rig.Start(services);
    for (u64 tick = 1; tick <= 10; ++tick)
    {
        rig.Step(services, tick, Weave(tick));
    }
    CHECK(rig.Solver().GetRecordedBodyCount() == 0);
    CHECK_FALSE(rig.Solver().HasRecordedState(rig.Vehicle, 10));

    const u64 steps = rig.Solver().GetStepCount();
    const vector<u8> state = rig.Solver().SaveBodyState(rig.Vehicle);
    BeginPhysicsReplay(*rig.World, 5);
    CHECK_FALSE(rig.Solver().IsReplaying());
    for (u64 tick = 6; tick <= 10; ++tick)
    {
        SystemContext replay = services.Make();
        replay.Tick = tick;
        replay.IsReplay = true;
        rig.Physics.OnUpdate(*rig.World, FixedStep, replay);
    }
    EndPhysicsReplay(*rig.World);
    CHECK(rig.Solver().GetStepCount() == steps);
    CHECK(rig.Solver().SaveBodyState(rig.Vehicle) == state);

    rig.Step(services, 11, Weave(11));
    CHECK(rig.Solver().GetStepCount() == steps + 1);
}

TEST_CASE("A body's record starts when it is marked Predicted and drops when it is not")
{
    TestSupport::TestServices services;
    Rig rig(false);
    rig.Start(services);
    rig.Step(services, 1, Weave(1));
    CHECK(rig.Solver().GetRecordedBodyCount() == 0);

    rig.World->Add<Predicted>(rig.Vehicle);
    rig.Step(services, 2, Weave(2));
    CHECK(rig.Solver().GetRecordedBodyCount() == 1);
    CHECK(rig.Solver().HasRecordedState(rig.Vehicle, 2));
    CHECK_FALSE(rig.Solver().HasRecordedState(rig.Vehicle, 1));

    // A tick a full window later reuses the slot, and the older save is gone.
    rig.Step(services, 2 + PredictionWindowTicks, Weave(3));
    CHECK(rig.Solver().HasRecordedState(rig.Vehicle, 2 + PredictionWindowTicks));
    CHECK_FALSE(rig.Solver().HasRecordedState(rig.Vehicle, 2));

    (void)rig.World->Remove<Predicted>(rig.Vehicle);
    rig.Step(services, 3 + PredictionWindowTicks, Weave(4));
    CHECK(rig.Solver().GetRecordedBodyCount() == 0);
}
