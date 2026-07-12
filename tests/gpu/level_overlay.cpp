// LevelOverlay: opening a whole Level as a secondary, simulated overlay over a running Application.
//
// LevelOverlay is a thin preset over WorldRunner::OpenWorld: opening one opens an owned world (the
// runner ticks it and the engine pushes its camera each frame, so there is no per-frame game call)
// and applies an overlay policy — a Presented viewport on top, a cursor-seat and focus handoff, and
// an optional refcounted pause on a caller-named covered world. Drives a real headless Application
// (its own Context, no window, no ImGui) through Run(), opening and dropping LevelOverlay handles
// from OnUpdate. The overlay creates a Presented viewport, so the suite is GPU-band (it needs a
// Context, though the assertions are router/scene state, not pixels); the gpu/main.cpp harness skips
// the whole band with no ICD.
//
// It pins: the open/close lifecycle leaving the router (cursor seat + pointer associations)
// byte-restored (including a drop while the focus scope is live, and asserting the pointer
// association was cleared); the populate hook running before StartSimulation; a stacked overlay (B
// over A) suspending the layer beneath's input and restoring it LIFO; a structural change to the
// lower overlay's scene while a higher one is open, then a clean close (the InputSeat re-resolve
// guard); the covered-world pause refcount (stacked overlays hold it until the last closes, and an
// explicit game pause composes without being clobbered); the full-window region resolving to the
// framebuffer extent while a fixed region is placed as given; an overlay rendering its scene through
// the drive-list with no per-frame game call; and a clean teardown with an overlay world still open.

#include <doctest/doctest.h>

#include <optional>

#include <Veng/Application.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/LevelOverlay.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Asset/Level.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/SeatFocusScope.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

using namespace Veng;

namespace
{
    // The action + key the input-suspension checks resolve against.
    constexpr ActionId Move{0xA1};
    constexpr u32 KeyW = u32(Key::W);

    // Records, from its OnStart, whether the populate hook's entity was already present — the proof
    // that Populate runs before StartSimulation.
    int g_StartSawPopulate = 0;

    struct PopulateProbe final : SceneSystem
    {
        void OnStart(Scene& scene, const SystemContext&) override
        {
            for (auto [entity, name] : scene.View<Name>())
            {
                if (name.Value == "populated")
                {
                    ++g_StartSawPopulate;
                }
            }
        }
        void OnUpdate(Scene&, f32, const SystemContext&) override {}
    };
}

namespace Veng
{
    template <>
    struct VengSystem<PopulateProbe>
    {
        static constexpr SystemId Id = 0x0AE12A0000000001ULL;
        static string Name() { return "PopulateProbe"; }
    };
}

