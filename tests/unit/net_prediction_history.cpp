// Prediction-history ring cases. PredictionHistory is device-free: it captures a tracked entity
// set's replicated component state per client tick (WriteFields into pooled scratch) alongside that
// tick's seat input, restores a recorded tick over the live scene (ReadFields), yields the ascending
// input run for a replay, and trims confirmed history. These exercise it over an in-process Scene —
// field-exact capture/restore, replicated-only capture, the input run, ring trim and overflow, and a
// component added or removed across the ring — with no socket and no device.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <Veng/Net/PredictionHistory.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    constexpr ActionId MoveAction{0xA1};

    // A seat input carrying one held move axis — the resolved input a recorded tick stamps.
    PlayerInput MoveInput(const vec2 move)
    {
        PlayerInput input;
        input.State.Actions = {
            ActionSample{.Id = MoveAction, .Value = move, .Phase = ActionPhase::Ongoing}};
        return input;
    }

    struct World
    {
        TypeRegistry Types;
        Unique<Scene> Scene;

        World()
        {
            RegisterBuiltinTypes(Types);
            Scene = Scene::Create(Types);
        }
    };
}

TEST_CASE("prediction history: capture then mutate then restore is field-exact")
{
    World world;
    const Entity pawn = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(pawn, Transform{.Position = vec3(1.0f, 2.0f, 3.0f)});

    PredictionHistory history;
    history.Track(pawn);
    history.Record(1, MoveInput(vec2(1.0f, 0.0f)), *world.Scene);

    // The live pose drifts; the restore rewinds it exactly.
    world.Scene->Get<Transform>(pawn).Position = vec3(9.0f, 9.0f, 9.0f);
    CHECK(history.Restore(1, *world.Scene));
    CHECK(glm::distance(world.Scene->Get<Transform>(pawn).Position, vec3(1.0f, 2.0f, 3.0f)) <
          0.0001f);
}

TEST_CASE("prediction history: only replicated components are captured and restored")
{
    World world;
    const Entity pawn = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(pawn, Transform{.Position = vec3(1.0f, 0.0f, 0.0f)});
    // CameraFollow is View-tier, not replicated: it must be neither captured nor touched by restore.
    world.Scene->Add<CameraFollow>(pawn, CameraFollow{.Offset = vec3(0.0f, 5.0f, 10.0f)});

    PredictionHistory history;
    history.Track(pawn);
    history.Record(1, PlayerInput{}, *world.Scene);

    world.Scene->Get<Transform>(pawn).Position = vec3(2.0f, 0.0f, 0.0f);
    world.Scene->Get<CameraFollow>(pawn).Offset = vec3(0.0f, 99.0f, 0.0f);
    CHECK(history.Restore(1, *world.Scene));

    // Transform (replicated) reverts; CameraFollow (not replicated) keeps its mutated value.
    CHECK(world.Scene->Get<Transform>(pawn).Position.x == doctest::Approx(1.0f));
    CHECK(world.Scene->Get<CameraFollow>(pawn).Offset.y == doctest::Approx(99.0f));
}

TEST_CASE("prediction history: the input run after a tick is ascending and exact")
{
    World world;
    const Entity pawn = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(pawn);

    PredictionHistory history;
    history.Track(pawn);
    for (u64 tick = 1; tick <= 5; ++tick)
    {
        history.Record(tick, MoveInput(vec2(static_cast<f32>(tick), 0.0f)), *world.Scene);
    }

    const std::span<const StoredInput> after = history.InputsAfter(2);
    REQUIRE(after.size() == 3);
    CHECK(after[0].Tick == 3);
    CHECK(after[1].Tick == 4);
    CHECK(after[2].Tick == 5);
    CHECK(after[0].Input.GetValue(MoveAction).x == doctest::Approx(3.0f));

    // A tick at or past the newest yields an empty run.
    CHECK(history.InputsAfter(5).empty());
}

