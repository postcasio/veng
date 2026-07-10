// The Application simulation drive-list: RegisterSimulation makes the engine tick a scene each frame
// (started, non-paused, in registration order) and drive its CaptureSurface components, decoupled
// from rendering. Drives a real headless Application (its own Context, no window, no ImGui) through
// Run(), registering scenes as simulations from OnInitialize and asserting from OnUpdate:
//
//  - registration ticks, deregistration (dropping the scene) stops, and an unregistered scene is
//    never auto-ticked (the opt-in);
//  - a paused sim does not tick while another keeps ticking; the primary is simulation #0, and
//    SetWorldPaused / IsWorldPaused delegate to it;
//  - SystemContext carries Tasks always, View + Debug for a presented sim, and View == nullopt for a
//    view-less sim and around a never-pushed viewport (no crash);
//  - the engine drives captures over every registered scene, including a paused one (registration,
//    not run-state, gates capture driving).
//
// It needs a Context for the Application (viewports, captures), so it rides the gpu band though it
// pins no pixels.

#include <doctest/doctest.h>

#include <optional>

#include <Veng/Application.h>
#include <Veng/Input.h>
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
    // slots keyed by Tag so two registered sims observe independently; Reset() clears them per case.
    //
    // It runs in the View phase, which the fixed-timestep drive ticks once per frame (the Sim phase
    // steps at the fixed rate off the accumulated wall clock, which is near-zero in this tight
    // headless loop). So Updates counts frames the scene was driven — the per-frame drive-list
    // semantic these cases pin (registration ticks, pause stops), decoupled from the sim tick rate.
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
    // StepFn (each OnUpdate, with the frame index). Scenes registered as simulations live on the app
    // so any still registered at teardown drop from OnDispose while the engine state is alive.
    class DriveApp final : public Application
    {
    public:
        using Application::Application;

        function<void(DriveApp&)> InitFn;
        function<void(DriveApp&, int)> StepFn;
        int Frames = 5;
        int Current = 0;

        std::vector<Unique<Scene>> Scenes;
        std::vector<Unique<Renderer::Viewport>> Viewports;

        // Registers a fresh scene running the named systems, started, and returns it.
        Scene& AddSimulation(std::vector<SystemId> systems)
        {
            Unique<Scene> scene = Scene::Create(GetTypeRegistry());
            scene->SetSimulation(CreateUnique<SceneSimulation>(GetSystemRegistry(), systems));
            RegisterSimulation(*scene);
            scene->StartSimulation(SystemContext{
                .Assets = GetAssetManager(), .Input = GetInput(), .Tasks = GetTaskSystem()});
            Scenes.push_back(std::move(scene));
            return *Scenes.back();
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

        void OnDispose() override
        {
            Viewports.clear();
            Scenes.clear();
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

TEST_CASE("Registration ticks a scene, dropping it stops, and an unregistered scene never ticks")
{
    ProbeSystem<1>::Reset();
    ProbeSystem<2>::Reset();

    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<ProbeSystem<1>>();
    systems.Register<ProbeSystem<2>>();

    DriveApp app(HeadlessInfo(), types, systems);

    // An unregistered scene the engine must never auto-tick (the opt-in), kept alive here.
    Unique<Scene> unregistered;

    app.InitFn = [&](DriveApp& a)
    {
        a.AddSimulation({SystemIdOf<ProbeSystem<1>>()});

        unregistered = Scene::Create(a.GetTypeRegistry());
        unregistered->SetSimulation(CreateUnique<SceneSimulation>(
            a.GetSystemRegistry(), std::vector<SystemId>{SystemIdOf<ProbeSystem<2>>()}));
        unregistered->StartSimulation(SystemContext{
            .Assets = a.GetAssetManager(), .Input = a.GetInput(), .Tasks = a.GetTaskSystem()});
    };

    int atDrop = 0;
    app.StepFn = [&](DriveApp&, int frame)
    {
        if (frame == 2)
        {
            // The registered scene ticked each frame so far; the unregistered one never did.
            CHECK(ProbeSystem<1>::Updates >= 2);
            CHECK(ProbeSystem<2>::Updates == 0);

            // Drop the registered scene: it self-unregisters, so the engine stops ticking it.
            atDrop = ProbeSystem<1>::Updates;
            app.Scenes.clear();
        }
        else if (frame == 4)
        {
            CHECK(ProbeSystem<1>::Updates == atDrop); // no further ticks after deregistration
            CHECK(ProbeSystem<2>::Updates == 0);      // still never auto-ticked
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("A paused sim does not tick while another does; the primary is sim #0")
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
        a.AddSimulation({SystemIdOf<ProbeSystem<1>>()}); // sim #0 (the primary)
        a.AddSimulation({SystemIdOf<ProbeSystem<2>>()}); // sim #1
    };

    int pausedAt = 0;
    app.StepFn = [&](DriveApp& a, int frame)
    {
        if (frame == 1)
        {
            CHECK_FALSE(a.IsWorldPaused());
            // SetWorldPaused delegates to the primary sim (#0): pause it, leaving sim #1 ticking.
            a.SetWorldPaused(true);
            CHECK(a.IsWorldPaused());
            pausedAt = ProbeSystem<1>::Updates;
        }
        else if (frame == 4)
        {
            CHECK(ProbeSystem<1>::Updates == pausedAt); // sim #0 frozen since the pause
            CHECK(ProbeSystem<2>::Updates > pausedAt);  // sim #1 kept ticking
            CHECK(a.IsWorldPaused());
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("SystemContext carries Tasks always and View/Debug only for a presented sim")
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
        presented = &a.AddSimulation({SystemIdOf<ProbeSystem<1>>()}); // gets a viewport
        a.AddSimulation({SystemIdOf<ProbeSystem<2>>()});              // view-less

        a.AddPresentedViewport(); // presents `presented` (pushed each frame below)
        a.AddPresentedViewport(); // never pushed — must not match or crash
    };

    app.StepFn = [&](DriveApp& a, int frame)
    {
        // Push the first viewport's ViewState toward `presented`; the engine ticks before this push,
        // so the sim reads it (the retained state) from the next frame on.
        a.Viewports.front()->SetViewState({.World = presented, .Delta = 0.016f});

        if (frame == 3)
        {
            // Tasks is always the app's task system, for every ticked sim.
            CHECK(ProbeSystem<1>::Tasks == &a.GetTaskSystem());
            CHECK(ProbeSystem<2>::Tasks == &a.GetTaskSystem());

            // The presented sim resolved View + Debug from its viewport; the view-less one did not.
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

TEST_CASE("The engine drives captures over every registered scene, including a paused one")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    DriveApp app(HeadlessInfo(), types, systems);

    Scene* secondary = nullptr;
    Entity captureEntity = Entity::Null;

    app.InitFn = [&](DriveApp& a)
    {
        a.AddSimulation({}); // sim #0 (the primary), empty

        // A secondary registered scene with a CaptureSurface entity, its sim paused so only
        // registration — not run-state — can drive its capture (the seam-1 fix).
        secondary = &a.AddSimulation({});
        captureEntity = secondary->CreateEntity();
        secondary->Add<Transform>(captureEntity);
        auto& capture = secondary->Add<Renderer::CaptureSurface>(captureEntity);
        capture.Resolution = 16;
        secondary->GetSimulation()->SetPaused(true);
    };

    app.StepFn = [&](DriveApp&, int frame)
    {
        if (frame == 2)
        {
            // The engine materialized the secondary (paused) scene's capture — it drove it despite
            // the pause, because registration alone gates capture driving.
            CHECK(secondary->Get<Renderer::CaptureSurface>(captureEntity).GetCapture() != nullptr);
        }
    };

    app.Frames = 3;
    app.Run({});
}
