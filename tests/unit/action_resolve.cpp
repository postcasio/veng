// The pure action-resolve core: ResolveActions turns a stack of active binding
// contexts plus a raw input snapshot into a resolved ActionState. Device-free — no
// Context, no Window, no asset. The raw surface is a scripted fake, so these run with
// no ICD, foundation-first like DecideBarrier / ComputeCascades.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <utility>

#include <Veng/Input/Actions.h>

using namespace Veng;

namespace
{
    // Placeholder-independent action ids for the tests — arbitrary distinct non-zero values.
    constexpr ActionId Move{0xA1};
    constexpr ActionId Jump{0xB2};
    constexpr ActionId Throttle{0xC3};

    // Keyboard control codes the fake and the bindings agree on.
    constexpr u32 KeyW = 1;
    constexpr u32 KeyA = 2;
    constexpr u32 KeyS = 3;
    constexpr u32 KeyD = 4;
    constexpr u32 KeySpace = 5;

    // A scripted raw input surface: keys and pad buttons down are sets, axes are a small map.
    // Neutral by default (empty), modelling the headless snapshot.
    struct FakeRawInput final : RawInputView
    {
        vector<u32> KeysDown;
        vector<u32> PadButtonsDown;
        vector<std::pair<u32, f32>> Axes;

        [[nodiscard]] bool IsKeyDown(u32 code) const override
        {
            return std::ranges::find(KeysDown, code) != KeysDown.end();
        }

        [[nodiscard]] bool IsButtonDown(InputDeviceType device, u32 code) const override
        {
            return device == InputDeviceType::GamepadButton &&
                   std::ranges::find(PadButtonsDown, code) != PadButtonsDown.end();
        }

        [[nodiscard]] f32 GetAxis(InputDeviceType, u32 code) const override
        {
            for (const auto& [axisCode, value] : Axes)
            {
                if (axisCode == code)
                {
                    return value;
                }
            }
            return 0.0f;
        }
    };

    // A WASD → 2D Move + Space → Jump context, the canonical gameplay binding set.
    ResolvedContext WasdContext()
    {
        return ResolvedContext{
            .Actions = {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D},
                        InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button}},
            .Bindings = {
                Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyD},
                        .Action = Move,
                        .Axis = AxisComponent::X,
                        .Scale = 1.0f},
                Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyA},
                        .Action = Move,
                        .Axis = AxisComponent::X,
                        .Scale = -1.0f},
                Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                        .Action = Move,
                        .Axis = AxisComponent::Y,
                        .Scale = 1.0f},
                Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyS},
                        .Action = Move,
                        .Axis = AxisComponent::Y,
                        .Scale = -1.0f},
                Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeySpace},
                        .Action = Jump,
                        .Axis = AxisComponent::Whole,
                        .Scale = 1.0f}}};
    }
}

TEST_CASE("ResolveActions maps WASD onto a 2D Move action with the right signs")
{
    const ResolvedContext context = WasdContext();
    const std::array active{context};

    SUBCASE("W drives +Y")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeyW};
        const ActionState state = ResolveActions(active, raw, {});
        CHECK(state.GetValue(Move) == vec2{0.0f, 1.0f});
    }

    SUBCASE("A drives -X")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeyA};
        const ActionState state = ResolveActions(active, raw, {});
        CHECK(state.GetValue(Move) == vec2{-1.0f, 0.0f});
    }

    SUBCASE("diagonals sum both components")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeyW, KeyD};
        const ActionState state = ResolveActions(active, raw, {});
        CHECK(state.GetValue(Move) == vec2{1.0f, 1.0f});
    }

    SUBCASE("opposing keys cancel")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeyA, KeyD};
        const ActionState state = ResolveActions(active, raw, {});
        CHECK(state.GetValue(Move) == vec2{0.0f, 0.0f});
    }
}

