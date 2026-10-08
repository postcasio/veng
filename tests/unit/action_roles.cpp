// Role-tagged actions and their per-frame resolution, headless. A role action is one the engine acts
// on: its press is resolved once per frame for every seat — the implicit all-devices seat and each
// locally-owned SeatInput seat in every world — and a navigation role is dispatched through the
// router's consumers only while the pressing seat holds UI focus, a ReleaseFocus press releasing the
// seat's gameplay focus only while it holds that. The resolver is driven over a real
// InputRouter, Input snapshot and WorldRunner; a recording consumer stands in for the Gui consumers,
// which the gpu band covers against real documents.

#include <doctest/doctest.h>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/InputConsumer.h>
#include <Veng/Input/SeatFocusScope.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <Input/RoleResolver.h>

#include <algorithm>

using namespace Veng;

namespace
{
    constexpr ActionId UiUp{0xA1};
    constexpr ActionId UiDown{0xA2};
    constexpr ActionId UiConfirm{0xA3};
    constexpr ActionId UiCancel{0xA4};
    constexpr ActionId ReleaseCursor{0xA5};

    // The repeat the directions carry: binary-exact, so a run of 0.125 s frames lands on each
    // deadline exactly rather than a float's width either side of it.
    constexpr f32 RepeatDelay = 0.5f;
    constexpr f32 RepeatRate = 0.25f;
    constexpr f32 Frame = 0.125f;

    InputAction Role(const ActionId id, const ActionRole role, const bool repeats)
    {
        return InputAction{.Id = id,
                           .Name = "role",
                           .Kind = ActionKind::Button,
                           .Role = role,
                           .RepeatDelay = repeats ? RepeatDelay : 0.0f,
                           .RepeatRate = repeats ? RepeatRate : 0.0f};
    }

