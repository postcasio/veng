// Per-seat focus, the consumer registry, and SeatFocusScope, all headless (no window, no ICD).
// The router's focus is now per-seat and token-identified: a seat's focus top decides where that
// seat's devices route, popping is by token so an out-of-order pop is safe, ImGui-style consumers
// register in priority order and the first to accept an event stops the fall-through, and the
// cursor capture / consumer signal derives from the single keyboard/mouse (cursor) seat's focus.
// SeatFocusScope bundles the takeover (push token + swap contexts + associate viewport) and
// restores it in inverse order. ResolveInputSeat finds the first locally-owned seat, null-safe.

#include <doctest/doctest.h>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/InputEvents.h>
#include <Veng/Input/InputConsumer.h>
#include <Veng/WindowEvents.h>
#include <Veng/Input/SeatFocusScope.h>
#include <Veng/InputRouter.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;

namespace
{
    // Two distinct seat entities standing in for two Viewer seats.
    constexpr Entity SeatA{.Index = 1, .Generation = 1};
    constexpr Entity SeatB{.Index = 2, .Generation = 1};

    // A consumer that records every forwarded event's type and every cursor-capture signal, and
    // optionally hard-consumes (stops fall-through) to prove registry priority order.
    struct RecordingConsumer final : InputConsumer
    {
        vector<EventType> Forwarded;
        vector<bool> CaptureSignals;
        bool Consume = false;

        bool ForwardEvent(const Event& event) override
        {
            Forwarded.push_back(event.GetEventType());
            return Consume;
        }

        void OnCursorCaptured(bool captured) override { CaptureSignals.push_back(captured); }
    };

    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        return registry;
    }

    // A resident AssetHandle<InputMappingContext> carrying the given non-zero id, built without an
    // AssetManager: a detached cache entry wired into a type-erased handle the way a prefab spawn
    // rehydrates one. The scope reads the id's validity to decide whether to swap, so a manufactured
    // non-zero id is enough.
    AssetHandle<InputMappingContext> MakeContext(u64 id)
    {
        const Ref<InputMappingContext> resource = InputMappingContext::Create({}, {});
        auto entry = CreateRef<Detail::AssetCacheEntry>(
            Detail::AssetCacheEntry{.Id = AssetId{.Value = id},
                                    .Type = AssetTypes::InputMap,
                                    .Resource = Detail::RefAny(resource)});

        AssetHandle<InputMappingContext> handle;
        Detail::RehydrateHandleField(&handle, AssetId{.Value = id}, std::move(entry));
        return handle;
    }

    // Action id and key for the two-seat resolution fixture.
    constexpr ActionId Move{0xA1};
    constexpr u32 KeyW = u32(Key::W);

    // A resident W → Move.y context, so a seat holding it resolves Move.y = 1 while W is held.
    AssetHandle<InputMappingContext> MakeWMoveContext(u64 id)
    {
        const Ref<InputMappingContext> resource = InputMappingContext::Create(
            {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D}},
            {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                     .Action = Move,
                     .Axis = AxisComponent::Y,
                     .Scale = 1.0f}});
        auto entry = CreateRef<Detail::AssetCacheEntry>(
            Detail::AssetCacheEntry{.Id = AssetId{.Value = id},
                                    .Type = AssetTypes::InputMap,
                                    .Resource = Detail::RefAny(resource)});
        AssetHandle<InputMappingContext> handle;
        Detail::RehydrateHandleField(&handle, AssetId{.Value = id}, std::move(entry));
        return handle;
    }

    // A SystemContext over the given headless Input and never-dereferenced asset storage; the
    // InputMappingSystem reads only the Input and the pointer routing (the seat_routing idiom).
    struct ContextStorage
    {
        const Input& HeadlessInput;
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(TasksBytes),
            };
        }
    };
}