namespace
{
    // A resident W -> Move.y context (an ordinary Adopt handle; InputMappingSystem gates on
    // IsLoaded(), not on the id, so an adopted resource resolves).
    AssetHandle<InputMappingContext> MakeMoveContext(AssetManager& assets)
    {
        Ref<InputMappingContext> resource = InputMappingContext::Create(
            {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D}},
            {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                     .Action = Move,
                     .Axis = AxisComponent::Y,
                     .Scale = 1.0f}});
        return assets.Adopt<InputMappingContext>(std::move(resource));
    }

    // Serializes one default-or-given component into a prefab record.
    template <typename T>
    Prefab::Component Comp(const T& value, const TypeRegistry& types)
    {
        Prefab::Component component;
        component.Type = types.IdOf<T>();
        WriteFields(component.Record, &value, types.Info(component.Type), types);
        return component;
    }

    // A level over a one-seat world prefab: an authored input seat (Viewer / InputContextStack /
    // PlayerInput / SeatInput) plus a Transform, resident and ready for LoadInto. `leadingDummies`
    // pads the prefab with that many Name-only entities before the seat, so two levels built here
    // resolve to distinct seat entities (the nesting checks need the two overlays' seats to differ).
    AssetHandle<Level> BuildSeatLevel(AssetManager& assets, const TypeRegistry& types,
                                      vector<SystemId> systems, int leadingDummies = 0)
    {
        vector<Prefab::PrefabEntity> entities;
        for (int i = 0; i < leadingDummies; ++i)
        {
            entities.push_back({{Comp(Name{"dummy"}, types)}});
        }
        entities.push_back({{
            Comp(Viewer{}, types),
            Comp(InputContextStack{}, types),
            Comp(PlayerInput{}, types),
            Comp(SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None}, types),
            Comp(Transform{}, types),
        }});
        const AssetHandle<Prefab> world =
            assets.Adopt<Prefab>(Prefab::Create(std::move(entities), {}));
        return assets.Adopt<Level>(
            Level::Create(world, std::move(systems), GameModeConfig{}, LevelRenderSettings{}));
    }

    // A headless application driven by two closures: InitFn (from OnInitialize, engine ready) and
    // StepFn (each OnUpdate, with the frame index). Overlays live on the app so any still open at
    // teardown are dropped from OnDispose, while the router/assets/context are still alive.
    class OverlayApp final : public Application
    {
    public:
        using Application::Application;

        function<void(OverlayApp&)> InitFn;
        function<void(OverlayApp&, int)> StepFn;
        int Frames = 6;
        int Current = 0;

        std::optional<LevelOverlay> A;
        std::optional<LevelOverlay> B;

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
            B.reset();
            A.reset();
        }
    };

    ApplicationInfo HeadlessInfo()
    {
        ApplicationInfo info;
        info.Name = "veng-level-overlay-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        return info;
    }

    // Resolves a seat's Move.y from a scene under a held-W snapshot: 1 when the seat's context
    // resolves the binding, 0 when it is suspended (its contexts swapped to the empty context).
    f32 ResolveMoveY(AssetManager& assets, Scene& scene)
    {
        Input input(nullptr);
        input.BeginFrame();
        input.ApplyEvent(KeyPressedEvent{Key::W, 0, 0});

        InputMappingSystem mapping;
        mapping.OnUpdate(
            scene, 0.016f,
            SystemContext{.Assets = assets, .Input = input, .Tasks = assets.GetTaskSystem()});

        const InputSeat seat = ResolveInputSeat(&scene);
        return scene.Get<PlayerInput>(seat.Viewer).GetValue(Move).y;
    }

    Entity SeatOf(const LevelOverlay& overlay)
    {
        return overlay.GetSeat();
    }
}

TEST_CASE("LevelOverlay open/close leaves the router byte-restored, no per-frame game call")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;

    Entity priorCursor = Entity::Null;
    Entity overlaySeat = Entity::Null;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        const InputRouter& router = a.GetInputRouter();
        if (frame == 0)
        {
            // Pre-open router state.
            priorCursor = router.GetCursorSeat();
            CHECK(router.ResolvePointer(ivec2(100, 100), false, Entity::Null).Owner ==
                  Entity::Null);

            a.A = LevelOverlay::Open(a, LevelOverlayInfo{.Source = level});
            a.A->GetViewport().SetEnabled(false); // this suite pins router/scene state, not pixels
            overlaySeat = SeatOf(*a.A);

            // While open: the overlay owns the cursor seat and a free pointer over its region.
            CHECK(overlaySeat != Entity::Null);
            CHECK(router.GetCursorSeat() == overlaySeat);
            CHECK(router.ResolvePointer(ivec2(100, 100), false, Entity::Null).Owner == overlaySeat);
        }
        else if (frame == 1)
        {
            // No per-frame Update call: the runner ticked the overlay world and the engine pushed its
            // camera on its own. The overlay still owns the cursor seat.
            CHECK(router.GetCursorSeat() == overlaySeat);
        }
        else if (frame == 2)
        {
            // Drop while the focus scope is live — the teardown-order guard.
            a.A.reset();

            // Byte-restored: cursor seat and pointer association back to pre-open.
            CHECK(router.GetCursorSeat() == priorCursor);
            CHECK(router.ResolvePointer(ivec2(100, 100), false, Entity::Null).Owner ==
                  Entity::Null); // ClearViewportSeat ran
        }
    };

    app.Frames = 4;
    app.Run({});
}