    Binding BindKey(const ActionId action, const Veng::Key key)
    {
        return Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = u32(key)},
                       .Action = action};
    }

    // The default UI context: repeating Up/Down on the arrows, a non-repeating Confirm on Enter, and
    // Cancel on Escape.
    ResolvedContext UiContext()
    {
        return ResolvedContext{.Actions = {Role(UiUp, ActionRole::NavigateUp, true),
                                           Role(UiDown, ActionRole::NavigateDown, true),
                                           Role(UiConfirm, ActionRole::Confirm, false),
                                           Role(UiCancel, ActionRole::Cancel, false)},
                               .Bindings = {BindKey(UiUp, Key::Up), BindKey(UiDown, Key::Down),
                                            BindKey(UiConfirm, Key::Enter),
                                            BindKey(UiCancel, Key::Escape)}};
    }

    // A gameplay context releasing the cursor on Escape, the key the UI context cancels on.
    ResolvedContext ReleaseContext()
    {
        return ResolvedContext{.Actions = {Role(ReleaseCursor, ActionRole::ReleaseFocus, false)},
                               .Bindings = {BindKey(ReleaseCursor, Key::Escape)},
                               .RequiresGameplayFocus = true};
    }

    // A resident handle over a hand-built context, wired the way a prefab spawn rehydrates one.
    AssetHandle<InputMappingContext> MakeHandle(const u64 id, const ResolvedContext& context)
    {
        const Ref<InputMappingContext> resource = InputMappingContext::Create(
            context.Actions, context.Bindings, context.RequiresGameplayFocus);
        auto entry = CreateRef<Detail::AssetCacheEntry>(
            Detail::AssetCacheEntry{.Id = AssetId{.Value = id},
                                    .Type = AssetTypes::InputMap,
                                    .Resource = Detail::RefAny(resource)});
        AssetHandle<InputMappingContext> handle;
        Detail::RehydrateHandleField(&handle, AssetId{.Value = id}, std::move(entry));
        return handle;
    }

    // A scripted raw surface for the per-seat core: the keys down are a set.
    struct HeldKeys final : RawInputView
    {
        vector<u32> Down;

        [[nodiscard]] bool IsKeyDown(const u32 code) const override
        {
            return std::ranges::find(Down, code) != Down.end();
        }
        [[nodiscard]] bool IsButtonDown(InputDeviceType, u32) const override { return false; }
        [[nodiscard]] f32 GetAxis(InputDeviceType, u32) const override { return 0.0f; }

        void Hold(const Veng::Key key) { Down.push_back(u32(key)); }
        void Release(const Veng::Key key) { std::erase(Down, u32(key)); }
    };

    // Records every role press offered to it; optionally claims key presses, standing in for an
    // immediate-mode overlay holding the keyboard.
    struct RoleRecorder final : InputConsumer
    {
        vector<RoleEvent> Roles;
        vector<Veng::Key> Claims;
        bool Accept = true;

        bool ForwardEvent(const Event& event) override
        {
            if (event.GetEventType() != EventType::KeyPressed)
            {
                return false;
            }
            const Veng::Key key = static_cast<const KeyPressedEvent&>(event).GetKey();
            return std::ranges::find(Claims, key) != Claims.end();
        }

        bool ForwardRole(const RoleEvent& event) override
        {
            Roles.push_back(event);
            return Accept;
        }

        [[nodiscard]] usize CountFor(const SeatRef seat) const
        {
            return static_cast<usize>(std::ranges::count_if(Roles, [seat](const RoleEvent& event)
                                                            { return event.Seat == seat; }));
        }
    };

    // The headless frame: a router over a snapshot, a device-free runner, the resolver, and a
    // recorder. Keys go through the router exactly as window events do.
    struct Rig
    {
        TypeRegistry Types;
        SystemRegistry Systems;
        Input Snapshot{nullptr};
        Renderer::ViewportRegistry Viewports;
        InputRouter Router{nullptr, Snapshot, Viewports};
        PresentationScopes Presentation;
        WorldRunner Runner{
            WorldRunnerInfo{.Types = &Types, .Systems = &Systems, .Presentation = &Presentation}};
        RoleResolver Resolver;
        RoleRecorder Recorder;
        AssetHandle<InputMappingContext> DefaultUi = MakeHandle(0xD1, UiContext());
        FocusRequestTokens Tokens;
        bool UseDefault = true;

        Rig()
        {
            RegisterBuiltinTypes(Types);
            Router.RegisterConsumer(Recorder);
        }

        void Press(const Veng::Key key)
        {
            KeyPressedEvent event(key, 0, 0);
            Router.Dispatch(event);
        }

        void Release(const Veng::Key key)
        {
            KeyReleasedEvent event(key, 0, 0);
            Router.Dispatch(event);
        }

        void Step(const f32 delta = Frame)
        {
            const PointerRouting pointer{};
            Resolver.Update(RoleFrameInfo{.Snapshot = Snapshot,
                                          .Router = Router,
                                          .Worlds = Runner,
                                          .DefaultUi = UseDefault ? DefaultUi.Get() : nullptr,
                                          .Pointer = pointer,
                                          .Delta = delta,
                                          .FocusTokens = &Tokens});
            // The next frame begins: the snapshot rolls, applying any release this frame deferred.
            Snapshot.BeginFrame(true);
        }

        // Opens a world holding one keyboard seat; returns the seat.
        SeatRef OpenSeat(const bool withStack = false)
        {
            const WorldInstanceId world = Runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
            Scene& scene = Runner.ResolveWorld(world)->GetScene();
            const Entity viewer = scene.CreateEntity();
            scene.Add<Viewer>(viewer);
            scene.Add<SeatInput>(viewer, SeatInput{.UsesKeyboardMouse = true});
            if (withStack)
            {
                scene.Add<InputContextStack>(viewer);
            }
            return SeatRef{.World = world, .Viewer = viewer};
        }

        Scene& SceneOf(const SeatRef seat) { return Runner.ResolveWorld(seat.World)->GetScene(); }

        // Opens the keyboard seat holding the cursor, with the releasing gameplay context on its
        // stack.
        SeatRef OpenReleasingSeat()
        {
            const SeatRef seat = OpenSeat(true);
            SceneOf(seat)
                .Get<InputContextStack>(seat.Viewer)
                .Active.push_back(MakeHandle(0xE2, ReleaseContext()));
            Router.SetCursorSeat(seat);
            return seat;
        }

        // Stamps a FocusRequest for the seat and reconciles it as the engine's drain does.
        void RequestFocus(const SeatRef seat, const InputFocus focus)
        {
            string error;
            ReconcileFocusRequest(Router, Tokens, seat.World,
                                  FocusRequest{.Seat = seat.Viewer, .Focus = focus}, error);
        }
    };
}