TEST_CASE("Per-seat focus: each seat's stack top is independent")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    // Both seats default to UI (an absent stack is UI).
    CHECK(router.GetFocus(SeatA) == InputFocus::UI);
    CHECK(router.GetFocus(SeatB) == InputFocus::UI);

    // Seat A takes gameplay focus; seat B is untouched — split-screen A plays, B stays UI.
    const FocusToken a = router.PushFocus(SeatA, InputFocus::Gameplay);
    CHECK(router.IsGameplayFocused(SeatA));
    CHECK_FALSE(router.IsGameplayFocused(SeatB));

    // Seat B takes UI focus on top of its (empty) stack; seat A is still gameplay.
    const FocusToken b = router.PushFocus(SeatB, InputFocus::UI);
    CHECK(router.IsGameplayFocused(SeatA));
    CHECK(router.GetFocus(SeatB) == InputFocus::UI);

    router.PopFocus(a);
    router.PopFocus(b);
    CHECK(router.GetFocus(SeatA) == InputFocus::UI);
    CHECK(router.GetFocus(SeatB) == InputFocus::UI);
}

TEST_CASE("Focus tokens: an out-of-order pop removes its own entry")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    // Two entries on seat A: UI beneath, Gameplay on top.
    const FocusToken lower = router.PushFocus(SeatA, InputFocus::UI);
    const FocusToken upper = router.PushFocus(SeatA, InputFocus::Gameplay);
    CHECK(router.IsGameplayFocused(SeatA));

    // Pop the LOWER (interleaved) entry out of order: the top is still Gameplay, and the entry
    // removed is the one the token names, not whoever is on top.
    router.PopFocus(lower);
    CHECK(router.IsGameplayFocused(SeatA));

    // Popping the upper now empties the stack back to UI.
    router.PopFocus(upper);
    CHECK(router.GetFocus(SeatA) == InputFocus::UI);
}

TEST_CASE("The consumer registry offers UI-owned events in registration order")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    RecordingConsumer first;
    RecordingConsumer second;
    router.RegisterConsumer(first);
    router.RegisterConsumer(second);

    // The cursor seat defaults to UI, so a drained key event is offered to the consumers and folds
    // into the snapshot.
    input.BeginFrame();
    KeyPressedEvent press(Key::W, 0, 0);
    router.Dispatch(press);

    // Both cooperative consumers saw it, first before second, and the snapshot mirrored it.
    CHECK(first.Forwarded.size() == 1);
    CHECK(second.Forwarded.size() == 1);
    CHECK(input.IsKeyDown(Key::W));
}

TEST_CASE("The first consumer to accept an event stops the fall-through")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    RecordingConsumer first;
    RecordingConsumer second;
    first.Consume = true; // the first registered hard-consumes
    router.RegisterConsumer(first);
    router.RegisterConsumer(second);

    input.BeginFrame();
    KeyPressedEvent press(Key::W, 0, 0);
    router.Dispatch(press);

    // The first consumed it, so the second never saw it; the snapshot fold is independent and still
    // mirrored the event.
    CHECK(first.Forwarded.size() == 1);
    CHECK(second.Forwarded.empty());
    CHECK(input.IsKeyDown(Key::W));
}

TEST_CASE("Under gameplay focus the cursor seat starves the consumers, snapshot still folds")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    RecordingConsumer consumer;
    router.RegisterConsumer(consumer);

    // The cursor seat (default Entity::Null) takes gameplay focus: an input event is swallowed from
    // the consumers but still reaches the snapshot.
    router.PushFocus(InputFocus::Gameplay);
    input.BeginFrame();
    KeyPressedEvent press(Key::Space, 0, 0);
    router.Dispatch(press);

    CHECK(consumer.Forwarded.empty());
    CHECK(input.IsKeyDown(Key::Space));

    // A window/system event (not an input event) still reaches the consumers regardless of focus.
    consumer.Forwarded.clear();
    WindowResizeEvent resize(800, 600);
    router.Dispatch(resize);
    CHECK(consumer.Forwarded.size() == 1);
}

TEST_CASE("Cursor capture derivation follows the cursor seat's focus top")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    RecordingConsumer consumer;
    router.RegisterConsumer(consumer);

    // Designate seat A the cursor seat: its focus top drives the capture signal to the consumers.
    router.SetCursorSeat(SeatA);
    REQUIRE(consumer.CaptureSignals.size() == 1);
    CHECK(consumer.CaptureSignals.back() == false); // UI top: free cursor

    // Seat A takes gameplay: the consumers are signalled captured.
    const FocusToken a = router.PushFocus(SeatA, InputFocus::Gameplay);
    CHECK(consumer.CaptureSignals.back() == true);

    // A push on a NON-cursor seat does not re-derive the capture (seat B is not the cursor seat).
    const usize before = consumer.CaptureSignals.size();
    const FocusToken b = router.PushFocus(SeatB, InputFocus::Gameplay);
    CHECK(consumer.CaptureSignals.size() == before);

    // Leaving seat A's gameplay frees the cursor again.
    router.PopFocus(a);
    CHECK(consumer.CaptureSignals.back() == false);
    router.PopFocus(b);
}

