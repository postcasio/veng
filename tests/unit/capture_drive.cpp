// The two bounded decisions the per-frame capture drive makes, both device-free:
//
//  - WorldRunner::DriveCaptureSurfaces walks only the worlds its IsPresented hook accepts. A capture
//    feeds a material sampled by a mesh drawn in some view, so an unpresented world's captures are
//    work nobody can see — and since worlds are flat peers of which several are live at once, driving
//    them all multiplies the frame's fixed view budget by the number of worlds held warm. The runner
//    here is device-free (no Context, no AssetManager), which is exactly what makes the gate testable:
//    reaching a capture surface at all would need a device, so a skipped world is proven skipped by
//    the pass completing and reporting it. The driving half needs a device and rides the gpu band
//    (tests/gpu/capture_surface.cpp).
//  - The capture rotation (Renderer/CaptureRotation.h): the arithmetic ViewportCompositor spends the
//    frame's leftover view budget through — the viewport reservation, and the round-robin that makes a
//    capture set larger than the budget refresh in turn instead of starving its tail.

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include "Renderer/CaptureRotation.h"

#include <set>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A world with one entity carrying an authored CaptureSurface. Nothing materializes its runtime,
    // so no GPU resource exists — the drive gate is what decides whether one would be built.
    WorldInstanceId OpenCaptureWorld(WorldRunner& runner, const CaptureRefresh refresh)
    {
        const WorldInstanceId world = runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
        Scene& scene = runner.ResolveWorld(world)->GetScene();
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        scene.Add<CaptureSurface>(entity).Refresh = refresh;
        return world;
    }
}

TEST_CASE("The capture drive skips a world no view presents, and reports what it walked")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    // Device-free: no AssetManager and no Context, so any attempt to drive a capture surface here
    // would fail its own precondition. Completing the pass is the proof the worlds were skipped.
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    const WorldInstanceId dark = OpenCaptureWorld(runner, CaptureRefresh::EveryFrame);
    const WorldInstanceId alsoDark = OpenCaptureWorld(runner, CaptureRefresh::OnDemand);

    // A world nothing presents is skipped whole: its scene is not walked and no capture is built.
    vector<WorldInstanceId> asked;
    const WorldCaptureDriveResult none = runner.DriveCaptureSurfaces({
        .Register = [](SceneCapture&) { FAIL("an unpresented world registered a capture"); },
        .IsPresented =
            [&asked](const WorldInstanceId world)
        {
            asked.push_back(world);
            return false;
        },
    });

    CHECK(none.WorldsDriven == 0);
    CHECK(none.WorldsSkipped == 2);
    CHECK(none.SurfacesDriven == 0);
    // Presentation is asked once per world, by handle — the runner holds no back-reference and cannot
    // answer for itself.
    REQUIRE(asked.size() == 2);
    CHECK(asked[0] == dark);
    CHECK(asked[1] == alsoDark);

    // Neither surface materialized a capture, so nothing was allocated for a world out of view.
    for (const WorldInstanceId world : {dark, alsoDark})
    {
        const Scene& scene = runner.ResolveWorld(world)->GetScene();
        for (auto [entity, surface] : scene.View<CaptureSurface>())
        {
            CHECK(surface.GetCapture() == nullptr);
        }
    }

    // A presented world is walked rather than skipped — the positive direction of the same gate, on a
    // world carrying no capture surface so the walk needs no device.
    const WorldInstanceId presented = runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
    const WorldCaptureDriveResult one = runner.DriveCaptureSurfaces({
        .Register = [](SceneCapture&) { FAIL("a surface-free world registered a capture"); },
        .IsPresented = [presented](const WorldInstanceId world) { return world == presented; },
    });

    CHECK(one.WorldsDriven == 1);
    CHECK(one.WorldsSkipped == 2);
    CHECK(one.SurfacesDriven == 0);
}

TEST_CASE("The capture rotation reserves the viewports' slots and starves no capture")
{
    // One slot per registered viewport is held back: a capture that cannot claim holds its last map,
    // where a viewport that cannot shows a stale window.
    CHECK(CaptureDriveHasRoom(32, 1));
    CHECK(CaptureDriveHasRoom(2, 1));
    CHECK_FALSE(CaptureDriveHasRoom(1, 1));
    CHECK_FALSE(CaptureDriveHasRoom(0, 1));
    // Four presented viewports reserve four, whatever is left over is the captures'.
    CHECK(CaptureDriveHasRoom(5, 4));
    CHECK_FALSE(CaptureDriveHasRoom(4, 4));
    // An empty drive-list has no budget question to answer.
    CHECK_FALSE(CaptureDriveHasRoom(0, 0));

    SUBCASE("A frame that affords the whole list drives it in list order and holds the cursor")
    {
        CHECK(CaptureDriveIndex(0, 0, 4) == 0);
        CHECK(CaptureDriveIndex(0, 3, 4) == 3);
        CHECK(NextCaptureCursor(0, 4, 4) == 0);
        // Order is immaterial when nothing was dropped, so a cursor left mid-list stays put.
        CHECK(NextCaptureCursor(2, 4, 4) == 2);
    }

    SUBCASE("A budget-limited frame resumes at the first capture it could not afford")
    {
        // Three of five driven from cursor 0: the next frame starts on index 3.
        CHECK(NextCaptureCursor(0, 3, 5) == 3);
        // And wraps rather than running off the end.
        CHECK(CaptureDriveIndex(3, 0, 5) == 3);
        CHECK(CaptureDriveIndex(3, 2, 5) == 0);
        CHECK(NextCaptureCursor(3, 3, 5) == 1);
    }

    SUBCASE("Every capture in an over-budget list is reached within ceil(count / budget) frames")
    {
        // The property the rotation exists for: with 9 captures and room for 4 a frame, a fixed
        // prefix would leave the last five permanently black. Walk three frames and collect what was
        // driven — every index must appear.
        constexpr usize Count = 9;
        constexpr usize Budget = 4;
        std::set<usize> driven;
        usize cursor = 0;
        for (int frame = 0; frame < 3; ++frame)
        {
            for (usize step = 0; step < Budget; ++step)
            {
                driven.insert(CaptureDriveIndex(cursor, step, Count));
            }
            cursor = NextCaptureCursor(cursor, Budget, Count);
        }
        CHECK(driven.size() == Count);
    }

    SUBCASE("An empty drive-list is inert")
    {
        CHECK(CaptureDriveIndex(0, 0, 0) == 0);
        CHECK(NextCaptureCursor(0, 0, 0) == 0);
    }
}