TEST_CASE("ResolveActions drives a 1D axis action from a whole-axis source")
{
    const ResolvedContext context{
        .Actions = {InputAction{.Id = Throttle, .Name = "Throttle", .Kind = ActionKind::Axis1D}},
        .Bindings = {Binding{.Source = {.Device = InputDeviceType::GamepadAxis, .Control = 0},
                             .Action = Throttle,
                             .Axis = AxisComponent::Whole,
                             .Scale = 1.0f}}};
    const std::array active{context};

    FakeRawInput raw;
    raw.Axes = {{0, 0.5f}};
    const ActionState state = ResolveActions(active, raw, {});
    CHECK(state.GetAxis(Throttle) == doctest::Approx(0.5f));
}

TEST_CASE("Two whole-axis bindings on one action are an OR, not the last one listed")
{
    // The alternate-key case: one verb, two keys that each mean it. Nothing in the pair is
    // privileged, so either alone must activate the action and neither may cancel the other by
    // resting. A sum would be wrong in the other direction — two keys held is still one press.
    const ResolvedContext context{
        .Actions = {InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button}},
        .Bindings = {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeySpace},
                             .Action = Jump,
                             .Axis = AxisComponent::Whole,
                             .Scale = 1.0f},
                     Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                             .Action = Jump,
                             .Axis = AxisComponent::Whole,
                             .Scale = 1.0f}}};
    const std::array active{context};

    SUBCASE("the first-listed key alone activates it")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeySpace};
        CHECK(ResolveActions(active, raw, {}).IsHeld(Jump));
    }

    SUBCASE("the last-listed key alone activates it")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeyW};
        CHECK(ResolveActions(active, raw, {}).IsHeld(Jump));
    }

    SUBCASE("both held reads as one press, not two")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeySpace, KeyW};
        const ActionState state = ResolveActions(active, raw, {});
        CHECK(state.IsHeld(Jump));
        CHECK(state.GetAxis(Jump) == doctest::Approx(1.0f));
    }

    SUBCASE("neither held leaves it inactive")
    {
        const FakeRawInput raw;
        CHECK_FALSE(ResolveActions(active, raw, {}).IsHeld(Jump));
    }
}

TEST_CASE("A whole-axis stick and a key on one action resolve to the stronger push")
{
    // The same rule where the two sources are not interchangeable: a partly-deflected stick must
    // not be lifted to a full press by a key resting beside it, and a full key press must not be
    // dragged down by the stick's own rest.
    const ResolvedContext context{
        .Actions = {InputAction{.Id = Throttle, .Name = "Throttle", .Kind = ActionKind::Axis1D}},
        .Bindings = {Binding{.Source = {.Device = InputDeviceType::GamepadAxis, .Control = 0},
                             .Action = Throttle,
                             .Axis = AxisComponent::Whole,
                             .Scale = 1.0f},
                     Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeySpace},
                             .Action = Throttle,
                             .Axis = AxisComponent::Whole,
                             .Scale = 1.0f}}};
    const std::array active{context};

    SUBCASE("the stick alone reads its own deflection")
    {
        FakeRawInput raw;
        raw.Axes = {{0, 0.5f}};
        CHECK(ResolveActions(active, raw, {}).GetAxis(Throttle) == doctest::Approx(0.5f));
    }

    SUBCASE("a key beside a resting stick reads the key")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeySpace};
        CHECK(ResolveActions(active, raw, {}).GetAxis(Throttle) == doctest::Approx(1.0f));
    }

    SUBCASE("a deflection past the key's own magnitude wins")
    {
        FakeRawInput raw;
        raw.KeysDown = {KeySpace};
        raw.Axes = {{0, -1.0f}};
        CHECK(ResolveActions(active, raw, {}).GetAxis(Throttle) == doctest::Approx(-1.0f));
    }
}