TEST_CASE("LevelOverlay runs the populate hook before StartSimulation")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<PopulateProbe>();
    g_StartSawPopulate = 0;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;

    app.InitFn = [&](OverlayApp& a)
    {
        level =
            BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {SystemIdOf<PopulateProbe>()});
    };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        if (frame == 0)
        {
            a.A = LevelOverlay::Open(a,
                                     LevelOverlayInfo{
                                         .Source = level,
                                         .Populate =
                                             [](Scene& scene)
                                         {
                                             const Entity e = scene.CreateEntity();
                                             scene.Add<Name>(e, Name{"populated"});
                                         },
                                     });
            a.A->GetViewport().SetEnabled(false);
        }
        else if (frame == 1)
        {
            a.A.reset();
        }
    };

    app.Frames = 3;
    app.Run({});

    // OnStart saw the populate hook's entity exactly once — the hook ran before the sim started.
    CHECK(g_StartSawPopulate == 1);
}

TEST_CASE("A stacked overlay suspends the layer beneath's input and restores it LIFO")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> levelA;
    AssetHandle<Level> levelB;
    AssetHandle<InputMappingContext> moveContext;

    Entity seatA = Entity::Null;
    Entity seatB = Entity::Null;
    Entity priorCursor = Entity::Null;

    app.InitFn = [&](OverlayApp& a)
    {
        levelA = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}, 0);
        levelB = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}, 1);
        moveContext = MakeMoveContext(a.GetAssetManager());
    };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        const InputRouter& router = a.GetInputRouter();
        AssetManager& assets = a.GetAssetManager();
        if (frame == 0)
        {
            priorCursor = router.GetCursorSeat();

            a.A = LevelOverlay::Open(a, LevelOverlayInfo{.Source = levelA});
            a.A->GetViewport().SetEnabled(false);
            seatA = SeatOf(*a.A);
            // Give A's seat a gameplay context so its suspension is observable.
            a.A->GetScene().Get<InputContextStack>(seatA).Active = {moveContext};
            CHECK(router.GetCursorSeat() == seatA);
            CHECK(ResolveMoveY(assets, a.A->GetScene()) == doctest::Approx(1.0f));
        }
        else if (frame == 1)
        {
            a.B = LevelOverlay::Open(a, LevelOverlayInfo{.Source = levelB});
            a.B->GetViewport().SetEnabled(false);
            seatB = SeatOf(*a.B);
            a.B->GetScene().Get<InputContextStack>(seatB).Active = {moveContext};

            CHECK(seatB != seatA); // the two overlays resolve distinct seats
            CHECK(router.GetCursorSeat() == seatB);

            // A is suspended (its contexts swapped to the empty context); B resolves.
            CHECK(ResolveMoveY(assets, a.A->GetScene()) == doctest::Approx(0.0f));
            CHECK(ResolveMoveY(assets, a.B->GetScene()) == doctest::Approx(1.0f));
        }
        else if (frame == 2)
        {
            a.B.reset();
            // Closing B returns focus/cursor to A (not the base) and restores A's input.
            CHECK(router.GetCursorSeat() == seatA);
            CHECK(ResolveMoveY(assets, a.A->GetScene()) == doctest::Approx(1.0f));
        }
        else if (frame == 3)
        {
            a.A.reset();
            // Closing A returns to the pre-open cursor seat; the association is gone.
            CHECK(router.GetCursorSeat() == priorCursor);
            CHECK(router.ResolvePointer(ivec2(50, 50), false, Entity::Null).Owner == Entity::Null);
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("A structural change to the lower overlay's scene then a clean close (re-resolve guard)")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> levelA;
    AssetHandle<Level> levelB;
    AssetHandle<InputMappingContext> moveContext;
    Entity seatA = Entity::Null;

    app.InitFn = [&](OverlayApp& a)
    {
        levelA = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}, 0);
        levelB = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}, 1);
        moveContext = MakeMoveContext(a.GetAssetManager());
    };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        AssetManager& assets = a.GetAssetManager();
        if (frame == 0)
        {
            a.A = LevelOverlay::Open(a, LevelOverlayInfo{.Source = levelA});
            a.A->GetViewport().SetEnabled(false);
            seatA = SeatOf(*a.A);
            a.A->GetScene().Get<InputContextStack>(seatA).Active = {moveContext};

            a.B = LevelOverlay::Open(a, LevelOverlayInfo{.Source = levelB});
            a.B->GetViewport().SetEnabled(false);
            // A is suspended by B.
            CHECK(ResolveMoveY(assets, a.A->GetScene()) == doctest::Approx(0.0f));
        }
        else if (frame == 1)
        {
            // Structural change to the suspended (lower) scene: grow its InputContextStack pool so
            // a cached borrowed pointer would dangle.
            Scene& lower = a.A->GetScene();
            for (int i = 0; i < 64; ++i)
            {
                lower.Add<InputContextStack>(lower.CreateEntity());
            }
        }
        else if (frame == 2)
        {
            // Closing B restores A's contexts through a fresh resolve into the moved pool.
            a.B.reset();
            CHECK(ResolveMoveY(assets, a.A->GetScene()) == doctest::Approx(1.0f));
            a.A.reset();
        }
    };

    app.Frames = 4;
    app.Run({});
}

