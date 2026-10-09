// LevelOverlay: a whole Level opened over a running world because a component asked for it.
//
// Drives a real headless Application (its own Context, no window, no ImGui) through Run(), adding
// and removing LevelOverlay components on entities of an opener world from OnUpdate; the engine
// opens and closes the overlay worlds at the next frame-top reconcile. Each overlay creates a
// Presented viewport, so the suite is GPU-band (it needs a Context, though most assertions are
// router, world and scene state, not pixels); the gpu/main.cpp harness skips the band with no ICD.
//
// It pins: a request opening an overlay (publishing LevelOverlayState) and its removal closing it
// with the cursor seat and the opener's pause restored; an overlay following its opener's world
// closed; an overlay ending itself through its own ExitRequest, its request taken away; the seed
// and the application's load hook landing before the overlay starts; Opaque covering the viewports
// presenting the opener's world; stacked overlays suspending each other and unwinding in reverse
// open order, or handing on when the lower one goes first; the overlay seat's pawn marked locally
// controlled; the layout resolving to pixels; an overlay rendering with no per-frame game call; and
// a clean teardown with an overlay still open.

#include <doctest/doctest.h>

#include <Veng/Application.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/LevelOverlay.h>
#include <Veng/ManagedViewports.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>
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
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

#include "support/TestServices.h"

using namespace Veng;

namespace
{
    // The action + key the input-suspension checks resolve against.
    constexpr ActionId Move{0xA1};
    constexpr u32 KeyW = u32(Key::W);

    // What the overlay's OnStart saw: the seed's copied Name and Possesses, and the load hook's mark.
    struct StartSeen
    {
        int Starts = 0;
        bool Seeded = false;
        bool SeedReferenceCleared = false;
        bool Loaded = false;
    };
    StartSeen g_Seen;

    struct StartProbe final : SceneSystem
    {
        void OnStart(Scene& scene, const SystemContext&) override
        {
            ++g_Seen.Starts;
            for (auto [entity, name] : scene.View<Name>())
            {
                if (name.Value == "seeded")
                {
                    g_Seen.Seeded = true;
                    const auto* possesses = std::as_const(scene).TryGet<Possesses>(entity);
                    g_Seen.SeedReferenceCleared =
                        possesses != nullptr && possesses->Pawn == Entity::Null;
                }
                g_Seen.Loaded |= name.Value == "loaded";
            }
        }
        void OnUpdate(Scene&, f32, const SystemContext&) override {}
    };
}

namespace Veng
{
    template <>
    struct VengSystem<StartProbe>
    {
        static constexpr SystemId Id = 0x0AE12A0000000001ULL;
        static string Name() { return "StartProbe"; }
    };
}

