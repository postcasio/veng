// The Application world drive: WorldRunner::OpenWorld makes the engine tick a world each frame
// (started, non-paused, in id order) and drive its CaptureSurface components, decoupled from
// rendering. Drives a real headless Application (its own Context, no window, no ImGui) through Run(),
// opening worlds from OnInitialize and asserting from OnUpdate:
//
//  - opening a world ticks it, closing it stops, and a scene never opened as a world never ticks
//    (the opt-in);
//  - a paused world does not tick while a flat-peer world keeps ticking, each paused by its own
//    handle (SetWorldPaused / IsWorldPaused are handle-keyed; no privileged primary);
//  - SystemContext carries Tasks always, View + Debug for a presented world, and View == nullopt for
//    a view-less world and around a never-pushed viewport (no crash);
//  - the engine drives a presented world's captures even while its sim is paused, and drives none of
//    an unpresented world's (presentation, not run-state, gates capture driving).
//
// It needs a Context for the Application (viewports, captures), so it rides the gpu band though it
// pins no pixels. Each world is opened through the runner from a synthetic empty-prefab Level naming
// the systems under test, so per-world system subsets stay independent.

#include <doctest/doctest.h>

#include <optional>

#include <Veng/Application.h>
#include <Veng/Input.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

namespace
{
    // A per-tag probe recording, from its OnUpdate, what the engine handed its SystemContext. Static
    // slots keyed by Tag so two open worlds observe independently; Reset() clears them per case.
    //
    // It runs in the View phase, which the fixed-timestep drive ticks once per frame (the Sim phase
    // steps at the fixed rate off the accumulated wall clock, which is near-zero in this tight
    // headless loop). So Updates counts frames the world was driven — the per-frame drive semantic
    // these cases pin (opening ticks, pause stops), decoupled from the sim tick rate.
    template <int Tag>
    struct ProbeSystem final : SceneSystem
    {
        static inline int Updates = 0;
        static inline bool HadView = false;
        static inline bool HadDebug = false;
        static inline const TaskSystem* Tasks = nullptr;
        static inline Renderer::ViewportRegion Region{};

        static void Reset()
        {
            Updates = 0;
            HadView = false;
            HadDebug = false;
            Tasks = nullptr;
            Region = {};
        }

        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        void OnUpdate(Scene&, f32, const SystemContext& context) override
        {
            ++Updates;
            Tasks = &context.Tasks;
            HadView = context.View.has_value();
            HadDebug = context.Debug != nullptr;
            if (context.View)
            {
                Region = context.View->Region;
            }
        }
    };
}

namespace Veng
{
    template <int Tag>
    struct VengSystem<ProbeSystem<Tag>>
    {
        static constexpr SystemId Id = 0x5D1D000000000000ULL + static_cast<SystemId>(Tag);
        static string Name() { return "ProbeSystem"; }
    };
}

namespace
{
    // A headless application driven by two closures: InitFn (from OnInitialize, engine ready) and
    // StepFn (each OnUpdate, with the frame index). Viewports live on the app so any still open at
    // teardown drop from ~DriveApp while the engine state is alive; worlds are runner-owned and the
    // runner drops them at teardown.
    class DriveApp final : public Application
    {
    public:
        using Application::Application;

        // Runs before ~Application, while the engine state is alive, so open viewports self-unregister.
        ~DriveApp() override { Viewports.clear(); }

        function<void(DriveApp&)> InitFn;
        function<void(DriveApp&, int)> StepFn;
        int Frames = 5;
        int Current = 0;

        std::vector<AssetHandle<Level>> Levels;
        std::vector<WorldInstanceId> SimIds;
        std::vector<Unique<Renderer::Viewport>> Viewports;

        // Opens a runner-owned world from a synthetic empty-prefab Level running the named systems,
        // started, recording its handle in SimIds, and returns its scene.
        Scene& AddWorld(std::vector<SystemId> systems)
        {
            const AssetHandle<Prefab> prefab =
                GetAssetManager().Adopt<Prefab>(Prefab::Create({}, {}));
            const AssetHandle<Level> level = GetAssetManager().Adopt<Level>(
                Level::Create(prefab, std::move(systems), GameModeConfig{}, LevelRenderSettings{}));
            Levels.push_back(level);
            const WorldInstanceId id = GetWorldRunner().OpenWorld(WorldOpenInfo{
                .Source = level,
                .MakeStartContext =
                    [this]
                {
                    return SystemContext{
                        .Assets = GetAssetManager(), .Input = GetInput(), .Tasks = GetTaskSystem()};
                },
            });
            SimIds.push_back(id);
            return GetWorldRunner().ResolveWorld(id)->GetScene();
        }