TEST_CASE("ResolveActions derives Started/Ongoing/Completed across scripted ticks")
{
    const ResolvedContext context = WasdContext();
    const std::array active{context};

    FakeRawInput down;
    down.KeysDown = {KeySpace};
    const FakeRawInput up;

    // Tick 1: pressed → Started.
    const ActionState t1 = ResolveActions(active, down, {});
    CHECK(t1.WasTriggered(Jump));
    CHECK(t1.IsHeld(Jump));
    CHECK_FALSE(t1.WasReleased(Jump));

    // Tick 2: still held → Ongoing.
    const ActionState t2 = ResolveActions(active, down, t1);
    CHECK_FALSE(t2.WasTriggered(Jump));
    CHECK(t2.IsHeld(Jump));

    // Tick 3: released → Completed.
    const ActionState t3 = ResolveActions(active, up, t2);
    CHECK(t3.WasReleased(Jump));
    CHECK_FALSE(t3.IsHeld(Jump));

    // Tick 4: still up → None.
    const ActionState t4 = ResolveActions(active, up, t3);
    CHECK_FALSE(t4.WasReleased(Jump));
    CHECK_FALSE(t4.IsHeld(Jump));
    CHECK_FALSE(t4.WasTriggered(Jump));
}

TEST_CASE("ResolveActions seeds the frame-accumulated edges from this tick's phase")
{
    const ResolvedContext context = WasdContext();
    const std::array active{context};

    FakeRawInput down;
    down.KeysDown = {KeySpace};
    const FakeRawInput up;

    // A single tick reads the *ThisFrame edges identically to the per-tick Was* — the seed the
    // InputMappingSystem later ORs across a multi-step frame, unchanged for a one-step frame.
    const ActionState started = ResolveActions(active, down, {});
    CHECK(started.WasTriggeredThisFrame(Jump));
    CHECK_FALSE(started.WasReleasedThisFrame(Jump));

    const ActionState ongoing = ResolveActions(active, down, started);
    CHECK_FALSE(ongoing.WasTriggeredThisFrame(Jump));
    CHECK_FALSE(ongoing.WasReleasedThisFrame(Jump));

    const ActionState completed = ResolveActions(active, up, ongoing);
    CHECK(completed.WasReleasedThisFrame(Jump));
    CHECK_FALSE(completed.WasTriggeredThisFrame(Jump));
}

TEST_CASE("A higher-priority context rebinds an action and leaves others falling through")
{
    const ResolvedContext base = WasdContext();

    // A vehicle context that rebinds Move entirely (W now drives -Y) but declares no Jump.
    const ResolvedContext vehicle{
        .Actions = {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D}},
        .Bindings = {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                             .Action = Move,
                             .Axis = AxisComponent::Y,
                             .Scale = -1.0f}}};
    const std::array active{base, vehicle};

    FakeRawInput raw;
    raw.KeysDown = {KeyW, KeySpace};
    const ActionState state = ResolveActions(active, raw, {});

    // Move fully rebound by the higher context: W → -Y, base's +Y binding shadowed entirely.
    CHECK(state.GetValue(Move) == vec2{0.0f, -1.0f});
    // Jump falls through to the base context (the vehicle context does not bind it).
    CHECK(state.WasTriggered(Jump));
}

TEST_CASE("The sample set is one per declared action in deterministic stack order")
{
    const ResolvedContext base = WasdContext();
    const ResolvedContext extra{
        .Actions = {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D},
                    InputAction{.Id = Throttle, .Name = "Throttle", .Kind = ActionKind::Axis1D}},
        .Bindings = {}};
    const std::array active{base, extra};

    const FakeRawInput raw;
    const ActionState state = ResolveActions(active, raw, {});

    // base declares Move, Jump; extra declares Move (already present, keeps first position),
    // then Throttle. So the order is Move, Jump, Throttle — one sample each.
    REQUIRE(state.Actions.size() == 3);
    CHECK(state.Actions[0].Id == Move);
    CHECK(state.Actions[1].Id == Jump);
    CHECK(state.Actions[2].Id == Throttle);
}