namespace
{
    // A resident W -> Move.y context.
    AssetHandle<InputMappingContext> MakeMoveContext(const AssetManager& assets)
    {
        Ref<InputMappingContext> resource = InputMappingContext::Create(
            {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D}},
            {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                     .Action = Move,
                     .Axis = AxisComponent::Y,
                     .Scale = 1.0f}});
        return assets.Adopt<InputMappingContext>(std::move(resource));
    }

    // Serializes one component into a prefab record.
    template <typename T>
    Prefab::Component Comp(const T& value, const TypeRegistry& types)
    {
        Prefab::Component component;
        component.Type = types.IdOf<T>();
        WriteFields(component.Record, &value, types.Info(component.Type), types);
        return component;
    }

    // A level over a one-seat world prefab: an authored input seat plus a Transform. `leadingDummies`
    // pads the prefab before the seat, so two levels built here resolve to distinct seat entities.
    AssetHandle<Level> BuildSeatLevel(const AssetManager& assets, const TypeRegistry& types,
                                      vector<SystemId> systems, int leadingDummies = 0)
    {
        vector<Prefab::PrefabEntity> entities;
        for (int i = 0; i < leadingDummies; ++i)
        {
            entities.push_back({.Components = {Comp(Name{"dummy"}, types)}});
        }
        entities.push_back(
            {.Components = {
                 Comp(Viewer{}, types),
                 Comp(InputContextStack{}, types),
                 Comp(PlayerInput{}, types),
                 Comp(SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None}, types),
                 Comp(Transform{}, types),
             }});
        const AssetHandle<Prefab> world =
            assets.Adopt<Prefab>(Prefab::Create(std::move(entities), {}));
        return assets.Adopt<Level>(
            Level::Create(world, std::move(systems), GameModeConfig{}, RenderLook{}));
    }

    // A headless application driven by two closures, holding one opener world the cases put their
    // requests in. Its OnOverlayLoaded runs LoadedFn when set.
    class OverlayApp final : public Application
    {
    public:
        using Application::Application;

        function<void(OverlayApp&)> InitFn;
        function<void(OverlayApp&, int)> StepFn;
        function<void(Scene&)> LoadedFn;
        int Frames = 6;
        int Current = 0;
        WorldInstanceId Opener;

        Scene& OpenerScene() { return GetWorldRunner().ResolveWorld(Opener)->GetScene(); }

        // Adds a request on a fresh entity of the opener world.
        Entity Request(const LevelOverlay& request)
        {
            Scene& scene = OpenerScene();
            const Entity entity = scene.CreateEntity();
            scene.Add<LevelOverlay>(entity, request);
            return entity;
        }

        const LevelOverlayState* StateOf(Entity entity)
        {
            return std::as_const(OpenerScene()).TryGet<LevelOverlayState>(entity);
        }

        bool HasRequest(Entity entity)
        {
            return std::as_const(OpenerScene()).TryGet<LevelOverlay>(entity) != nullptr;
        }

        // The open overlay's scene; the request must have opened.
        Scene& OverlayScene(Entity entity)
        {
            const LevelOverlayState* state = StateOf(entity);
            REQUIRE(state != nullptr);
            return GetWorldRunner().ResolveWorld(state->World)->GetScene();
        }

        // The open overlay's viewport, disabled so a case pinning state renders nothing.
        Renderer::Viewport& Quiet(Entity entity)
        {
            const LevelOverlayState* state = StateOf(entity);
            REQUIRE(state != nullptr);
            Renderer::Viewport* viewport = FindOverlayViewport(state->World);
            REQUIRE(viewport != nullptr);
            viewport->SetEnabled(false);
            return *viewport;
        }

    protected:
        void OnInitialize() override
        {
            // The opener: an empty world with a simulation, so it can be paused.
            Opener = GetWorldRunner().OpenWorld(WorldOpenInfo{.Systems = vector<SystemId>{}});
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

        void OnOverlayLoaded(WorldInstanceId, Entity, WorldInstanceId, Scene& scene) override
        {
            if (LoadedFn)
            {
                LoadedFn(scene);
            }
        }
    };

    ApplicationInfo HeadlessInfo()
    {
        ApplicationInfo info;
        info.Name = "veng-level-overlay-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        info.HeadlessExtent = {320, 240};
        return info;
    }

    // A seat's Move.y under a held-W snapshot: 1 when its context resolves the binding, 0 when it
    // is suspended (its contexts swapped to the empty context).
    f32 ResolveMoveY(AssetManager& assets, Scene& scene)
    {
        Input input(nullptr);
        input.BeginFrame();
        input.ApplyEvent(KeyPressedEvent{Key::W, 0, 0});

        TestSupport::TestServices services({.Assets = &assets, .Input = &input});
        InputMappingSystem mapping;
        mapping.OnUpdate(scene, 0.016f, services.Make());

        const InputSeat seat = ResolveInputSeat(&scene, {});
        return scene.Get<PlayerInput>(seat.Viewer).GetValue(Move).y;
    }
}

TEST_CASE("A LevelOverlay opens an overlay, and removing it closes it and restores the opener")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;
    Entity request = Entity::Null;
    SeatRef priorCursor;
    WorldInstanceId overlay;
    bool openedAfterOneFrame = false;
    bool closedWorld = false;
    bool cursorRestored = false;
    bool pausedWhileOpen = false;
    bool resumedOnClose = false;
    bool stateCleared = false;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        const InputRouter& router = a.GetInputRouter();
        if (frame == 0)
        {
            priorCursor = router.GetCursorSeat();
            request = a.Request(LevelOverlay{.Source = level, .PauseOpener = true});
        }
        else if (frame == 1)
        {
            const LevelOverlayState* state = a.StateOf(request);
            openedAfterOneFrame = state != nullptr && state->World.IsValid() &&
                                  a.GetWorldRunner().ResolveWorld(state->World) != nullptr &&
                                  router.GetCursorSeat() == state->Seat &&
                                  !state->Seat.IsImplicit();
            overlay = state != nullptr ? state->World : WorldInstanceId{};
            a.Quiet(request);
            pausedWhileOpen = a.IsWorldPaused(a.Opener);
            (void)a.OpenerScene().Remove<LevelOverlay>(request);
        }
        else if (frame == 2)
        {
            closedWorld = a.GetWorldRunner().ResolveWorld(overlay) == nullptr &&
                          a.FindOverlayViewport(overlay) == nullptr;
            cursorRestored = router.GetCursorSeat() == priorCursor;
            resumedOnClose = !a.IsWorldPaused(a.Opener);
            stateCleared = a.StateOf(request) == nullptr;
        }
    };

    app.Frames = 4;
    app.Run({});

    CHECK(openedAfterOneFrame);
    CHECK(pausedWhileOpen);
    CHECK(closedWorld);
    CHECK(cursorRestored);
    CHECK(resumedOnClose);
    CHECK(stateCleared);
}