TEST_CASE("ResolveInputSeat returns the first locally-owned seat, null-safe before the world")
{
    // A null scene resolves an empty seat.
    const InputSeat none = ResolveInputSeat(nullptr);
    CHECK(none.Viewer == Entity::Null);
    CHECK(none.World == nullptr);
    CHECK(none.ResolveContexts() == nullptr);

    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A scene with no seat resolves empty.
    CHECK(ResolveInputSeat(scene.get()).Viewer == Entity::Null);

    // A full (Viewer, InputContextStack, PlayerInput) seat resolves with a borrowed context stack.
    const Entity seat = scene->CreateEntity();
    scene->Add<Viewer>(seat);
    scene->Add<InputContextStack>(seat);
    scene->Add<PlayerInput>(seat);

    const InputSeat resolved = ResolveInputSeat(scene.get());
    CHECK(resolved.Viewer == seat);
    CHECK(resolved.World == scene.get());
    CHECK(resolved.ResolveContexts() == &scene->Get<InputContextStack>(seat));
}

TEST_CASE("SeatFocusScope round-trips push + swap + associate, restoring in inverse order")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity seatEntity = scene->CreateEntity();
    scene->Add<Viewer>(seatEntity);
    scene->Add<PlayerInput>(seatEntity);

    // Seed a gameplay context so the swap has something to suspend.
    const AssetHandle<InputMappingContext> gameplayContext = MakeContext(0xAA11);
    auto& stack = scene->Add<InputContextStack>(seatEntity);
    stack.Active.push_back(gameplayContext);

    const InputSeat seat = ResolveInputSeat(scene.get());
    REQUIRE(seat.Viewer == seatEntity);

    // A distinct non-zero UI context so the swap replaces the gameplay contexts.
    const AssetHandle<InputMappingContext> uiContext = MakeContext(0xBB22);

    {
        // Open the takeover. No viewport here (a real Viewport needs a Context); the push + swap are
        // what this case pins.
        const SeatFocusScope scope(router, seat, nullptr, uiContext);

        // (a) A UI focus entry is on the seat's stack.
        CHECK(router.GetFocus(seatEntity) == InputFocus::UI);

        // (b) The seat's contexts are the UI context alone — the gameplay context is suspended.
        const InputContextStack& active = scene->Get<InputContextStack>(seatEntity);
        REQUIRE(active.Active.size() == 1);
        CHECK(active.Active[0].Id().Value == 0xBB22);

        // Push an unrelated entry ABOVE the scope's, so the scope's is not on top at destruction —
        // the token pop must still remove the scope's own entry.
        const FocusToken above = router.PushFocus(seatEntity, InputFocus::Gameplay);
        CHECK(router.IsGameplayFocused(seatEntity));
        router.PopFocus(above);
    }

    // After destruction the stack is back to UI (the scope popped its own token) and the seat's
    // gameplay context is restored in place.
    CHECK(router.GetFocus(seatEntity) == InputFocus::UI);
    const InputContextStack& restored = scene->Get<InputContextStack>(seatEntity);
    REQUIRE(restored.Active.size() == 1);
    CHECK(restored.Active[0].Id().Value == 0xAA11);
}