TEST_CASE("The covered-world pause is a refcount that stacks and composes with an explicit toggle")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;

    // The world the overlays cover — an ordinary runner-owned world named as CoveredWorld. It needs
    // no simulation for the pause assertions; IsWorldPaused reads its refcount + explicit toggle.
    WorldInstanceId baseWorld;

    app.InitFn = [&](OverlayApp& a)
    {
        level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {});
        baseWorld = a.GetWorldRunner().OpenWorld(WorldOpenInfo{.StartSimulation = false});
    };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        const auto open = [&](std::optional<LevelOverlay>& slot, WorldInstanceId covered)
        {
            slot =
                LevelOverlay::Open(a, LevelOverlayInfo{.Source = level, .CoveredWorld = covered});
            slot->GetViewport().SetEnabled(false);
        };

        if (frame == 0)
        {
            CHECK_FALSE(a.IsWorldPaused(baseWorld));
            open(a.A, WorldInstanceId{}); // a default overlay covers nothing
            CHECK_FALSE(a.IsWorldPaused(baseWorld));
            open(a.B, baseWorld); // covering it holds a pause scope
            CHECK(a.IsWorldPaused(baseWorld));
        }
        else if (frame == 1)
        {
            a.B.reset();
            CHECK_FALSE(a.IsWorldPaused(baseWorld)); // A covered nothing, so the base resumes
            a.A.reset();

            // Stacked pause: both cover the base; closing the inner leaves it paused under the outer
            // (the refcount, not a boolean).
            open(a.A, baseWorld);
            CHECK(a.IsWorldPaused(baseWorld));
            open(a.B, baseWorld);
            CHECK(a.IsWorldPaused(baseWorld));
        }
        else if (frame == 2)
        {
            a.B.reset();
            CHECK(a.IsWorldPaused(baseWorld)); // stays paused under A's scope
            a.A.reset();
            CHECK_FALSE(a.IsWorldPaused(baseWorld)); // the last scope dropped, so it resumes

            // A base the game paused itself composes with an overlay's scope without clobbering.
            a.SetWorldPaused(baseWorld, true);
            open(a.A, baseWorld);
        }
        else if (frame == 3)
        {
            a.A.reset();
            CHECK(a.IsWorldPaused(baseWorld)); // the explicit toggle still holds it
            a.SetWorldPaused(baseWorld, false);
            CHECK_FALSE(a.IsWorldPaused(baseWorld));
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("A full-window overlay resolves to the framebuffer extent; a fixed region does not")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        const uvec2 extent = a.GetRenderContext().GetRenderExtent();
        if (frame == 0)
        {
            // Full window (zero-extent region) resolves to the framebuffer extent and carries a
            // Layout so the compositor re-fits it on resize.
            a.A = LevelOverlay::Open(a, LevelOverlayInfo{.Source = level});
            a.A->GetViewport().SetEnabled(false);
            CHECK(a.A->GetViewport().GetRegion().Extent == extent);
            CHECK(a.A->GetViewport().GetLayout().has_value());

            // Fixed sub-region (PiP) placed as given, with no tracking Layout.
            const Renderer::ViewportRegion pip{.Offset = {40, 30}, .Extent = {200, 150}};
            a.B = LevelOverlay::Open(a, LevelOverlayInfo{.Source = level, .Region = pip});
            a.B->GetViewport().SetEnabled(false);
            CHECK(a.B->GetViewport().GetRegion().Offset == pip.Offset);
            CHECK(a.B->GetViewport().GetRegion().Extent == pip.Extent);
            CHECK_FALSE(a.B->GetViewport().GetLayout().has_value());
        }
        else if (frame == 1)
        {
            // The regions are unchanged frame to frame with no window resize (headless).
            CHECK(a.A->GetViewport().GetRegion().Extent == extent);
            CHECK(a.B->GetViewport().GetRegion().Extent == uvec2(200, 150));
        }
        else if (frame == 2)
        {
            a.B.reset();
            a.A.reset();
        }
    };

    app.Frames = 4;
    app.Run({});
}