TEST_CASE("action roles: a press fires once, however long it is held")
{
    RoleResolver resolver;
    const ResolvedContext ui = UiContext();
    HeldKeys keys;
    vector<RoleFire> fires;

    keys.Hold(Key::Enter);
    for (int frame = 0; frame < 20; ++frame)
    {
        resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, Frame, fires);
    }
    REQUIRE(fires.size() == 1);
    CHECK(fires[0].Role == ActionRole::Confirm);
    CHECK_FALSE(fires[0].Repeat);

    // Released and pressed again is a second press.
    keys.Release(Key::Enter);
    resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, Frame, fires);
    keys.Hold(Key::Enter);
    resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, Frame, fires);
    CHECK(fires.size() == 2);
}

TEST_CASE("action roles: a held role repeats at its delay, then at its rate, until released")
{
    RoleResolver resolver;
    const ResolvedContext ui = UiContext();
    HeldKeys keys;

    // Frame n of the hold (the press is frame 0) has been held n · Frame seconds.
    vector<int> fired;
    keys.Hold(Key::Down);
    for (int frame = 0; frame <= 10; ++frame)
    {
        vector<RoleFire> fires;
        resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, Frame, fires);
        if (!fires.empty())
        {
            CHECK(fires[0].Repeat == (frame != 0));
            fired.push_back(frame);
        }
    }
    // The press, then the delay (0.5 s = 4 frames), then every rate (0.25 s = 2 frames).
    CHECK(fired == vector<int>{0, 4, 6, 8, 10});

    // Released, nothing more fires however long the frames run.
    keys.Release(Key::Down);
    vector<RoleFire> after;
    for (int frame = 0; frame < 10; ++frame)
    {
        resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, Frame, after);
    }
    CHECK(after.empty());

    // A frame owing several repeats pays one.
    keys.Hold(Key::Down);
    vector<RoleFire> burst;
    resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, Frame, burst);
    resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, 2.0f, burst);
    CHECK(burst.size() == 2);
}

TEST_CASE("action roles: two held directions keep separate repeat timers")
{
    RoleResolver resolver;
    const ResolvedContext ui = UiContext();
    HeldKeys keys;

    vector<int> up;
    vector<int> down;
    for (int frame = 0; frame <= 6; ++frame)
    {
        if (frame == 0)
        {
            keys.Hold(Key::Up);
        }
        if (frame == 1)
        {
            keys.Hold(Key::Down);
        }
        vector<RoleFire> fires;
        resolver.ResolveSeat(SeatRef{}, {&ui, 1}, 0, keys, Frame, fires);
        for (const RoleFire& fire : fires)
        {
            (fire.Role == ActionRole::NavigateUp ? up : down).push_back(frame);
        }
    }
    // Each runs its own delay from its own press, one frame apart.
    CHECK(up == vector<int>{0, 4, 6});
    CHECK(down == vector<int>{1, 5});
}

TEST_CASE("action roles: nothing navigates without a default UI context")
{
    Rig rig;
    rig.UseDefault = false;
    rig.OpenSeat();

    rig.Press(Key::Down);
    rig.Step();
    CHECK(rig.Recorder.Roles.empty());
}

TEST_CASE("action roles: the implicit seat navigates under the cursor seat's UI focus only")
{
    Rig rig;

    // Under gameplay focus the press resolves but is not dispatched.
    const FocusToken gameplay = rig.Router.PushFocus(InputFocus::Gameplay);
    rig.Press(Key::Down);
    rig.Step();
    CHECK(rig.Recorder.Roles.empty());

    // The key is still held as focus returns to the UI: it is the same press, not a new one, so
    // nothing fires on the far side of the change either.
    rig.Router.PopFocus(gameplay);
    rig.Step();
    rig.Step();
    CHECK(rig.Recorder.Roles.empty());

    // A fresh press under UI focus navigates, once, on behalf of the implicit seat.
    rig.Release(Key::Down);
    rig.Step();
    rig.Press(Key::Down);
    rig.Step();
    REQUIRE(rig.Recorder.Roles.size() == 1);
    CHECK(rig.Recorder.Roles[0].Role == ActionRole::NavigateDown);
    CHECK(rig.Recorder.Roles[0].Seat.IsImplicit());
}

TEST_CASE("action roles: a seat in a paused world still navigates")
{
    Rig rig;
    const SeatRef seat = rig.OpenSeat();
    rig.Runner.SetWorldPaused(seat.World, true);

    rig.Press(Key::Down);
    rig.Step();
    CHECK(rig.Recorder.CountFor(seat) == 1);
}