TEST_CASE("A SeatFocusScope suspends its seat's gameplay resolution, the other seat plays on")
{
    // Two full seats, each holding the same W → Move.y context; both hold the keyboard/mouse so
    // both resolve it while W is held. A SeatFocusScope over seat A suspends A's contexts (its
    // PlayerInput stops resolving Move), while seat B keeps resolving untouched — the split-screen
    // guarantee: A's menu leaves B playing.
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const auto MakeSeat = [&](u64 contextId)
    {
        const Entity seat = scene->CreateEntity();
        scene->Add<Viewer>(seat);
        scene->Add<PlayerInput>(seat);
        scene->Add<SeatInput>(seat,
                              SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None});
        auto& stack = scene->Add<InputContextStack>(seat);
        stack.Active.push_back(MakeWMoveContext(contextId));
        return seat;
    };
    const Entity seatA = MakeSeat(0xC001);
    const Entity seatB = MakeSeat(0xC002);

    Input input(nullptr);
    input.BeginFrame();
    input.ApplyEvent(KeyPressedEvent{Key::W, 0, 0});

    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);
    InputMappingSystem mapping;
    ContextStorage storage{.HeadlessInput = input};

    // Baseline: both seats resolve Move.y = 1 from the held W.
    mapping.OnUpdate(*scene, 0.016f, storage.Make());
    CHECK(scene->Get<PlayerInput>(seatA).GetValue(Move).y == doctest::Approx(1.0f));
    CHECK(scene->Get<PlayerInput>(seatB).GetValue(Move).y == doctest::Approx(1.0f));

    {
        // Open a UI takeover on seat A with no swap-in context: the scope pushes UI focus and
        // suspends A's gameplay contexts, so A resolves to neutral while B is unaffected.
        const InputSeat seat{.Viewer = seatA, .World = scene.get()};
        const SeatFocusScope scope(router, seat, nullptr, MakeContext(0xD001));

        input.BeginFrame();
        input.ApplyEvent(KeyPressedEvent{Key::W, 0, 0});
        mapping.OnUpdate(*scene, 0.016f, storage.Make());

        // Seat A swapped to the empty UI context, so its Move no longer resolves; seat B still does.
        CHECK(scene->Get<PlayerInput>(seatA).GetValue(Move).y == doctest::Approx(0.0f));
        CHECK(scene->Get<PlayerInput>(seatB).GetValue(Move).y == doctest::Approx(1.0f));
        CHECK(router.IsGameplayFocused(seatB) == false);
    }

    // The scope closed: seat A's gameplay context is restored, so it resolves Move again.
    input.BeginFrame();
    input.ApplyEvent(KeyPressedEvent{Key::W, 0, 0});
    mapping.OnUpdate(*scene, 0.016f, storage.Make());
    CHECK(scene->Get<PlayerInput>(seatA).GetValue(Move).y == doctest::Approx(1.0f));
}

TEST_CASE("A SeatFocusScope restores through a re-resolve after a structural change moved the pool")
{
    // The re-resolve guard: a scope suspends a seat's contexts, then the scene undergoes a
    // structural change that reallocates the InputContextStack pool while the scope is open. A
    // cached borrowed pointer would dangle; re-resolving through the (World, Viewer) identity at
    // restore time finds the live pool and swaps the gameplay context back correctly.
    Input input(nullptr);
    const Renderer::ViewportRegistry viewportRegistry;
    InputRouter router(nullptr, input, viewportRegistry);

    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity seatEntity = scene->CreateEntity();
    scene->Add<Viewer>(seatEntity);
    scene->Add<PlayerInput>(seatEntity);
    auto& stack = scene->Add<InputContextStack>(seatEntity);
    stack.Active.push_back(MakeContext(0xAA11));

    const InputSeat seat = ResolveInputSeat(scene.get());
    REQUIRE(seat.Viewer == seatEntity);

    {
        const SeatFocusScope scope(router, seat, nullptr, MakeContext(0xBB22));

        // While the scope holds the seat, grow the InputContextStack pool with many more
        // components — a structural change that reallocates the dense storage the seat's stack
        // lived in, so a raw pointer captured at open would now dangle.
        for (int i = 0; i < 64; ++i)
        {
            const Entity other = scene->CreateEntity();
            scene->Add<InputContextStack>(other);
        }

        // The suspended seat still reads the UI context through a fresh resolve.
        const InputContextStack* live = seat.ResolveContexts();
        REQUIRE(live != nullptr);
        REQUIRE(live->Active.size() == 1);
        CHECK(live->Active[0].Id().Value == 0xBB22);
    }

    // The scope closed after the pool moved: the gameplay context is restored into the live pool,
    // proving the restore re-resolved rather than writing through a stale pointer.
    const InputContextStack& restored = scene->Get<InputContextStack>(seatEntity);
    REQUIRE(restored.Active.size() == 1);
    CHECK(restored.Active[0].Id().Value == 0xAA11);
}