        Renderer::Viewport& AddPresentedViewport()
        {
            Unique<Renderer::Viewport> viewport = Renderer::Viewport::Create({
                .Context = GetRenderContext(),
                .Assets = GetAssetManager(),
                .Region = {.Offset = {0, 0}, .Extent = {64, 64}},
                .Role = Renderer::ViewportRole::Presented,
            });
            viewport->SetEnabled(false); // this suite pins drive semantics, not pixels
            RegisterViewport(*viewport);
            Viewports.push_back(std::move(viewport));
            return *Viewports.back();
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

    ApplicationInfo HeadlessInfo()
    {
        ApplicationInfo info;
        info.Name = "veng-simulation-drive-list-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        return info;
    }
}

TEST_CASE("Opening a world ticks it, closing it stops, and an unopened scene never ticks")
{
    ProbeSystem<1>::Reset();
    ProbeSystem<2>::Reset();

    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<ProbeSystem<1>>();
    systems.Register<ProbeSystem<2>>();

    DriveApp app(HeadlessInfo(), types, systems);

    // A scene never opened as a world the engine must never auto-tick (the opt-in), kept alive here.
    Unique<Scene> unopened;

    app.InitFn = [&](DriveApp& a)
    {
        a.AddWorld({SystemIdOf<ProbeSystem<1>>()});

        unopened = Scene::Create(a.GetTypeRegistry());
        unopened->SetSimulation(CreateUnique<SceneSimulation>(
            a.GetSystemRegistry(), std::vector<SystemId>{SystemIdOf<ProbeSystem<2>>()}));
        unopened->StartSimulation(SystemContext{
            .Assets = a.GetAssetManager(), .Input = a.GetInput(), .Tasks = a.GetTaskSystem()});
    };

    int atClose = 0;
    app.StepFn = [&](DriveApp& a, int frame)
    {
        if (frame == 2)
        {
            // The opened world ticked each frame so far; the unopened scene never did.
            CHECK(ProbeSystem<1>::Updates >= 2);
            CHECK(ProbeSystem<2>::Updates == 0);

            // Close the opened world: the engine stops ticking it.
            atClose = ProbeSystem<1>::Updates;
            a.GetWorldRunner().CloseWorld(a.SimIds[0]);
        }
        else if (frame == 4)
        {
            CHECK(ProbeSystem<1>::Updates == atClose); // no further ticks after the close
            CHECK(ProbeSystem<2>::Updates == 0);       // still never auto-ticked
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("A paused world does not tick while a flat-peer world does, resolved by handle")
{
    ProbeSystem<1>::Reset();
    ProbeSystem<2>::Reset();

    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<ProbeSystem<1>>();
    systems.Register<ProbeSystem<2>>();

    DriveApp app(HeadlessInfo(), types, systems);

    app.InitFn = [&](DriveApp& a)
    {
        a.AddWorld({SystemIdOf<ProbeSystem<1>>()}); // world #0
        a.AddWorld({SystemIdOf<ProbeSystem<2>>()}); // world #1
    };

    int pausedAt = 0;
    app.StepFn = [&](DriveApp& a, int frame)
    {
        if (frame == 1)
        {
            CHECK_FALSE(a.IsWorldPaused(a.SimIds[0]));
            // Pause world #0 by its handle, leaving world #1 ticking: worlds are flat peers, each
            // paused independently by id — no privileged primary.
            a.SetWorldPaused(a.SimIds[0], true);
            CHECK(a.IsWorldPaused(a.SimIds[0]));
            pausedAt = ProbeSystem<1>::Updates;
        }
        else if (frame == 4)
        {
            CHECK(ProbeSystem<1>::Updates == pausedAt); // world #0 frozen since the pause
            CHECK(ProbeSystem<2>::Updates > pausedAt);  // world #1 kept ticking
            CHECK(a.IsWorldPaused(a.SimIds[0]));
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("SystemContext carries Tasks always and View/Debug only for a presented world")
{
    ProbeSystem<1>::Reset();
    ProbeSystem<2>::Reset();

    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<ProbeSystem<1>>();
    systems.Register<ProbeSystem<2>>();

    DriveApp app(HeadlessInfo(), types, systems);

    Scene* presented = nullptr;

    app.InitFn = [&](DriveApp& a)
    {
        presented = &a.AddWorld({SystemIdOf<ProbeSystem<1>>()}); // gets a viewport
        a.AddWorld({SystemIdOf<ProbeSystem<2>>()});              // view-less

        a.AddPresentedViewport(); // presents `presented` (pushed each frame below)
        a.AddPresentedViewport(); // never pushed — must not match or crash
    };

    app.StepFn = [&](DriveApp& a, int frame)
    {
        // Push the first viewport's ViewState toward `presented`; the engine ticks before this push,
        // so the world reads it (the retained state) from the next frame on.
        a.Viewports.front()->SetViewState({.World = presented, .Delta = 0.016f});

        if (frame == 3)
        {
            // Tasks is always the app's task system, for every ticked world.
            CHECK(ProbeSystem<1>::Tasks == &a.GetTaskSystem());
            CHECK(ProbeSystem<2>::Tasks == &a.GetTaskSystem());

            // The presented world resolved View + Debug from its viewport; the view-less one did not.
            CHECK(ProbeSystem<1>::HadView);
            CHECK(ProbeSystem<1>::HadDebug);
            CHECK(ProbeSystem<1>::Region.Extent == uvec2(64, 64));
            CHECK_FALSE(ProbeSystem<2>::HadView);
            CHECK_FALSE(ProbeSystem<2>::HadDebug);

            // The free document-space helper over the assembled view matches Viewport::WorldToDocument
            // (the lifted math is the viewport's own, reached without a Renderer::Viewport).
            const Renderer::Viewport& vp = *a.Viewports.front();
            const SystemViewInfo view{.Camera = vp.GetPresentedCamera(),
                                      .Region = vp.GetRegion(),
                                      .UiScale = vp.GetUiScale()};
            const vec3 world(1.0f, 2.0f, -3.0f);
            const optional<vec2> viaHelper = WorldToDocument(view, world);
            const optional<vec2> viaViewport = vp.WorldToDocument(world);
            REQUIRE(viaHelper.has_value() == viaViewport.has_value());
            if (viaHelper.has_value())
            {
                CHECK(viaHelper->x == doctest::Approx(viaViewport->x));
                CHECK(viaHelper->y == doctest::Approx(viaViewport->y));
            }
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("The engine drives a presented world's captures even paused, and none of an unpresented "
          "world's")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    DriveApp app(HeadlessInfo(), types, systems);

    Scene* presented = nullptr;
    Scene* dark = nullptr;
    Entity presentedCapture = Entity::Null;
    Entity darkCapture = Entity::Null;

    const auto addCapture = [](Scene& scene, Entity& entity)
    {
        entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        scene.Add<Renderer::CaptureSurface>(entity).Resolution = 16;
    };

    app.InitFn = [&](DriveApp& a)
    {
        a.AddWorld({}); // world #0, empty

        // Two further open worlds, each with a CaptureSurface entity, both with their sim paused: one
        // presented through a viewport the app pushes each frame, one presented by nothing.
        presented = &a.AddWorld({});
        addCapture(*presented, presentedCapture);
        presented->GetSimulation()->SetPaused(true);

        dark = &a.AddWorld({});
        addCapture(*dark, darkCapture);
        dark->GetSimulation()->SetPaused(true);

        a.AddPresentedViewport();
    };

    app.StepFn = [&](DriveApp& a, int frame)
    {
        // Presented from the first frame on: the engine resolves presentation from the scene a
        // registered viewport's pushed ViewState names, so a consumer driving its own viewport is
        // covered as an engine-managed binding is.
        a.Viewports.front()->SetViewState({.World = presented, .Delta = 0.016f});

        if (frame == 2)
        {
            // The presented world's capture materialized despite the pause — run-state is not what
            // gates capture driving.
            CHECK(presented->Get<Renderer::CaptureSurface>(presentedCapture).GetCapture() !=
                  nullptr);

            // The world no view shows drove nothing: with no view to sample it, its capture would be a
            // scene render and a view slot spent on an image nothing can read.
            CHECK(dark->Get<Renderer::CaptureSurface>(darkCapture).GetCapture() == nullptr);
        }
    };

    app.Frames = 3;
    app.Run({});
}