TEST_CASE("An overlay renders its scene through the engine drive-list with no per-frame game call")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        if (frame == 0)
        {
            a.A = LevelOverlay::Open(a, LevelOverlayInfo{.Source = level});
        }
        else if (frame == 3)
        {
            // The overlay rendered through the drive-list each frame — the engine pushed its camera
            // and the compositor rendered it, with no LevelOverlay::Update call: its output is live.
            CHECK(a.A->GetViewport().GetOutput() != nullptr);
            CHECK(a.A->GetViewport().GetOutputHandle().IsValid());
            a.A.reset();
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("A PiP overlay renders at its sub-region over a live managed primary")
{
    // The render-path assertion the committed PiP golden stands in for: a managed primary viewport
    // renders its own scene every frame while a fixed sub-region overlay renders its scene over it,
    // both live Presented outputs in the same frame composited by the managed gather. A byte-exact
    // reference PNG is deliberately not committed — the display-mode brightness path is not stable
    // enough across platforms for a golden — so this pins the structure the golden would.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    ApplicationInfo info;
    info.Name = "veng-level-overlay-pip";
    info.Headless = true;
    info.ImGui = std::nullopt;
    info.HeadlessExtent = {320, 240};
    info.ManagedViewport = ManagedViewportInfo{};

    OverlayApp app(info, types, systems);
    AssetHandle<Level> level;
    Unique<Scene> primary;
    const Renderer::ViewportRegion pip{.Offset = {40, 30}, .Extent = {120, 90}};

    app.InitFn = [&](OverlayApp& a)
    {
        level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {});
        primary = Scene::Create(a.GetTypeRegistry());
    };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        // Keep the managed primary live each frame — a cleared scene still renders through the full
        // deferred path, so the primary output is a real composited frame the overlay draws over.
        a.GetManagedViewports().Get(0)->SetViewState({.World = primary.get(), .Delta = 0.016f});

        if (frame == 0)
        {
            a.A = LevelOverlay::Open(a, LevelOverlayInfo{.Source = level, .Region = pip});
        }
        else if (frame == 3)
        {
            // The primary and the PiP overlay both produced live outputs this frame, the overlay
            // placed at its fixed sub-region over the full-window primary — PiP over a live primary.
            CHECK(a.GetManagedViewports().Get(0)->GetOutput() != nullptr);
            CHECK(a.GetManagedViewports().Get(0)->GetOutputHandle().IsValid());
            CHECK(a.A->GetViewport().GetOutput() != nullptr);
            CHECK(a.A->GetViewport().GetOutputHandle().IsValid());
            CHECK(a.A->GetViewport().GetRegion().Offset == pip.Offset);
            CHECK(a.A->GetViewport().GetRegion().Extent == pip.Extent);
            a.A.reset();
        }
    };

    app.Frames = 5;
    app.Run({});
}

TEST_CASE("Tearing down the Context with an overlay world still open retires cleanly")
{
    // The two-hop WorldRunner -> World -> overlay-viewport ownership chain at teardown: the overlay
    // is opened and never explicitly closed mid-run, so it is still open when OnDispose drops it
    // during Run()'s teardown (the runner and managed set reset before the Context disposes). Its
    // viewport must retire against the still-live viewport registry and its world drop cleanly.
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        if (frame == 0)
        {
            a.A = LevelOverlay::Open(a, LevelOverlayInfo{.Source = level});
            CHECK(a.A->GetWorld().IsValid());
            // Left open deliberately: OnDispose (not this step) closes it during teardown.
        }
    };

    app.Frames = 3;
    app.Run({});

    // Reaching here (ASan-clean) is the assertion: the open overlay tore down in teardown order.
    CHECK(true);
}