TEST_CASE("An overlay closes with its opener's world, leaving no viewport, focus or cursor behind")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;
    Entity request = Entity::Null;
    SeatRef priorCursor;
    bool closedSameFrame = false;
    bool viewportGone = false;
    bool cursorRestored = false;
    bool pointerFreed = false;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        const InputRouter& router = a.GetInputRouter();
        if (frame == 0)
        {
            priorCursor = router.GetCursorSeat();
            request = a.Request(LevelOverlay{.Source = level, .PauseOpener = true});
        }
        else if (frame == 1)
        {
            const WorldInstanceId overlay = a.StateOf(request)->World;
            const Renderer::ViewportId viewport = a.Quiet(request).GetId();
            a.GetWorldRunner().CloseWorld(a.Opener);

            closedSameFrame = a.GetWorldRunner().ResolveWorld(overlay) == nullptr &&
                              a.FindOverlayViewport(overlay) == nullptr;
            viewportGone = a.GetRenderContext().GetViewportRegistry().Resolve(viewport) == nullptr;
            cursorRestored = router.GetCursorSeat() == priorCursor;
            pointerFreed =
                router.ResolvePointer(ivec2(100, 100), false, Entity::Null).Owner == Entity::Null;
        }
    };

    app.Frames = 3;
    app.Run({});

    CHECK(closedSameFrame);
    CHECK(viewportGone);
    CHECK(cursorRestored);
    CHECK(pointerFreed);
}

TEST_CASE("An overlay's ExitRequest closes it and takes its request away, not the application")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;
    Entity request = Entity::Null;
    WorldInstanceId overlay;
    bool closed = false;
    bool requestRemoved = false;
    bool stillClosed = false;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        if (frame == 0)
        {
            request = a.Request(LevelOverlay{.Source = level});
        }
        else if (frame == 1)
        {
            overlay = a.StateOf(request)->World;
            a.Quiet(request);
            Scene& scene = a.OverlayScene(request);
            scene.Add<ExitRequest>(scene.CreateEntity());
        }
        else if (frame == 2)
        {
            closed = a.GetWorldRunner().ResolveWorld(overlay) == nullptr &&
                     a.StateOf(request) == nullptr;
            requestRemoved = !a.HasRequest(request);
        }
        else if (frame == 3)
        {
            // Nothing reopened it.
            stillClosed =
                a.StateOf(request) == nullptr && a.GetWorldRunner().GetWorlds().size() == 1;
        }
    };

    app.Frames = 5;
    app.Run({});

    CHECK(closed);
    CHECK(requestRemoved);
    CHECK(stillClosed);
    CHECK(app.Current == app.Frames); // the application ran on to its own exit
}

TEST_CASE("The seed and the load hook land in the overlay scene before it starts")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<StartProbe>();
    g_Seen = {};

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;
    Entity request = Entity::Null;
    bool requestNotCopied = false;

    app.InitFn = [&](OverlayApp& a)
    {
        level =
            BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {SystemIdOf<StartProbe>()});
    };
    app.LoadedFn = [](Scene& scene) { scene.Add<Name>(scene.CreateEntity(), Name{"loaded"}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        if (frame == 0)
        {
            // The request is its own seed; its Possesses names an opener entity, which the copy clears.
            Scene& opener = a.OpenerScene();
            request = opener.CreateEntity();
            opener.Add<Name>(request, Name{"seeded"});
            opener.Add<Possesses>(request).Pawn = opener.CreateEntity();
            opener.Add<LevelOverlay>(request, LevelOverlay{.Source = level, .Seed = request});
        }
        else if (frame == 1)
        {
            a.Quiet(request);
            requestNotCopied = a.OverlayScene(request).TryGetFirst<LevelOverlay>() == nullptr;
        }
    };

    app.Frames = 3;
    app.Run({});

    CHECK(g_Seen.Starts == 1);
    CHECK(g_Seen.Seeded);
    CHECK(g_Seen.SeedReferenceCleared);
    CHECK(g_Seen.Loaded);
    CHECK(requestNotCopied);
}