TEST_CASE("action roles: a seat suspended under an empty context navigates nothing")
{
    Rig rig;
    const SeatRef seat = rig.OpenSeat(true);
    const InputSeat taken{
        .Viewer = seat.Viewer, .World = &rig.SceneOf(seat), .WorldId = seat.World};

    {
        const SeatFocusScope suspend(rig.Router, taken, nullptr, MakeHandle(0xE1, {}));
        rig.Press(Key::Down);
        rig.Step();
        rig.Step();
        CHECK(rig.Recorder.CountFor(seat) == 0);
    }

    // Restored while the key is still down: the press began under the suspension, so it is not a new
    // one on this side of it either — the Escape that closes an overlay does not also cancel the menu
    // it uncovers.
    rig.Step();
    CHECK(rig.Recorder.CountFor(seat) == 0);

    rig.Release(Key::Down);
    rig.Step();
    rig.Press(Key::Down);
    rig.Step();
    CHECK(rig.Recorder.CountFor(seat) == 1);
}

TEST_CASE("action roles: a seat context re-binding a role action's id shadows the default's keys")
{
    Rig rig;
    const SeatRef seat = rig.OpenSeat(true);

    // The seat's own context re-declares UiDown and binds it to S instead of the arrow.
    const ResolvedContext rebound{.Actions = {Role(UiDown, ActionRole::NavigateDown, false)},
                                  .Bindings = {BindKey(UiDown, Key::S)}};
    rig.SceneOf(seat)
        .Get<InputContextStack>(seat.Viewer)
        .Active.push_back(MakeHandle(0xE2, rebound));

    rig.Press(Key::Down);
    rig.Step();
    CHECK(rig.Recorder.CountFor(seat) == 0);
    rig.Release(Key::Down);

    rig.Press(Key::S);
    rig.Step();
    REQUIRE(rig.Recorder.CountFor(seat) == 1);
    const auto pressed = std::ranges::find(rig.Recorder.Roles, seat, &RoleEvent::Seat);
    CHECK(pressed->Role == ActionRole::NavigateDown);
}

TEST_CASE("action roles: a key a consumer claimed reads as up until it is released")
{
    Rig rig;
    rig.Recorder.Claims = {Key::Down};

    rig.Press(Key::Down);
    CHECK(rig.Router.IsKeyClaimed(Key::Down));
    for (int frame = 0; frame < 10; ++frame)
    {
        rig.Step();
    }
    CHECK(rig.Recorder.Roles.empty());

    // The release ends the claim; an unclaimed press navigates.
    rig.Release(Key::Down);
    CHECK_FALSE(rig.Router.IsKeyClaimed(Key::Down));
    rig.Step();
    rig.Recorder.Claims.clear();
    rig.Press(Key::Down);
    CHECK_FALSE(rig.Router.IsKeyClaimed(Key::Down));
    rig.Step();
    CHECK(rig.Recorder.Roles.size() == 1);
}

TEST_CASE("action roles: a press carries the modifiers held on the seat's keyboard")
{
    Rig rig;

    rig.Press(Key::RightShift);
    rig.Press(Key::LeftControl);
    rig.Press(Key::Down);
    rig.Step();
    REQUIRE(rig.Recorder.Roles.size() == 1);
    CHECK(rig.Recorder.Roles[0].Modifiers.Shift);
    CHECK(rig.Recorder.Roles[0].Modifiers.Control);
    CHECK_FALSE(rig.Recorder.Roles[0].Modifiers.Alt);
    CHECK_FALSE(rig.Recorder.Roles[0].Modifiers.Super);
}

TEST_CASE("action roles: a press reaches the first consumer that takes it and no other")
{
    Rig rig;
    RoleRecorder second;
    rig.Router.RegisterConsumer(second);

    rig.Press(Key::Enter);
    rig.Step();
    CHECK(rig.Recorder.Roles.size() == 1);
    CHECK(second.Roles.empty());

    // Declined by the first, it falls through to the next.
    rig.Recorder.Accept = false;
    rig.Release(Key::Enter);
    rig.Step();
    rig.Press(Key::Enter);
    rig.Step();
    CHECK(second.Roles.size() == 1);
}

