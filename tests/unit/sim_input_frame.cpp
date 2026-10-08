// SimInputFrame: the per-frame input protocol every simulation stepped in one frame shares — a
// WorldRunner's worlds and a driver stepping a SimClock of its own, as a tool's play session does.
// The rig drives frames in the application's order: open the frame, land its input, scope the
// pointer, tick the runner, then step the driver. Input is headless and fed fabricated events, and
// the runner is device-free, so the whole protocol runs with no window or GPU.

#include <doctest/doctest.h>

#include <numeric>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Input/SimInputFrame.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SimClock.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>
#include "support/TestServices.h"

using namespace Veng;

namespace
{
    // The 60 Hz tick, and half of it: a 120 Hz display's frame, which runs a step every other frame.
    constexpr f32 Tick60 = 1.0f / 60.0f;
    constexpr f32 Half60 = Tick60 * 0.5f;

    // The position BeginFrame's routing carries, marking a scene the pointer is scoped to.
    constexpr vec2 RoutedMarker{1.0f, 2.0f};

    // What one Sim step read: the pointer's horizontal look, as a seat reads it only while its scene
    // holds the pointer, and pad 0's horizontal swipe.
    struct StepRead
    {
        f32 Look = 0.0f;
        f32 Swipe = 0.0f;
    };

    f32 SumLook(const vector<StepRead>& reads)
    {
        return std::accumulate(reads.begin(), reads.end(), 0.0f,
                               [](const f32 sum, const StepRead& read) { return sum + read.Look; });
    }

    f32 SumSwipe(const vector<StepRead>& reads)
    {
        return std::accumulate(reads.begin(), reads.end(), 0.0f,
                               [](const f32 sum, const StepRead& read)
                               { return sum + read.Swipe; });
    }

    struct Rig
    {
        TypeRegistry Types;
        SystemRegistry Systems;
        Input Snapshot{nullptr};
        SimInputFrame Frame;
        TestSupport::TestServices Services{TestSupport::TestServicesInfo{.Input = &Snapshot}};
        WorldRunner Runner{WorldRunnerInfo{.Types = &Types, .Systems = &Systems}};
        WorldInstanceId World;
        Unique<Scene> DriverScene = Scene::Create(Types);
        SimClock DriverClock{SimClockInfo{.TickRate = 60}};
        vector<StepRead> WorldReads;
        vector<StepRead> DriverReads;
        f32 PointerX = 0.0f;
        f32 FingerX = 0.0f;

        // With @p world, a runner world running no systems steps beside the driver.
        explicit Rig(const bool world)
        {
            Runner.SetContextFactory(Services.Factory());
            if (world)
            {
                World = Runner.OpenWorld(WorldOpenInfo{
                    .SimTickRate = 60,
                    .StartSimulation = true,
                    .Systems = vector<SystemId>{},
                });
            }

            // The first move only seeds the position, and a finger's first step reads no motion, so
            // both are put in place on a stepping frame no case measures.
            Frame.BeginFrame(Snapshot);
            Snapshot.ApplyEvent(MouseMovedEvent(vec2{0.0f, 0.0f}));
            Ingest();
            StepDriver(Tick60);
            DriverReads.clear();
        }

        [[nodiscard]] Scene& WorldScene() { return Runner.ResolveWorld(World)->GetScene(); }

        [[nodiscard]] StepRead Read(const Scene& scene) const
        {
            const bool ownsPointer = Frame.GetPointer(scene).LocalPosition == RoutedMarker;
            return {.Look = ownsPointer ? Snapshot.GetSimMouseDelta().x : 0.0f,
                    .Swipe = Snapshot.GetSimGamepadAxis(GamepadId{0}, GamepadAxis::TouchpadDeltaX)};
        }

        // Pad 0 with a finger resting at FingerX, as the backend polls it once per frame.
        void Ingest()
        {
            std::array<GamepadState, Input::MaxGamepads> states{};
            states[0].Connected = true;
            states[0].Buttons[static_cast<usize>(GamepadButton::TouchpadTouch)] = true;
            states[0].Axes[static_cast<usize>(GamepadAxis::TouchpadX)] = FingerX;
            Snapshot.IngestGamepadStates(states);
        }

