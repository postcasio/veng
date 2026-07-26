// The presentation gate on the per-frame capture drive, device-free:
//
//  - WorldRunner::DriveCaptureSurfaces walks only the worlds its IsPresented hook accepts. A capture
//    feeds a material sampled by a mesh drawn in some view, so an unpresented world's captures are
//    work nobody can see — and since worlds are flat peers of which several are live at once, driving
//    them all multiplies the frame's fixed view budget by the number of worlds held warm. The runner
//    here is device-free (no Context, no AssetManager), which is exactly what makes the gate testable:
//    reaching a capture surface at all would need a device, so a skipped world is proven skipped by
//    the pass completing and reporting it. The driving half needs a device and rides the gpu band
//    (tests/gpu/capture_surface.cpp).

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

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