TEST_CASE("An unbound declared action still produces a None sample")
{
    const ResolvedContext context{
        .Actions = {InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button}},
        .Bindings = {}};
    const std::array active{context};

    const FakeRawInput raw;
    const ActionState state = ResolveActions(active, raw, {});
    REQUIRE(state.Actions.size() == 1);
    CHECK(state.Actions[0].Id == Jump);
    CHECK(state.Actions[0].Value == vec2{0.0f, 0.0f});
    CHECK(state.Actions[0].Phase == ActionPhase::None);
}

TEST_CASE("A focus-gated context is excluded from resolution while gameplay focus is off")
{
    // The gate is a pure predicate over the resolved contexts; the InputMappingSystem filters the
    // seat's stack through it before ResolveActions. Model that here device-free: a gated gameplay
    // context stacked over an always-active base, filtered by focus, resolved.
    ResolvedContext gameplay = WasdContext();
    gameplay.RequiresGameplayFocus = true;

    // The authored stack, never mutated by the gate.
    const std::array<ResolvedContext, 1> stack{gameplay};

    FakeRawInput raw;
    raw.KeysDown = {KeyW, KeySpace};

    // Build the effective active list exactly as the system does: keep only contexts active under
    // the current focus. This is the whole of the gate — the source stack is untouched throughout.
    const auto effective = [&](bool focused)
    {
        vector<ResolvedContext> active;
        for (const ResolvedContext& context : stack)
        {
            if (IsContextActiveUnderFocus(context, focused))
            {
                active.push_back(context);
            }
        }
        return active;
    };

    SUBCASE("unfocused: the gated context contributes nothing")
    {
        const vector<ResolvedContext> active = effective(/*focused=*/false);
        CHECK(active.empty());
        const ActionState state = ResolveActions(active, raw, {});
        // No context resolved, so no action samples at all — every gameplay binding silenced.
        CHECK(state.Actions.empty());
        CHECK(state.GetValue(Move) == vec2{0.0f, 0.0f});
        CHECK_FALSE(state.WasTriggered(Jump));
    }

    SUBCASE("focused: the gated context resolves identically to an ungated one")
    {
        const vector<ResolvedContext> active = effective(/*focused=*/true);
        REQUIRE(active.size() == 1);
        const ActionState gatedState = ResolveActions(active, raw, {});

        // The same bindings as an ungated stack yield a byte-identical result once focused.
        const ResolvedContext ungated = WasdContext();
        const std::array<ResolvedContext, 1> ungatedStack{ungated};
        const ActionState ungatedState = ResolveActions(ungatedStack, raw, {});

        CHECK(gatedState.GetValue(Move) == ungatedState.GetValue(Move));
        CHECK(gatedState.WasTriggered(Jump) == ungatedState.WasTriggered(Jump));
        CHECK(gatedState.GetValue(Move) == vec2{0.0f, 1.0f});
    }

    // The authored stack's one entry still carries its gate flag — never mutated by evaluation.
    CHECK(stack[0].RequiresGameplayFocus);
}

TEST_CASE("An ungated stack resolves the same regardless of focus")
{
    const ResolvedContext base = WasdContext(); // RequiresGameplayFocus defaults false
    const std::array<ResolvedContext, 1> stack{base};

    FakeRawInput raw;
    raw.KeysDown = {KeyD};

    CHECK(IsContextActiveUnderFocus(stack[0], /*focused=*/true));
    CHECK(IsContextActiveUnderFocus(stack[0], /*focused=*/false));

    const ActionState focused = ResolveActions(stack, raw, {});
    const ActionState unfocused = ResolveActions(stack, raw, {});
    CHECK(focused.GetValue(Move) == unfocused.GetValue(Move));
    CHECK(focused.GetValue(Move) == vec2{1.0f, 0.0f});
}

TEST_CASE("A neutral snapshot yields every action None with zero value")
{
    const ResolvedContext context = WasdContext();
    const std::array active{context};

    const FakeRawInput neutral;
    const ActionState state = ResolveActions(active, neutral, {});
    REQUIRE(state.Actions.size() == 2);
    for (const ActionSample& sample : state.Actions)
    {
        CHECK(sample.Value == vec2{0.0f, 0.0f});
        CHECK(sample.Phase == ActionPhase::None);
    }
}

