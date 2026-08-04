// ManagedViewportSet: the engine's managed-viewport policy, bound to worlds by handle.
//
// Drives a real headless Application (its own Context, no window, no ImGui) through Run(), plus a
// standalone-constructed set for the teardown case. It pins the plan's world↔viewport contract:
//
//  - two managed viewports naming two different worlds each present their own world's scene through
//    their own seat's camera (distinct scenes, distinct resolved cameras) — the multi-world pull;
//  - a split-screen reconfigure to N quadrant Layouts over one world routes and renders as before
//    (four viewports, quadrant regions, all presenting the one world's scene);
//  - a viewport whose bound world is closed at runtime renders a cleared target (its presented scene
//    goes null, inert) with no dangling read or crash;
//  - tearing down a set with viewports still registered self-unregisters each from the compositor
//    and retires its id against the live Context registry (the teardown-order invariant).
//
// It needs a Context for the Application/viewports, so it rides the gpu band though it pins no pixels.

#include <doctest/doctest.h>

#include <array>
#include <optional>

#include <Veng/Application.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/ManagedViewports.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // A headless application driven by two closures: InitFn (from OnInitialize, engine ready) and
    // StepFn (each OnUpdate, with the frame index). Worlds opened here live on the runner; viewports
    // are the engine's managed set.
    class MvApp final : public Application
    {
    public:
        using Application::Application;

        function<void(MvApp&)> InitFn;
        function<void(MvApp&, int)> StepFn;
        int Frames = 4;
        int Current = 0;

        // Opens an empty world holding a camera at `eye` and a seat viewing through it; returns both.
        struct WorldSeat
        {
            WorldInstanceId World;
            Entity Seat;
            const Scene* Scene = nullptr;
        };
        WorldSeat OpenCameraWorld(vec3 eye)
        {
            const WorldInstanceId world =
                GetWorldRunner().OpenWorld(WorldOpenInfo{.StartSimulation = false});
            Scene& scene = GetWorldRunner().ResolveWorld(world)->GetScene();
            const Entity camera = scene.CreateEntity();
            scene.Add<Transform>(camera).Position = eye;
            scene.Add<Camera>(camera);
            const Entity seat = scene.CreateEntity();
            scene.Add<Viewer>(seat).Camera = camera;
            return {.World = world, .Seat = seat, .Scene = &scene};
        }

        // Like OpenCameraWorld, but with a started simulation so the world ticks each frame and reaches
        // presentability (resolves, started, resident, ticked) — the destination a present-on-ready
        // rebind waits on and then applies.
        WorldSeat OpenReadyCameraWorld(vec3 eye)
        {
            const WorldInstanceId world = GetWorldRunner().OpenWorld(WorldOpenInfo{
                .SimTickRate = 60,
                .StartSimulation = true,
                .Systems = vector<SystemId>{},
                .MakeStartContext =
                    [this]
                {
                    return SystemContext{.Assets = GetAssetManager(),
                                         .Input = GetInput(),
                                         .Tasks = GetTaskSystem(),
                                         .Audio = GetAudioEngine(),
                                         .Role = GetNetRole()};
                },
            });
            Scene& scene = GetWorldRunner().ResolveWorld(world)->GetScene();
            const Entity camera = scene.CreateEntity();
            scene.Add<Transform>(camera).Position = eye;
            scene.Add<Camera>(camera);
            const Entity seat = scene.CreateEntity();
            scene.Add<Viewer>(seat).Camera = camera;
            return {.World = world, .Seat = seat, .Scene = &scene};
        }

    protected:
        void OnInitialize() override
        {
            if (InitFn)
            {
                InitFn(*this);
            }
        }

        void OnUpdate(f32) override
        {
            if (StepFn)
            {
                StepFn(*this, Current);
            }
            if (++Current >= Frames)
            {
                RequestExit();
            }
        }
    };

    ApplicationInfo HeadlessInfo(vector<ManagedViewportInfo> managed)
    {
        ApplicationInfo info;
        info.Name = "veng-managed-viewport-set-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        info.HeadlessExtent = {128, 96};
        info.ManagedViewports = std::move(managed);
        return info;
    }

    bool CamerasDiffer(const CameraView& a, const CameraView& b)
    {
        const mat4 va = a.ViewProjection();
        const mat4 vb = b.ViewProjection();
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (va[c][r] != vb[c][r])
                {
                    return true;
                }
            }
        }
        return false;
    }
}

