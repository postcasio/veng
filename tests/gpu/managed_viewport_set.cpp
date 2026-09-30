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
//  - a completed rebind and an abandoned present-on-ready each reach their Application hook once,
//    the first carrying the seat the viewport ended the frame bound to;
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
#include <Veng/WorldDirectory.h>
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

#include "support/BootstrapFixture.h"

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
                                         .Localization = GetLocalization(),
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

        // A world that reaches presentability but seats no Viewer, so a rebind onto it resolves no
        // seat and the unbound-seat pass finds none either.
        WorldInstanceId OpenReadySeatlessWorld()
        {
            return GetWorldRunner().OpenWorld(WorldOpenInfo{
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
                                         .Localization = GetLocalization(),
                                         .Role = GetNetRole()};
                },
            });
        }

        // One OnWorldPresented call, plus the seat the set reported for that viewport at the moment
        // the hook ran — the evidence that the association is in place before the consumer is told.
        struct PresentedCall
        {
            usize Index = 0;
            WorldInstanceId World;
            Entity Seat = Entity::Null;
            Entity BoundSeatAtCall = Entity::Null;
            WorldInstanceId BindingAtCall;
        };

        // One OnWorldDeparted call, plus what held at the moment the hook ran: the world's tick, and
        // the presentation pins the directory still counted on it (zero with no directory).
        struct DepartedCall
        {
            WorldInstanceId World;
            u64 TickAtCall = 0;
            u32 PresenceAtCall = 0;
        };

        vector<PresentedCall> Presented;
        vector<std::pair<usize, WorldInstanceId>> Abandoned;
        vector<DepartedCall> Departed;
        // Every presented ('P') and departed ('D') call in the order the hooks ran.
        vector<std::pair<char, WorldInstanceId>> Hooks;

    protected:
        void OnWorldPresented(usize index, WorldInstanceId world, Entity seat) override
        {
            Hooks.emplace_back('P', world);
            Presented.push_back({
                .Index = index,
                .World = world,
                .Seat = seat,
                .BoundSeatAtCall = GetManagedViewports().GetViewportViewer(index),
                .BindingAtCall = GetManagedViewportWorld(index),
            });
        }

        void OnWorldPresentAbandoned(usize index, WorldInstanceId destination) override
        {
            Abandoned.emplace_back(index, destination);
        }

        void OnWorldDeparted(World& world) override
        {
            Hooks.emplace_back('D', world.Id);
            const WorldDirectory* const directory = GetWorldDirectory();
            Departed.push_back({
                .World = world.Id,
                .TickAtCall = world.Clock.GetTick(),
                .PresenceAtCall = directory != nullptr ? directory->PresenceOf(world.Id) : 0,
            });
        }

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

TEST_CASE("A present-on-ready rebind superseded is not abandoned, but one whose destination is "
          "reaped mid-wait is — every non-completion observable")
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
            // Negative control: a deliberate supersession is an intentional replacement, not a
            // failure — it records no abandonment.
            CHECK(app.GetAbandonedManagedPresentWorld(0) == WorldInstanceId{});
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
            // The destination was reaped mid-wait, so the conditional rebind was abandoned: no pending,
            // the viewport still presents the replacement (unchanged), and the vanished destination is
            // recorded — a reaped present-on-ready failure is observable, not silent.
            CHECK_FALSE(app.GetPendingManagedViewportWorld(0).has_value());
            CHECK(app.GetManagedViewportWorld(0) == replacement.World);
            CHECK(app.GetAbandonedManagedPresentWorld(0) == neverReady.World);
        }
    };

    app.Frames = 7;
    app.Run({});
}

TEST_CASE("A completed rebind is reported once, with the seat the viewport ended up adopting")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat base{};
    MvApp::WorldSeat seated{};
    WorldInstanceId seatless;

    app.InitFn = [&](MvApp& app)
    {
        base = app.OpenReadyCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        seated = app.OpenReadyCameraWorld(vec3(20.0f, 3.0f, 5.0f));
        seatless = app.OpenReadySeatlessWorld();
        // The bootstrap setter, not a rebind: it completes nothing and reports nothing.
        app.GetManagedViewports().SetViewportWorld(0, base.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        if (frame == 0)
        {
            CHECK(app.Presented.empty());
            app.RebindManagedViewportWhenReady(0, seated.World);
        }
        else if (frame == 3)
        {
            // The present-on-ready rebind landed, and was reported exactly once — with the seat it
            // adopted, which the set was already bound to when the hook ran.
            REQUIRE(app.Presented.size() == 1);
            CHECK(app.Presented[0].Index == 0);
            CHECK(app.Presented[0].World == seated.World);
            CHECK(app.Presented[0].Seat == seated.Seat);
            CHECK(app.Presented[0].BoundSeatAtCall == seated.Seat);
            CHECK(app.Presented[0].BindingAtCall == seated.World);
            CHECK(app.Abandoned.empty());

            // A destination seating no Viewer resolves none, and the unbound-seat pass finds none
            // either — so the reported seat is null rather than the departed world's stale handle.
            app.RebindManagedViewport(0, seatless);
        }
        else if (frame == 5)
        {
            REQUIRE(app.Presented.size() == 2);
            CHECK(app.Presented[1].World == seatless);
            CHECK(app.Presented[1].Seat == Entity::Null);
            CHECK(app.Presented[1].BoundSeatAtCall == Entity::Null);
        }
    };

    app.Frames = 7;
    app.Run({});
}