namespace
{
    constexpr ActionId Left{0xD4};
    constexpr ActionId Right{0xE5};

    // One axis source (control 0) bound to an action of the given kind, with the given shaping.
    ResolvedContext AxisBound(const ActionId id, const ActionKind kind, const f32 scale,
                              const f32 threshold, const f32 exponent = 1.0f)
    {
        return ResolvedContext{
            .Actions = {InputAction{.Id = id, .Name = "Shaped", .Kind = kind}},
            .Bindings = {Binding{.Source = {.Device = InputDeviceType::GamepadAxis, .Control = 0},
                                 .Action = id,
                                 .Axis = AxisComponent::Whole,
                                 .Scale = scale,
                                 .Threshold = threshold,
                                 .Exponent = exponent}}};
    }

    // Resolves one context against a single axis value on control 0.
    ActionState ResolveAxis(const ResolvedContext& context, const f32 value)
    {
        FakeRawInput raw;
        raw.Axes = {{0, value}};
        const std::array active{context};
        return ResolveActions(active, raw, {});
    }
}

TEST_CASE("A trigger bound to a button presses at its threshold and not below it")
{
    const ResolvedContext context = AxisBound(Jump, ActionKind::Button, 1.0f, 0.5f);

    const ActionState atThreshold = ResolveAxis(context, 0.5f);
    CHECK(atThreshold.IsHeld(Jump));
    CHECK(atThreshold.GetAxis(Jump) == 1.0f);
    CHECK_FALSE(ResolveAxis(context, 0.49f).IsHeld(Jump));
}

TEST_CASE("A stick axis bound to two buttons by sign fires each only on its own half")
{
    const ResolvedContext context{
        .Actions = {InputAction{.Id = Left, .Name = "Left", .Kind = ActionKind::Button},
                    InputAction{.Id = Right, .Name = "Right", .Kind = ActionKind::Button}},
        .Bindings = {Binding{.Source = {.Device = InputDeviceType::GamepadAxis, .Control = 0},
                             .Action = Left,
                             .Scale = -1.0f,
                             .Threshold = 0.5f},
                     Binding{.Source = {.Device = InputDeviceType::GamepadAxis, .Control = 0},
                             .Action = Right,
                             .Scale = 1.0f,
                             .Threshold = 0.5f}}};

    const ActionState pushedLeft = ResolveAxis(context, -0.8f);
    CHECK(pushedLeft.IsHeld(Left));
    CHECK_FALSE(pushedLeft.IsHeld(Right));

    const ActionState pushedRight = ResolveAxis(context, 0.8f);
    CHECK_FALSE(pushedRight.IsHeld(Left));
    CHECK(pushedRight.IsHeld(Right));
}

TEST_CASE("A resting axis never presses a default-threshold button, whatever its scale")
{
    for (const f32 scale : {1.0f, -1.0f})
    {
        const ActionState state =
            ResolveAxis(AxisBound(Jump, ActionKind::Button, scale, 0.0f), 0.0f);
        CHECK(state.Actions.front().Phase == ActionPhase::None);
    }
}

TEST_CASE("An axis action ignores a source under its threshold and passes it unrescaled above")
{
    const ResolvedContext context = AxisBound(Throttle, ActionKind::Axis1D, 1.0f, 0.2f);
    CHECK(ResolveAxis(context, -0.19f).GetAxis(Throttle) == 0.0f);
    CHECK(ResolveAxis(context, -0.3f).GetAxis(Throttle) == doctest::Approx(-0.3f));
}

TEST_CASE("A response exponent curves the magnitude and keeps the sign")
{
    const ResolvedContext squared = AxisBound(Throttle, ActionKind::Axis1D, 1.0f, 0.0f, 2.0f);
    CHECK(ResolveAxis(squared, 0.5f).GetAxis(Throttle) == doctest::Approx(0.25f));
    CHECK(ResolveAxis(squared, -0.5f).GetAxis(Throttle) == doctest::Approx(-0.25f));
    CHECK(ResolveAxis(squared, 1.0f).GetAxis(Throttle) == doctest::Approx(1.0f));
}