TEST_CASE("action roles: ReleaseFocus releases a gameplay-focused seat and nothing under UI focus")
{
    Rig rig;
    const SeatRef seat = rig.OpenSeat(true);
    ResolvedContext anyFocus = ReleaseContext();
    anyFocus.RequiresGameplayFocus = false;
    rig.SceneOf(seat)
        .Get<InputContextStack>(seat.Viewer)
        .Active.push_back(MakeHandle(0xE3, anyFocus));
    rig.Router.SetCursorSeat(seat);

    // Resolved under UI focus too, the press has no gameplay focus to release.
    const FocusToken ui = rig.Router.PushFocus(seat, InputFocus::UI);
    rig.Press(Key::Escape);
    rig.Step();
    CHECK(rig.Router.IsFocusTokenLive(ui));
    rig.Release(Key::Escape);
    rig.Step();
    rig.Router.PopFocus(ui);

    const FocusToken gameplay = rig.Router.PushFocus(seat, InputFocus::Gameplay);
    rig.Press(Key::Escape);
    rig.Step();
    CHECK(rig.Router.GetFocus(seat) == InputFocus::UI);
    CHECK_FALSE(rig.Router.IsFocusTokenLive(gameplay));
}

TEST_CASE("action roles: the press that releases focus never also cancels")
{
    Rig rig;
    const SeatRef seat = rig.OpenReleasingSeat();
    rig.Router.PushFocus(seat, InputFocus::Gameplay);

    // Escape carries both roles. Its press releases, and the same press held on into the UI it
    // uncovered is never a Cancel there — not on its own frame, nor on any later one.
    rig.Press(Key::Escape);
    rig.Step();
    CHECK(rig.Router.GetFocus(seat) == InputFocus::UI);
    for (int frame = 0; frame < 4; ++frame)
    {
        rig.Step();
    }
    CHECK(rig.Recorder.Roles.empty());

    // A fresh press under UI focus is a Cancel.
    rig.Release(Key::Escape);
    rig.Step();
    rig.Press(Key::Escape);
    rig.Step();
    REQUIRE(rig.Recorder.CountFor(seat) == 1);
    CHECK(rig.Recorder.Roles[0].Role == ActionRole::Cancel);
}

TEST_CASE("action roles: a FocusRequest after a role release captures afresh")
{
    Rig rig;
    const SeatRef seat = rig.OpenReleasingSeat();

    rig.RequestFocus(seat, InputFocus::Gameplay);
    REQUIRE(rig.Tokens.size() == 1);

    // The release goes through the drain's token, which the drain then no longer holds.
    rig.Press(Key::Escape);
    rig.Step();
    CHECK(rig.Router.GetFocus(seat) == InputFocus::UI);
    CHECK(rig.Tokens.empty());
    rig.Release(Key::Escape);
    rig.Step();

    rig.RequestFocus(seat, InputFocus::Gameplay);
    CHECK(rig.Router.IsGameplayFocused(seat));
    CHECK(rig.Tokens.size() == 1);
    rig.RequestFocus(seat, InputFocus::UI);
    CHECK(rig.Router.GetFocus(seat) == InputFocus::UI);
}

TEST_CASE("action roles: with no release bound, Escape leaves gameplay focus alone")
{
    Rig rig;
    const SeatRef seat = rig.OpenSeat(true);
    rig.Router.SetCursorSeat(seat);
    rig.Router.PushFocus(seat, InputFocus::Gameplay);

    rig.Press(Key::Escape);
    rig.Step();
    CHECK(rig.Router.IsGameplayFocused(seat));
}

TEST_CASE("action roles: one press releases one entry, however many seats raise it")
{
    Rig rig;
    ResolvedContext ui = UiContext();
    ui.Actions.push_back(Role(ReleaseCursor, ActionRole::ReleaseFocus, false));
    ui.Bindings.push_back(BindKey(ReleaseCursor, Key::Escape));
    rig.DefaultUi = MakeHandle(0xD2, ui);

    // The seat and the implicit seat both resolve the default UI context's release against the
    // keyboard, and both release the cursor seat.
    const SeatRef seat = rig.OpenSeat();
    rig.Router.SetCursorSeat(seat);
    const FocusToken beneath = rig.Router.PushFocus(seat, InputFocus::Gameplay);
    const FocusToken top = rig.Router.PushFocus(seat, InputFocus::Gameplay);

    rig.Press(Key::Escape);
    rig.Step();
    CHECK_FALSE(rig.Router.IsFocusTokenLive(top));
    CHECK(rig.Router.IsFocusTokenLive(beneath));
    CHECK(rig.Router.IsGameplayFocused(seat));
}