TEST_CASE("An opaque overlay disables the viewports presenting its opener's world while open")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    ApplicationInfo info = HeadlessInfo();
    info.ManagedViewport = ManagedViewportInfo{};
    OverlayApp app(info, types, systems);
    AssetHandle<Level> level;
    Entity request = Entity::Null;
    bool enabledBefore = false;
    bool disabledWhileOpen = false;
    bool overlayEnabled = false;
    bool enabledAfter = false;

    app.InitFn = [&](OverlayApp& a)
    {
        level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {});
        a.GetManagedViewports().SetViewportWorld(0, a.Opener);
    };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        const Renderer::Viewport& beneath = *a.GetManagedViewports().Get(0);
        if (frame == 0)
        {
            enabledBefore = beneath.IsEnabled();
            request = a.Request(LevelOverlay{.Source = level, .Opaque = true});
        }
        else if (frame == 1)
        {
            disabledWhileOpen = !beneath.IsEnabled();
            overlayEnabled = a.FindOverlayViewport(a.StateOf(request)->World)->IsEnabled();
            a.Quiet(request);
            (void)a.OpenerScene().Remove<LevelOverlay>(request);
        }
        else if (frame == 2)
        {
            enabledAfter = beneath.IsEnabled();
        }
    };

    app.Frames = 4;
    app.Run({});

    CHECK(enabledBefore);
    CHECK(disabledWhileOpen);
    CHECK(overlayEnabled);
    CHECK(enabledAfter);
}

TEST_CASE("Stacked overlays suspend the one beneath and unwind in reverse open order")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> levelA;
    AssetHandle<Level> levelB;
    AssetHandle<InputMappingContext> moveContext;
    Entity lower = Entity::Null;
    Entity upper = Entity::Null;
    SeatRef priorCursor;
    SeatRef seatB;

    f32 lowerMove = -1.0f;
    f32 upperMove = -1.0f;
    bool cursorOnUpper = false;
    bool distinctSeats = false;
    bool bothClosed = false;
    bool restoredToOriginal = false;
    bool pausedUnderBoth = false;
    bool resumed = false;

    // Out of order: the lower one closes first, and the upper keeps the cursor and its suspension.
    bool upperKeepsCursor = false;
    bool restoredAfterUpper = false;

    app.InitFn = [&](OverlayApp& self)
    {
        levelA = BuildSeatLevel(self.GetAssetManager(), self.GetTypeRegistry(), {}, 0);
        levelB = BuildSeatLevel(self.GetAssetManager(), self.GetTypeRegistry(), {}, 1);
        moveContext = MakeMoveContext(self.GetAssetManager());
    };

    app.StepFn = [&](OverlayApp& self, int frame)
    {
        const InputRouter& router = self.GetInputRouter();
        AssetManager& assets = self.GetAssetManager();
        const auto giveMove = [&](Entity request)
        {
            Scene& scene = self.OverlayScene(request);
            const LevelOverlayState* state = self.StateOf(request);
            scene.Get<InputContextStack>(state->Seat.Viewer).Active = {moveContext};
        };

        if (frame == 0)
        {
            priorCursor = router.GetCursorSeat();
            lower = self.Request(LevelOverlay{.Source = levelA, .PauseOpener = true});
        }
        else if (frame == 1)
        {
            self.Quiet(lower);
            giveMove(lower);
            upper = self.Request(LevelOverlay{.Source = levelB, .PauseOpener = true});
        }
        else if (frame == 2)
        {
            self.Quiet(upper);
            giveMove(upper);
            seatB = self.StateOf(upper)->Seat;
            distinctSeats = !(self.StateOf(lower)->Seat == seatB);
            cursorOnUpper = router.GetCursorSeat() == seatB;
            lowerMove = ResolveMoveY(assets, self.OverlayScene(lower));
            upperMove = ResolveMoveY(assets, self.OverlayScene(upper));
            pausedUnderBoth = self.IsWorldPaused(self.Opener);

            (void)self.OpenerScene().Remove<LevelOverlay>(lower);
            (void)self.OpenerScene().Remove<LevelOverlay>(upper);
        }
        else if (frame == 3)
        {
            bothClosed = self.StateOf(lower) == nullptr && self.StateOf(upper) == nullptr;
            restoredToOriginal = router.GetCursorSeat() == priorCursor;
            resumed = !self.IsWorldPaused(self.Opener);

            lower = self.Request(LevelOverlay{.Source = levelA});
        }
        else if (frame == 4)
        {
            self.Quiet(lower);
            upper = self.Request(LevelOverlay{.Source = levelB});
        }
        else if (frame == 5)
        {
            self.Quiet(upper);
            seatB = self.StateOf(upper)->Seat;
            (void)self.OpenerScene().Remove<LevelOverlay>(lower);
        }
        else if (frame == 6)
        {
            upperKeepsCursor = self.StateOf(lower) == nullptr && router.GetCursorSeat() == seatB;
            (void)self.OpenerScene().Remove<LevelOverlay>(upper);
        }
        else if (frame == 7)
        {
            restoredAfterUpper = router.GetCursorSeat() == priorCursor;
        }
    };

    app.Frames = 9;
    app.Run({});

    CHECK(distinctSeats);
    CHECK(cursorOnUpper);
    CHECK(lowerMove == doctest::Approx(0.0f));
    CHECK(upperMove == doctest::Approx(1.0f));
    CHECK(pausedUnderBoth);
    CHECK(bothClosed);
    CHECK(restoredToOriginal);
    CHECK(resumed);
    CHECK(upperKeepsCursor);
    CHECK(restoredAfterUpper);
}