namespace
{
    constexpr ActionId Yaw{0xF6};
    constexpr ActionId Roll{0x107};
    constexpr ActionId Boost{0x118};

    // Pad control codes the fake and the bindings agree on.
    constexpr u32 StickX = 2;
    constexpr u32 StickY = 3;
    constexpr u32 Trigger = 5;
    constexpr u32 StickClick = 7;

    constexpr InputSource StickXSource{.Device = InputDeviceType::GamepadAxis, .Control = StickX};
    constexpr InputSource StickClickModifier{.Device = InputDeviceType::GamepadButton,
                                             .Control = StickClick};

    // Stick X drives Yaw plainly.
    Binding PlainYaw()
    {
        return Binding{.Source = StickXSource, .Action = Yaw};
    }

    // Stick X drives Roll while the stick is clicked.
    Binding ChordRoll()
    {
        return Binding{.Source = StickXSource, .Action = Roll, .Modifier = StickClickModifier};
    }

    InputAction AxisAction(const ActionId id)
    {
        return InputAction{.Id = id, .Name = "Axis", .Kind = ActionKind::Axis1D};
    }

    // Plain Yaw and chorded Roll on the same stick axis, in one context.
    ResolvedContext StickContext()
    {
        return ResolvedContext{.Actions = {AxisAction(Yaw), AxisAction(Roll)},
                               .Bindings = {PlainYaw(), ChordRoll()}};
    }

    // The stick pushed to 0.8 on X, with the stick click held or not.
    FakeRawInput StickPushed(const bool clicked)
    {
        FakeRawInput raw;
        raw.Axes = {{StickX, 0.8f}};
        if (clicked)
        {
            raw.PadButtonsDown = {StickClick};
        }
        return raw;
    }
}

TEST_CASE("A chord contributes only while its modifier is down")
{
    const std::array active{StickContext()};

    CHECK(ResolveActions(active, StickPushed(false), {}).GetAxis(Roll) == 0.0f);
    CHECK(ResolveActions(active, StickPushed(true), {}).GetAxis(Roll) == doctest::Approx(0.8f));
}

TEST_CASE("A chord silences the plain binding on its control, which is live again on release")
{
    const std::array active{StickContext()};

    const ActionState held = ResolveActions(active, StickPushed(true), {});
    CHECK(held.GetAxis(Yaw) == 0.0f);
    CHECK(held.GetAxis(Roll) == doctest::Approx(0.8f));

    const ActionState released = ResolveActions(active, StickPushed(false), held);
    CHECK(released.GetAxis(Yaw) == doctest::Approx(0.8f));
    CHECK(released.GetAxis(Roll) == 0.0f);
    CHECK(released.WasReleased(Roll));
}

TEST_CASE("A chord leaves a plain binding on a different control alone")
{
    ResolvedContext context = StickContext();
    context.Actions.push_back(AxisAction(Throttle));
    context.Bindings.push_back(Binding{
        .Source = {.Device = InputDeviceType::GamepadAxis, .Control = StickY}, .Action = Throttle});
    const std::array active{context};

    FakeRawInput raw = StickPushed(true);
    raw.Axes.emplace_back(StickY, -0.4f);
    const ActionState state = ResolveActions(active, raw, {});
    CHECK(state.GetAxis(Roll) == doctest::Approx(0.8f));
    CHECK(state.GetAxis(Throttle) == doctest::Approx(-0.4f));
}