TEST_CASE(
    "Two managed viewports naming two worlds present each world through its own seat's camera")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    // Two default managed viewports at startup; rebound to the two worlds once they exist.
    MvApp app(HeadlessInfo({ManagedViewportInfo{}, ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat a{};
    MvApp::WorldSeat b{};

    app.InitFn = [&](MvApp& app)
    {
        a = app.OpenCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        b = app.OpenCameraWorld(vec3(20.0f, 3.0f, 5.0f));

        // Reconfigure the set so viewport 0 presents world A through seat A, viewport 1 world B
        // through seat B — each names both its world and its seat.
        const ManagedViewportInfo infos[] = {
            ManagedViewportInfo{.World = a.World, .Viewer = a.Seat},
            ManagedViewportInfo{.World = b.World, .Viewer = b.Seat},
        };
        app.ReconfigureManagedViewports(infos);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        if (frame == 2)
        {
            const ManagedViewportSet& set = app.GetManagedViewports();
            REQUIRE(set.GetCount() == 2);
            const Renderer::Viewport* v0 = set.Get(0);
            const Renderer::Viewport* v1 = set.Get(1);
            REQUIRE(v0 != nullptr);
            REQUIRE(v1 != nullptr);

            // Each viewport presents its own world's scene, resolved by the per-frame pull.
            CHECK(v0->GetPresentedScene() == a.Scene);
            CHECK(v1->GetPresentedScene() == b.Scene);
            CHECK(v0->GetPresentedScene() != v1->GetPresentedScene());

            // Each viewport renders through its own seat's camera — distinct views (distinct outputs).
            CHECK(CamerasDiffer(v0->GetPresentedCamera(), v1->GetPresentedCamera()));

            // Both produced live outputs.
            CHECK(v0->GetOutputHandle().IsValid());
            CHECK(v1->GetOutputHandle().IsValid());
        }
    };

    app.Frames = 4;
    app.Run({});
}

TEST_CASE("A split-screen reconfigure to N quadrants over one world routes and renders as before")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat world{};

    app.InitFn = [&](MvApp& app)
    {
        world = app.OpenCameraWorld(vec3(0.0f, 0.0f, 5.0f));

        // Four quadrant Layouts, all over the one world (scene-primary camera, no bound Viewer).
        const ManagedViewportInfo quadrants[] = {
            ManagedViewportInfo{.Layout = {.Offset = {0.0f, 0.0f}, .Extent = {0.5f, 0.5f}},
                                .World = world.World},
            ManagedViewportInfo{.Layout = {.Offset = {0.5f, 0.0f}, .Extent = {0.5f, 0.5f}},
                                .World = world.World},
            ManagedViewportInfo{.Layout = {.Offset = {0.0f, 0.5f}, .Extent = {0.5f, 0.5f}},
                                .World = world.World},
            ManagedViewportInfo{.Layout = {.Offset = {0.5f, 0.5f}, .Extent = {0.5f, 0.5f}},
                                .World = world.World},
        };
        app.ReconfigureManagedViewports(quadrants);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        if (frame == 2)
        {
            const ManagedViewportSet& set = app.GetManagedViewports();
            REQUIRE(set.GetCount() == 4);

            // The render extent (HeadlessExtent) is the quadrant basis: 128×96 → 64×48 quadrants.
            const std::array<Renderer::ViewportRegion, 4> expected = {{
                {.Offset = {0, 0}, .Extent = {64, 48}},
                {.Offset = {64, 0}, .Extent = {64, 48}},
                {.Offset = {0, 48}, .Extent = {64, 48}},
                {.Offset = {64, 48}, .Extent = {64, 48}},
            }};
            for (usize i = 0; i < 4; ++i)
            {
                const Renderer::Viewport* v = set.Get(i);
                REQUIRE(v != nullptr);
                CHECK(v->GetRegion().Offset == expected[i].Offset);
                CHECK(v->GetRegion().Extent == expected[i].Extent);
                // Every quadrant presents the one world's scene and produced a live output.
                CHECK(v->GetPresentedScene() == world.Scene);
                CHECK(v->GetOutputHandle().IsValid());
            }
        }
    };

    app.Frames = 4;
    app.Run({});
}

TEST_CASE("A viewport whose bound world is closed renders a cleared target without crashing")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat world{};

    app.InitFn = [&](MvApp& app)
    {
        world = app.OpenCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, world.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        const Renderer::Viewport* v = app.GetManagedViewports().Get(0);
        REQUIRE(v != nullptr);

        if (frame == 1)
        {
            // The pull presented the bound world's scene while it was open.
            CHECK(v->GetPresentedScene() == world.Scene);
        }
        else if (frame == 2)
        {
            // Close the world: the id now resolves to nothing.
            app.GetWorldRunner().CloseWorld(world.World);
        }
        else if (frame == 3)
        {
            // The next pull resolved no world, so the viewport dropped its retained scene pointer and
            // renders a cleared target — inert, never a dangling read into the destroyed scene.
            CHECK(v->GetPresentedScene() == nullptr);
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("A runtime rebind re-points a managed viewport from one world to another, leaving the "
          "first untouched")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat a{};
    MvApp::WorldSeat b{};

    app.InitFn = [&](MvApp& app)
    {
        a = app.OpenCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        b = app.OpenCameraWorld(vec3(20.0f, 3.0f, 5.0f));
        // Bind viewport 0 to world A at startup (the immediate bootstrap setter).
        app.GetManagedViewports().SetViewportWorld(0, a.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        const Renderer::Viewport* v = app.GetManagedViewports().Get(0);
        REQUIRE(v != nullptr);

        if (frame == 1)
        {
            // The pull presented world A.
            CHECK(v->GetPresentedScene() == a.Scene);
            // Request a runtime rebind to world B — deferred, so it applies at the next frame's top, not
            // mid-drive: this frame's push still presents A.
            app.RebindManagedViewport(0, b.World);
            CHECK(v->GetPresentedScene() == a.Scene);
        }
        else if (frame == 3)
        {
            // The rebind applied at frame 2's top and frame 2's push presented B: the viewport now shows
            // world B, and A is untouched — still a live, open world (the rebind re-pointed, not closed).
            CHECK(v->GetPresentedScene() == b.Scene);
            CHECK(app.GetWorldRunner().ResolveWorld(a.World) != nullptr);
            CHECK(v->GetOutputHandle().IsValid());
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("A present-on-ready rebind holds the old world until the destination readies, then swaps "
          "once")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat oldWorld{};
    MvApp::WorldSeat destination{};

    app.InitFn = [&](MvApp& app)
    {
        oldWorld = app.OpenReadyCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        destination = app.OpenReadyCameraWorld(vec3(20.0f, 3.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, oldWorld.World);
        // Request the swap to the destination only once it is ready. At this point the destination has
        // just opened and not yet ticked, so it is not presentable.
        app.RebindManagedViewportWhenReady(0, destination.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        const Renderer::Viewport* v = app.GetManagedViewports().Get(0);
        REQUIRE(v != nullptr);

        if (frame == 0)
        {
            // The destination had not ticked at this frame's top, so the swap is still pending: the
            // viewport's applied binding is still the old world, and the query reports the destination
            // in flight. (GetPresentedScene reflects the prior frame's push, so it is checked from
            // frame 1 on, once a push has run.)
            CHECK(app.GetManagedViewportWorld(0) == oldWorld.World);
            REQUIRE(app.GetPendingManagedViewportWorld(0).has_value());
            CHECK(*app.GetPendingManagedViewportWorld(0) == destination.World);
        }
        else if (frame == 1)
        {
            // Frame 0's push presented the old world (the rebind had not applied yet).
            CHECK(v->GetPresentedScene() == oldWorld.Scene);
        }
        else if (frame == 3)
        {
            // The destination ticked and became presentable, so the rebind applied: the viewport now
            // presents the destination, the pending query clears, and the old world stays open.
            CHECK(app.GetManagedViewportWorld(0) == destination.World);
            CHECK_FALSE(app.GetPendingManagedViewportWorld(0).has_value());
            CHECK(v->GetPresentedScene() == destination.Scene);
            CHECK(app.GetWorldRunner().ResolveWorld(oldWorld.World) != nullptr);
            CHECK(app.GetAbandonedManagedPresentWorld(0) == WorldInstanceId{});
        }
    };

    app.Frames = 6;
    app.Run({});
}

TEST_CASE(
    "A present-on-ready rebind is superseded by a later rebind, and dropped if its destination "
    "closes")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat base{};
    MvApp::WorldSeat neverReady{};
    MvApp::WorldSeat replacement{};

    app.InitFn = [&](MvApp& app)
    {
        base = app.OpenReadyCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        // A world whose simulation never starts never reaches presentability, so a present-on-ready
        // rebind onto it waits indefinitely — until superseded or dropped.
        neverReady = app.OpenCameraWorld(vec3(20.0f, 3.0f, 5.0f));
        replacement = app.OpenReadyCameraWorld(vec3(-10.0f, 1.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, base.World);
        app.RebindManagedViewportWhenReady(0, neverReady.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        const Renderer::Viewport* v = app.GetManagedViewports().Get(0);
        REQUIRE(v != nullptr);

        if (frame == 0)
        {
            // The conditional rebind is in flight and holds the base world (the destination never
            // readies).
            REQUIRE(app.GetPendingManagedViewportWorld(0).has_value());
            CHECK(*app.GetPendingManagedViewportWorld(0) == neverReady.World);
            CHECK(app.GetManagedViewportWorld(0) == base.World);
            // A later unconditional rebind supersedes the conditional one (last wins): the viewport goes
            // to the replacement, not the never-ready destination.
            app.RebindManagedViewport(0, replacement.World);
        }
        else if (frame == 2)
        {
            // The unconditional rebind applied; the superseded conditional never did.
            CHECK(app.GetManagedViewportWorld(0) == replacement.World);
            CHECK_FALSE(app.GetPendingManagedViewportWorld(0).has_value());
            CHECK(v->GetPresentedScene() == replacement.Scene);
            // Now request a present-on-ready to the never-ready world again, then close it mid-wait.
            app.RebindManagedViewportWhenReady(0, neverReady.World);
        }
        else if (frame == 3)
        {
            REQUIRE(app.GetPendingManagedViewportWorld(0).has_value());
            CHECK(*app.GetPendingManagedViewportWorld(0) == neverReady.World);
            app.GetWorldRunner().CloseWorld(neverReady.World);
        }
        else if (frame == 5)
        {
            // The destination closed mid-wait, so the conditional rebind dropped: no pending, and the
            // viewport still presents the replacement (unchanged).
            CHECK_FALSE(app.GetPendingManagedViewportWorld(0).has_value());
            CHECK(app.GetManagedViewportWorld(0) == replacement.World);
        }
    };

    app.Frames = 7;
    app.Run({});
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ManagedViewportSet teardown self-unregisters each viewport and retires its id "
                  "against the live Context registry")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    Input input(nullptr);
    InputRouter router(nullptr, input, Context.GetViewportRegistry());
    Renderer::ViewportCompositor compositor(Context);
    const Renderer::ViewportRegistry& registry = Context.GetViewportRegistry();

    Renderer::ViewportId firstId;
    Renderer::ViewportId secondId;

    // The set is declared after the compositor/router/Context, so it destructs first (reverse scope
    // order) — the Application teardown order (managed viewports < compositor/router < Context).
    {
        ManagedViewportSet set(Context, assets, compositor, router);
        const ManagedViewportInfo infos[] = {ManagedViewportInfo{}, ManagedViewportInfo{}};
        set.Build(infos);

        REQUIRE(set.GetCount() == 2);
        REQUIRE(compositor.GetViewports().size() == 2);
        firstId = set.Get(0)->GetId();
        secondId = set.Get(1)->GetId();
        CHECK(registry.Resolve(firstId) == set.Get(0));
        CHECK(registry.Resolve(secondId) == set.Get(1));
    }

    // The set destructed with its viewports still registered: each self-unregistered from the live
    // compositor drive-list and retired its id against the live Context registry — no retire touched
    // a destroyed registry.
    CHECK(compositor.GetViewports().empty());
    CHECK(registry.Resolve(firstId) == nullptr);
    CHECK(registry.Resolve(secondId) == nullptr);
}