TEST_CASE("An abandoned present-on-ready is reported once, and a later rebind does not repeat it")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat base{};
    MvApp::WorldSeat neverReady{};

    app.InitFn = [&](MvApp& app)
    {
        base = app.OpenReadyCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        // A world whose simulation never starts never reaches presentability.
        neverReady = app.OpenCameraWorld(vec3(20.0f, 3.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, base.World);
        app.RebindManagedViewportWhenReady(0, neverReady.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        if (frame == 0)
        {
            CHECK(app.Abandoned.empty());
            app.GetWorldRunner().CloseWorld(neverReady.World);
        }
        else if (frame == 2)
        {
            // The destination vanished mid-wait: reported exactly once, naming the destination it
            // never presented, while the standing record says the same.
            REQUIRE(app.Abandoned.size() == 1);
            CHECK(app.Abandoned[0].first == 0);
            CHECK(app.Abandoned[0].second == neverReady.World);
            CHECK(app.GetAbandonedManagedPresentWorld(0) == neverReady.World);

            // A later rebind of the same index supersedes the record; it does not re-report the
            // abandonment, and its own completion is a presented moment, not an abandoned one.
            app.RebindManagedViewport(0, base.World);
        }
        else if (frame == 4)
        {
            CHECK(app.Abandoned.size() == 1);
            CHECK(app.GetAbandonedManagedPresentWorld(0) == WorldInstanceId{});
            REQUIRE(app.Presented.size() == 1);
            CHECK(app.Presented[0].World == base.World);
        }
    };

    app.Frames = 6;
    app.Run({});
}

TEST_CASE("A world the viewport leaves departs once, untouched by a tick, after the destination "
          "presents")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat a{};
    MvApp::WorldSeat b{};
    u64 tickOfA = 0;

    app.InitFn = [&](MvApp& app)
    {
        a = app.OpenReadyCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        b = app.OpenReadyCameraWorld(vec3(20.0f, 3.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, a.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        if (frame == 1)
        {
            // The last frame A presents: the rebind applies at the next frame's top.
            CHECK(app.Departed.empty());
            app.RebindManagedViewport(0, b.World);
            tickOfA = app.GetWorldRunner().ResolveWorld(a.World)->Clock.GetTick();
        }
        else if (frame == 4)
        {
            REQUIRE(app.Departed.size() == 1);
            CHECK(app.Departed[0].World == a.World);
            // A had not ticked again when the hook ran, and it stays open afterwards.
            CHECK(app.Departed[0].TickAtCall == tickOfA);
            CHECK(app.GetWorldRunner().ResolveWorld(a.World) != nullptr);
            // The destination's presentation was reported before the source's departure.
            REQUIRE(app.Hooks.size() == 2);
            CHECK(app.Hooks[0] == std::pair{'P', b.World});
            CHECK(app.Hooks[1] == std::pair{'D', a.World});
        }
    };

    app.Frames = 6;
    app.Run({});
}

TEST_CASE("A world closed while presented, and an abandoned destination, never depart")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    MvApp app(HeadlessInfo({ManagedViewportInfo{}}), types, systems);

    MvApp::WorldSeat closing{};
    MvApp::WorldSeat base{};
    MvApp::WorldSeat neverReady{};

    app.InitFn = [&](MvApp& app)
    {
        closing = app.OpenReadyCameraWorld(vec3(0.0f, 0.0f, 5.0f));
        base = app.OpenReadyCameraWorld(vec3(20.0f, 3.0f, 5.0f));
        neverReady = app.OpenCameraWorld(vec3(-10.0f, 1.0f, 5.0f));
        app.GetManagedViewports().SetViewportWorld(0, closing.World);
    };

    app.StepFn = [&](MvApp& app, int frame)
    {
        if (frame == 1)
        {
            // Close the presented world, then move the viewport off it.
            app.GetWorldRunner().CloseWorld(closing.World);
            app.RebindManagedViewport(0, base.World);
        }
        else if (frame == 3)
        {
            CHECK(app.GetManagedViewportWorld(0) == base.World);
            // A destination that never readies is abandoned once it vanishes mid-wait.
            app.RebindManagedViewportWhenReady(0, neverReady.World);
        }
        else if (frame == 4)
        {
            app.GetWorldRunner().CloseWorld(neverReady.World);
        }
        else if (frame == 6)
        {
            REQUIRE(app.Abandoned.size() == 1);
            CHECK(app.Departed.empty());
        }
    };

    app.Frames = 8;
    app.Run({});
}

TEST_CASE("A departing world still holds its presentation pin in the hook, and loses it after")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    // A managed world, so the application builds its world directory and pins what it presents.
    ApplicationInfo info = HeadlessInfo({ManagedViewportInfo{}});
    info.World = GameWorldInfo{.Project = TestSupport::WriteBootstrapFixture(types, "departed")};
    MvApp app(std::move(info), types, systems);

    MvApp::WorldSeat destination{};
    WorldInstanceId start;

    app.StepFn = [&](MvApp& app, int frame)
    {
        if (frame == 0)
        {
            start = app.GetManagedViewportWorld(0);
            REQUIRE(start.IsValid());
            destination = app.OpenReadyCameraWorld(vec3(0.0f, 0.0f, 5.0f));
            app.RebindManagedViewport(0, destination.World);
        }
        else if (frame == 3)
        {
            REQUIRE(app.Departed.size() == 1);
            CHECK(app.Departed[0].World == start);
            CHECK(app.Departed[0].PresenceAtCall == 1);
            CHECK(app.GetWorldDirectory()->PresenceOf(start) == 0);
        }
    };

    app.Frames = 5;
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