TEST_CASE("A live chord suppresses across contexts; a shadowed chord suppresses nothing")
{
    const ResolvedContext plain{.Actions = {AxisAction(Yaw)}, .Bindings = {PlainYaw()}};
    const ResolvedContext chord{.Actions = {AxisAction(Roll)}, .Bindings = {ChordRoll()}};

    SUBCASE("a chord in a higher context silences a lower plain binding")
    {
        const std::array active{plain, chord};
        const ActionState state = ResolveActions(active, StickPushed(true), {});
        CHECK(state.GetAxis(Yaw) == 0.0f);
        CHECK(state.GetAxis(Roll) == doctest::Approx(0.8f));
    }

    SUBCASE("a chord in a lower context silences a higher plain binding")
    {
        const std::array active{chord, plain};
        const ActionState state = ResolveActions(active, StickPushed(true), {});
        CHECK(state.GetAxis(Yaw) == 0.0f);
        CHECK(state.GetAxis(Roll) == doctest::Approx(0.8f));
    }

    SUBCASE("a chord whose action a higher context rebinds is dead")
    {
        // The top context binds Roll to a key, shadowing the chord, so the stick keeps driving Yaw.
        const ResolvedContext rebound{
            .Actions = {AxisAction(Roll)},
            .Bindings = {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                                 .Action = Roll}}};
        const std::array active{plain, chord, rebound};
        const ActionState state = ResolveActions(active, StickPushed(true), {});
        CHECK(state.GetAxis(Yaw) == doctest::Approx(0.8f));
        CHECK(state.GetAxis(Roll) == 0.0f);
    }
}

TEST_CASE("An axis modifier counts at its ModifierThreshold, not the binding's Threshold")
{
    // A trigger modifier at 0.6; the binding's own Threshold (0.1) shapes only the stick.
    Binding chord = ChordRoll();
    chord.Modifier = {.Device = InputDeviceType::GamepadAxis, .Control = Trigger};
    chord.ModifierThreshold = 0.6f;
    chord.Threshold = 0.1f;
    const std::array active{ResolvedContext{.Actions = {AxisAction(Yaw), AxisAction(Roll)},
                                            .Bindings = {PlainYaw(), chord}}};

    const auto resolveAt = [&active](const f32 trigger)
    {
        FakeRawInput raw;
        raw.Axes = {{StickX, 0.8f}, {Trigger, trigger}};
        return ResolveActions(active, raw, {});
    };

    const ActionState light = resolveAt(0.3f);
    CHECK(light.GetAxis(Roll) == 0.0f);
    CHECK(light.GetAxis(Yaw) == doctest::Approx(0.8f));

    const ActionState pulled = resolveAt(0.6f);
    CHECK(pulled.GetAxis(Roll) == doctest::Approx(0.8f));
    CHECK(pulled.GetAxis(Yaw) == 0.0f);
}

TEST_CASE("A keyboard chord on a button action replaces the key's plain meaning")
{
    constexpr u32 KeyShift = 6;
    const std::array active{ResolvedContext{
        .Actions = {InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button},
                    InputAction{.Id = Boost, .Name = "Boost", .Kind = ActionKind::Button}},
        .Bindings = {
            Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeySpace},
                    .Action = Jump},
            Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeySpace},
                    .Action = Boost,
                    .Modifier = {.Device = InputDeviceType::Keyboard, .Control = KeyShift}}}}};

    FakeRawInput raw;
    raw.KeysDown = {KeySpace, KeyShift};
    const ActionState state = ResolveActions(active, raw, {});
    CHECK(state.IsHeld(Boost));
    CHECK_FALSE(state.IsHeld(Jump));
}

TEST_CASE("Modifier then control and control then modifier resolve the same")
{
    const std::array active{StickContext()};

    FakeRawInput modifierOnly;
    modifierOnly.PadButtonsDown = {StickClick};
    const ActionState modifierFirst =
        ResolveActions(active, StickPushed(true), ResolveActions(active, modifierOnly, {}));

    const ActionState controlFirst =
        ResolveActions(active, StickPushed(true), ResolveActions(active, StickPushed(false), {}));

    for (const ActionState* state : {&modifierFirst, &controlFirst})
    {
        CHECK(state->GetAxis(Roll) == doctest::Approx(0.8f));
        CHECK(state->WasTriggered(Roll));
        CHECK(state->GetAxis(Yaw) == 0.0f);
    }
}