TEST_CASE("prediction history: trim drops confirmed ticks and reports the bounds")
{
    World world;
    const Entity pawn = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(pawn);

    PredictionHistory history;
    history.Track(pawn);
    for (u64 tick = 1; tick <= 5; ++tick)
    {
        history.Record(tick, PlayerInput{}, *world.Scene);
    }
    CHECK(history.Size() == 5);
    CHECK(history.OldestTick() == 1);
    CHECK(history.NewestTick() == 5);

    history.TrimThrough(3);
    CHECK(history.Size() == 2);
    CHECK(history.OldestTick() == 4);
    CHECK(history.NewestTick() == 5);
    CHECK_FALSE(history.Contains(3));
    CHECK(history.Contains(4));
    CHECK_FALSE(history.Restore(3, *world.Scene));
    CHECK(history.Restore(4, *world.Scene));
}

TEST_CASE("prediction history: recording past the capacity trims the oldest, never growing")
{
    World world;
    const Entity pawn = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(pawn);

    PredictionHistory history(PredictionHistory::Settings{.Capacity = 4});
    history.Track(pawn);
    for (u64 tick = 1; tick <= 6; ++tick)
    {
        history.Record(tick, PlayerInput{}, *world.Scene);
    }

    CHECK(history.Size() == 4);
    CHECK(history.OldestTick() == 3);
    CHECK(history.NewestTick() == 6);
    CHECK_FALSE(history.Restore(1, *world.Scene));
    CHECK_FALSE(history.Restore(2, *world.Scene));
    CHECK(history.Restore(3, *world.Scene));
}

TEST_CASE("prediction history: re-recording the newest tick overwrites it in place")
{
    World world;
    const Entity pawn = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(pawn, Transform{.Position = vec3(1.0f, 0.0f, 0.0f)});

    PredictionHistory history;
    history.Track(pawn);
    history.Record(1, MoveInput(vec2(1.0f, 0.0f)), *world.Scene);

    // Re-simulate tick 1 with a moved pose and a different input.
    world.Scene->Get<Transform>(pawn).Position = vec3(7.0f, 0.0f, 0.0f);
    history.Record(1, MoveInput(vec2(2.0f, 0.0f)), *world.Scene);
    CHECK(history.Size() == 1);

    world.Scene->Get<Transform>(pawn).Position = vec3(0.0f, 0.0f, 0.0f);
    CHECK(history.Restore(1, *world.Scene));
    CHECK(world.Scene->Get<Transform>(pawn).Position.x == doctest::Approx(7.0f));
    CHECK(history.InputsAfter(0)[0].Input.GetValue(MoveAction).x == doctest::Approx(2.0f));
}

TEST_CASE("prediction history: a component added across the ring restores as absent then present")
{
    World world;
    const Entity pawn = world.Scene->CreateEntity();
    const Entity target = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(pawn);

    PredictionHistory history;
    history.Track(pawn);

    // Tick 1: only Transform. Tick 2: Possesses (replicated) added.
    history.Record(1, PlayerInput{}, *world.Scene);
    world.Scene->Add<Possesses>(pawn, Possesses{.Pawn = target});
    history.Record(2, PlayerInput{}, *world.Scene);

    // Restoring tick 1 undoes the added component; restoring tick 2 brings it back.
    CHECK(history.Restore(1, *world.Scene));
    CHECK_FALSE(world.Scene->Has<Possesses>(pawn));

    CHECK(history.Restore(2, *world.Scene));
    REQUIRE(world.Scene->Has<Possesses>(pawn));
    CHECK(world.Scene->Get<Possesses>(pawn).Pawn == target);
}

TEST_CASE("prediction history: restoring an unrecorded tick fails and the tracked set round-trips")
{
    World world;
    const Entity a = world.Scene->CreateEntity();
    const Entity b = world.Scene->CreateEntity();
    world.Scene->Add<Transform>(a);
    world.Scene->Add<Transform>(b);

    PredictionHistory history;
    history.Track(a);
    history.Track(b);
    history.Track(a); // idempotent
    CHECK(history.Tracked().size() == 2);

    history.Untrack(b);
    CHECK(history.Tracked().size() == 1);
    CHECK(history.Tracked()[0] == a);

    CHECK_FALSE(history.Restore(99, *world.Scene));
    history.Record(1, PlayerInput{}, *world.Scene);
    history.Clear();
    CHECK(history.Size() == 0);
    CHECK_FALSE(history.Restore(1, *world.Scene));
}
