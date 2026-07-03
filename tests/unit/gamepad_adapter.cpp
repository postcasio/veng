// The live gamepad resolve path: a polled GamepadState folded into Veng::Input, read
// through the RawInput adapter, resolves a GamepadButton / GamepadAxis binding to the
// right action value — where planset-42's inert arms resolved zero. Headless (Input over
// a null window), so it runs with no ICD; the adapter and the enum→control mapping are
// what is under test.

#include <doctest/doctest.h>

#include <array>

#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/RawInput.h>

using namespace Veng;

namespace
{
    // Action ids for the fixture — arbitrary distinct non-zero values.
    constexpr ActionId Move{0xA1};
    constexpr ActionId Jump{0xB2};

    // A single connected pad in slot 0 with the given axes/buttons, ingested into an Input.
    void IngestOnePad(Input& input, const GamepadState& pad)
    {
        std::array<GamepadState, 16> slots{};
        slots[0] = pad;
        input.IngestGamepadStates(slots);
    }

    // A left-stick → 2D Move (Y inverted to match a forward-is-up stick) + A → Jump context.
    ResolvedContext PadContext()
    {
        return ResolvedContext{
            .Actions = {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D},
                        InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button}},
            .Bindings = {Binding{.Source = {.Device = InputDeviceType::GamepadAxis,
                                            .Control = u32(GamepadAxis::LeftX)},
                                 .Action = Move,
                                 .Axis = AxisComponent::X,
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::GamepadAxis,
                                            .Control = u32(GamepadAxis::LeftY)},
                                 .Action = Move,
                                 .Axis = AxisComponent::Y,
                                 .Scale = -1.0f},
                         Binding{.Source = {.Device = InputDeviceType::GamepadButton,
                                            .Control = u32(GamepadButton::A)},
                                 .Action = Jump,
                                 .Axis = AxisComponent::Whole,
                                 .Scale = 1.0f}}};
    }
}

TEST_CASE("A gamepad axis binding resolves against the designated pad's stick")
{
    Input input(nullptr);
    input.BeginFrame();

    GamepadState pad;
    pad.Connected = true;
    pad.Axes[usize(GamepadAxis::LeftX)] = 0.5f;
    pad.Axes[usize(GamepadAxis::LeftY)] = -1.0f; // stick up: −Y raw → +Y Move via the −1 scale
    IngestOnePad(input, pad);

    const RawInput raw(input);
    const ResolvedContext context = PadContext();
    const std::array active{context};

    const ActionState state = ResolveActions(active, raw, {});
    CHECK(state.GetValue(Move).x == doctest::Approx(0.5f));
    CHECK(state.GetValue(Move).y == doctest::Approx(1.0f));
}

TEST_CASE("A gamepad button binding resolves against the designated pad")
{
    Input input(nullptr);
    input.BeginFrame();

    GamepadState pad;
    pad.Connected = true;
    pad.Buttons[usize(GamepadButton::A)] = true;
    IngestOnePad(input, pad);

    const RawInput raw(input);
    const ResolvedContext context = PadContext();
    const std::array active{context};

    const ActionState state = ResolveActions(active, raw, {});
    CHECK(state.WasTriggered(Jump));
    CHECK(state.IsHeld(Jump));
}

TEST_CASE("With no pad connected a gamepad binding resolves neutral")
{
    Input input(nullptr);
    input.BeginFrame();
    // No IngestGamepadStates → the neutral no-pads surface a headless run reports.

    const RawInput raw(input);
    const ResolvedContext context = PadContext();
    const std::array active{context};

    const ActionState state = ResolveActions(active, raw, {});
    CHECK(state.GetValue(Move) == vec2{0.0f, 0.0f});
    CHECK_FALSE(state.IsHeld(Jump));
}

TEST_CASE("Veng::Input reports the pad connect state and the pressed edge")
{
    Input input(nullptr);

    input.BeginFrame();
    GamepadState pad;
    pad.Connected = true;
    pad.Buttons[usize(GamepadButton::A)] = true;
    IngestOnePad(input, pad);

    const auto slot0 = static_cast<GamepadId>(0);
    CHECK(input.IsGamepadConnected(slot0));
    CHECK(input.IsGamepadButtonDown(slot0, GamepadButton::A));
    CHECK(input.WasGamepadButtonPressed(slot0, GamepadButton::A));
    REQUIRE(input.ConnectedGamepads().size() == 1);
    CHECK(input.ConnectedGamepads()[0] == slot0);

    // Next frame still held → down but no longer a fresh press.
    input.BeginFrame();
    IngestOnePad(input, pad);
    CHECK(input.IsGamepadButtonDown(slot0, GamepadButton::A));
    CHECK_FALSE(input.WasGamepadButtonPressed(slot0, GamepadButton::A));

    // A stale slot (none connected) reports neutral.
    input.BeginFrame();
    input.IngestGamepadStates(std::array<GamepadState, 16>{});
    CHECK_FALSE(input.IsGamepadConnected(slot0));
    CHECK_FALSE(input.IsGamepadButtonDown(slot0, GamepadButton::A));
    CHECK(input.ConnectedGamepads().empty());
    CHECK_FALSE(input.IsGamepadConnected(GamepadId::None));
}