        // Opens a frame whose input moves the pointer by @p look and the finger by @p swipe, and
        // scopes the pointer to @p routed (no scene when null), as the application does before the
        // worlds tick.
        void BeginFrame(const f32 look, const f32 swipe, const Scene* routed)
        {
            Frame.BeginFrame(Snapshot);
            PointerX += look;
            Snapshot.ApplyEvent(MouseMovedEvent(vec2{PointerX, 0.0f}));
            FingerX += swipe;
            Ingest();
            Frame.SetPointer(PointerRouting{.LocalPosition = RoutedMarker}, routed);
        }

        u64 TickWorld(const f32 delta)
        {
            const WorldTickResult result = Runner.Tick(WorldTickInfo{
                .Delta = delta,
                .BeforeSimStep =
                    [this](WorldInstanceId, const Scene& scene, u64)
                {
                    Frame.BeginSimStep(Snapshot, scene);
                    WorldReads.push_back(Read(scene));
                },
            });
            if (result.AnyActive)
            {
                Frame.Report(result.AnyTicked);
            }
            return result.AnyTicked ? 1 : 0;
        }

        u64 StepDriver(const f32 delta)
        {
            const SimStep step = DriverClock.Run(
                delta,
                [this](const SimStepInfo&)
                {
                    Frame.BeginSimStep(Snapshot, *DriverScene);
                    DriverReads.push_back(Read(*DriverScene));
                    return true;
                },
                [] { return 0.0; });
            Frame.Report(step.Steps > 0);
            return step.Steps;
        }
    };
}

TEST_CASE("A self-clocked driver's step reads the pointer and touchpad motion since its last step")
{
    Rig rig(/*world=*/false);

    rig.BeginFrame(3.0f, 0.1f, rig.DriverScene.get());
    REQUIRE(rig.StepDriver(Half60) == 0);
    rig.BeginFrame(4.0f, 0.15f, rig.DriverScene.get());
    REQUIRE(rig.StepDriver(Half60) == 1);

    REQUIRE(rig.DriverReads.size() == 1);
    CHECK(rig.DriverReads[0].Look == doctest::Approx(7.0f));
    CHECK(rig.DriverReads[0].Swipe == doctest::Approx(0.25f));
}

TEST_CASE("A driver's multi-step frame reads its motion on the first step, and none twice")
{
    Rig rig(/*world=*/false);

    // A quarter tick is left over, so the next half-tick frame runs no step and the one after does.
    rig.BeginFrame(9.0f, 0.3f, rig.DriverScene.get());
    REQUIRE(rig.StepDriver(Tick60 * 3.25f) == 3);
    rig.BeginFrame(2.0f, 0.1f, rig.DriverScene.get());
    REQUIRE(rig.StepDriver(Half60) == 0);
    rig.BeginFrame(1.0f, 0.05f, rig.DriverScene.get());
    REQUIRE(rig.StepDriver(Half60) == 1);

    REQUIRE(rig.DriverReads.size() == 4);
    CHECK(rig.DriverReads[0].Look == doctest::Approx(9.0f));
    CHECK(rig.DriverReads[3].Look == doctest::Approx(3.0f));
    CHECK(rig.DriverReads[3].Swipe == doctest::Approx(0.15f));
    CHECK(SumLook(rig.DriverReads) == doctest::Approx(12.0f));
    CHECK(SumSwipe(rig.DriverReads) == doctest::Approx(0.45f));
}

TEST_CASE("Beside a runner world, only the routed scene's steps take the pointer's motion")
{
    Rig rig(/*world=*/true);
    const Scene& world = rig.WorldScene();
    const Scene& driver = *rig.DriverScene;

    // The runner's world steps first in every frame, the driver after it.
    const auto frame = [&](const f32 look, const Scene* routed)
    {
        rig.BeginFrame(look, 0.0f, routed);
        REQUIRE(rig.TickWorld(Tick60) == 1);
        REQUIRE(rig.StepDriver(Tick60) == 1);
    };
    frame(5.0f, &driver);
    CHECK(rig.Frame.GetPointer(world).LocalPosition == vec2{0.0f, 0.0f});
    frame(6.0f, &world);
    frame(2.0f, nullptr);
    frame(1.0f, &driver);

    // Each frame's motion reaches its routed scene once; the unrouted frame's is drained, not banked.
    REQUIRE(rig.DriverReads.size() == 4);
    REQUIRE(rig.WorldReads.size() == 4);
    CHECK(rig.DriverReads[0].Look == doctest::Approx(5.0f));
    CHECK(rig.WorldReads[1].Look == doctest::Approx(6.0f));
    CHECK(rig.DriverReads[3].Look == doctest::Approx(1.0f));
    CHECK(SumLook(rig.WorldReads) + SumLook(rig.DriverReads) == doctest::Approx(12.0f));
}