TEST_CASE("An overlay's seat marks the pawn it possesses locally controlled")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;
    Entity request = Entity::Null;
    Entity pawn = Entity::Null;
    bool marked = false;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };
    app.LoadedFn = [&pawn](Scene& scene)
    {
        pawn = scene.CreateEntity();
        for (auto [seat, viewer] : scene.View<Viewer>())
        {
            scene.Add<Possesses>(seat).Pawn = pawn;
            break;
        }
    };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        if (frame == 0)
        {
            request = a.Request(LevelOverlay{.Source = level});
        }
        else if (frame == 1)
        {
            a.Quiet(request);
        }
        else if (frame == 2)
        {
            const auto* control = std::as_const(a.OverlayScene(request)).TryGet<LocalControl>(pawn);
            marked = control != nullptr && control->Seat == a.StateOf(request)->Seat.Viewer;
        }
    };

    app.Frames = 4;
    app.Run({});

    CHECK(marked);
}

TEST_CASE("An overlay's layout resolves to its pixel region, and it renders with no game call")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    OverlayApp app(HeadlessInfo(), types, systems);
    AssetHandle<Level> level;
    Entity full = Entity::Null;
    Entity corner = Entity::Null;
    uvec2 fullExtent{};
    Renderer::ViewportRegion cornerRegion;
    bool tracks = false;
    bool rendered = false;

    app.InitFn = [&](OverlayApp& a)
    { level = BuildSeatLevel(a.GetAssetManager(), a.GetTypeRegistry(), {}); };

    app.StepFn = [&](OverlayApp& a, int frame)
    {
        if (frame == 0)
        {
            full = a.Request(LevelOverlay{.Source = level});
            corner = a.Request(LevelOverlay{
                .Source = level,
                .Layout = {.Offset = {0.5f, 0.5f}, .Extent = {0.25f, 0.5f}},
            });
        }
        else if (frame == 1)
        {
            a.Quiet(corner);
            const Renderer::Viewport& viewport = *a.FindOverlayViewport(a.StateOf(full)->World);
            fullExtent = viewport.GetRegion().Extent;
            cornerRegion = a.FindOverlayViewport(a.StateOf(corner)->World)->GetRegion();
            tracks = viewport.GetLayout().has_value();
        }
        else if (frame == 3)
        {
            const Renderer::Viewport& viewport = *a.FindOverlayViewport(a.StateOf(full)->World);
            rendered = viewport.GetOutput() != nullptr && viewport.GetOutputHandle().IsValid();
        }
    };

    app.Frames = 5;
    app.Run({});

    CHECK(fullExtent == uvec2(320, 240));
    CHECK(cornerRegion.Offset == ivec2(160, 120));
    CHECK(cornerRegion.Extent == uvec2(80, 120));
    CHECK(tracks);
    CHECK(rendered);
}

TEST_CASE("An overlay still open at exit closes with the worlds and tears down cleanly")
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
            a.Request(LevelOverlay{.Source = level});
        }
    };

    app.Frames = 3;
    app.Run({});

    // Reaching here (ASan-clean) is the assertion: the open overlay unwound in the shutdown sweep.
    CHECK(app.GetWorldRunner().GetWorlds().empty());
}