TEST_CASE("Beside a runner world, the touchpad's motion is read once, by the frame's first step")
{
    Rig rig(/*world=*/true);
    const Scene& driver = *rig.DriverScene;

    rig.BeginFrame(0.0f, 0.2f, &driver);
    REQUIRE(rig.TickWorld(Tick60) == 1);
    REQUIRE(rig.StepDriver(Tick60) == 1);
    rig.BeginFrame(0.0f, 0.3f, &driver);
    REQUIRE(rig.TickWorld(Tick60) == 1);
    REQUIRE(rig.StepDriver(Tick60) == 1);

    // A pad belongs to no scene, so the world, stepping first, takes each frame's swipe.
    CHECK(SumSwipe(rig.WorldReads) == doctest::Approx(0.5f));
    CHECK(SumSwipe(rig.DriverReads) == 0.0f);
}

TEST_CASE("A driver stepping after a paused runner world keeps the frame's motion")
{
    Rig rig(/*world=*/true);
    rig.Runner.SetWorldPaused(rig.World, true);
    const Scene& driver = *rig.DriverScene;

    rig.BeginFrame(4.0f, 0.1f, &driver);
    REQUIRE(rig.TickWorld(Half60) == 0);
    REQUIRE(rig.StepDriver(Half60) == 0);
    rig.BeginFrame(3.0f, 0.1f, &driver);
    REQUIRE(rig.TickWorld(Half60) == 0);
    REQUIRE(rig.StepDriver(Half60) == 1);

    REQUIRE(rig.DriverReads.size() == 1);
    CHECK(rig.DriverReads[0].Look == doctest::Approx(7.0f));
    CHECK(rig.DriverReads[0].Swipe == doctest::Approx(0.2f));
}

TEST_CASE("Motion made on a frame nothing simulates never reaches a later step")
{
    Rig rig(/*world=*/false);
    const Scene& driver = *rig.DriverScene;

    // The driver sits a frame out, as a paused play session does, then resumes.
    rig.BeginFrame(5.0f, 0.2f, &driver);
    rig.BeginFrame(1.0f, 0.0f, &driver);
    REQUIRE(rig.StepDriver(Tick60) == 1);

    REQUIRE(rig.DriverReads.size() == 1);
    CHECK(rig.DriverReads[0].Look == doctest::Approx(1.0f));
    CHECK(rig.DriverReads[0].Swipe == 0.0f);
}

TEST_CASE("The edges hold after a frame that simulated without a step, else roll")
{
    Input input(nullptr);
    SimInputFrame frame;

    frame.BeginFrame(input);
    input.ApplyEvent(KeyPressedEvent(Key::A, 0, 0));
    frame.Report(false);
    frame.BeginFrame(input);
    CHECK(input.WasKeyPressed(Key::A));
    frame.BeginFrame(input);
    CHECK_FALSE(input.WasKeyPressed(Key::A));

    // Any simulation stepping consumes the held edges for every one of them.
    input.ApplyEvent(KeyPressedEvent(Key::B, 0, 0));
    frame.Report(false);
    frame.Report(true);
    frame.BeginFrame(input);
    CHECK_FALSE(input.WasKeyPressed(Key::B));
}

TEST_CASE("A tap within a self-clocked driver's zero-step frame reaches its next step")
{
    Input input(nullptr);
    SimInputFrame frame;
    SimClock clock(SimClockInfo{.TickRate = 60});
    int stepsReadingDown = 0;
    const auto run = [&](const f32 delta, const bool tap)
    {
        frame.BeginFrame(input);
        if (tap)
        {
            input.ApplyEvent(KeyPressedEvent(Key::Space, 0, 0));
            input.ApplyEvent(KeyReleasedEvent(Key::Space, 0, 0));
        }
        const SimStep step = clock.Run(
            delta,
            [&](const SimStepInfo&)
            {
                stepsReadingDown += input.IsKeyDown(Key::Space) ? 1 : 0;
                return true;
            },
            [] { return 0.0; });
        frame.Report(step.Steps > 0);
        return step.Steps;
    };

    REQUIRE(run(Half60, /*tap=*/true) == 0);
    REQUIRE(run(Half60, /*tap=*/false) == 1);
    CHECK(stepsReadingDown == 1);

    // The step consumed the tap, so the next roll applies its deferred release.
    REQUIRE(run(Tick60, /*tap=*/false) == 1);
    CHECK(stepsReadingDown == 1);
}
