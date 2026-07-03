// Per-seat device routing: the SeatInputView filters the shared Veng::Input snapshot to one
// seat's assigned devices, so two seats resolve to distinct PlayerInputs, and the
// DeviceAssignmentSystem auto-assigns a connected pad to the first opted-in seat and clears it
// on disconnect. Headless (Input over a null window), so it runs with no ICD; the view's device
// gating and the assignment policy are what is under test.

#include <doctest/doctest.h>

#include <array>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/RawInput.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/DeviceAssignmentSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

using namespace Veng;

namespace
{
    // Action ids for the fixture — arbitrary distinct non-zero values.
    constexpr ActionId Move{0xA1};
    constexpr ActionId Jump{0xB2};

    // Keyboard control code the bindings and the injected key event agree on.
    constexpr u32 KeyW = u32(Key::W);

    // Ingests one connected pad into the given slot, leaving the rest empty.
    void IngestPad(Input& input, const usize slot, const GamepadState& pad)
    {
        std::array<GamepadState, 16> slots{};
        slots[slot] = pad;
        input.IngestGamepadStates(slots);
    }

    // A W → Move.y (keyboard) + left-stick-X → Move.x + A → Jump (gamepad) context, so the
    // keyboard and the gamepad each drive a distinguishable part of the resolved state.
    ResolvedContext MixedContext()
    {
        return ResolvedContext{
            .Actions = {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D},
                        InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button}},
            .Bindings = {Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = KeyW},
                                 .Action = Move,
                                 .Axis = AxisComponent::Y,
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::GamepadAxis,
                                            .Control = u32(GamepadAxis::LeftX)},
                                 .Action = Move,
                                 .Axis = AxisComponent::X,
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::GamepadButton,
                                            .Control = u32(GamepadButton::A)},
                                 .Action = Jump,
                                 .Axis = AxisComponent::Whole,
                                 .Scale = 1.0f}}};
    }

    // An Input snapshot with W held on the keyboard, pad slot 0 pushing its stick +X and A down,
    // and pad slot 1 pushing its stick the opposite way — the shared devices the per-seat views
    // narrow.
    void SeedDevices(Input& input)
    {
        input.ApplyEvent(KeyPressedEvent{Key::W, 0, 0});

        std::array<GamepadState, 16> slots{};
        slots[0].Connected = true;
        slots[0].Axes[usize(GamepadAxis::LeftX)] = 1.0f;
        slots[0].Buttons[usize(GamepadButton::A)] = true;
        slots[1].Connected = true;
        slots[1].Axes[usize(GamepadAxis::LeftX)] = -1.0f;
        input.IngestGamepadStates(slots);
    }

    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        return registry;
    }

    // A SystemContext over the given headless Input and never-dereferenced asset storage. The
    // DeviceAssignmentSystem reads only the Input's connected-pad set.
    struct ContextStorage
    {
        Input& HeadlessInput;
        alignas(16) unsigned char AssetsBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
            };
        }
    };
}

TEST_CASE("A keyboard seat sees the keyboard and no pad")
{
    Input input(nullptr);
    input.BeginFrame();
    SeedDevices(input);

    const SeatInputView view{
        input,
        SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false}};
    const ResolvedContext context = MixedContext();
    const std::array active{context};

    const ActionState state = ResolveActions(active, view, {});
    // W drives Move.y through the keyboard arm; no pad, so Move.x and Jump stay neutral.
    CHECK(state.GetValue(Move).y == doctest::Approx(1.0f));
    CHECK(state.GetValue(Move).x == doctest::Approx(0.0f));
    CHECK_FALSE(state.IsHeld(Jump));
}

TEST_CASE("A pad-0 seat sees pad 0 and no keyboard")
{
    Input input(nullptr);
    input.BeginFrame();
    SeedDevices(input);

    const SeatInputView view{
        input,
        SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId(0), .WantsGamepad = false}};
    const ResolvedContext context = MixedContext();
    const std::array active{context};

    const ActionState state = ResolveActions(active, view, {});
    // Pad 0's stick +X and A drive Move.x and Jump; the keyboard is gated off, so Move.y is zero.
    CHECK(state.GetValue(Move).x == doctest::Approx(1.0f));
    CHECK(state.GetValue(Move).y == doctest::Approx(0.0f));
    CHECK(state.IsHeld(Jump));
}

TEST_CASE("A pad-1 seat sees neither the keyboard nor pad 0")
{
    Input input(nullptr);
    input.BeginFrame();
    SeedDevices(input);

    const SeatInputView view{
        input,
        SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId(1), .WantsGamepad = false}};
    const ResolvedContext context = MixedContext();
    const std::array active{context};

    const ActionState state = ResolveActions(active, view, {});
    // Pad 1's stick pushes −X, distinct from pad 0's +X; the keyboard is gated off. Neither pad 0
    // nor the keyboard leaks in.
    CHECK(state.GetValue(Move).x == doctest::Approx(-1.0f));
    CHECK(state.GetValue(Move).y == doctest::Approx(0.0f));
    CHECK_FALSE(state.IsHeld(Jump));
}

TEST_CASE("The same binding set through two seat views yields two different ActionStates")
{
    Input input(nullptr);
    input.BeginFrame();
    SeedDevices(input);

    const ResolvedContext context = MixedContext();
    const std::array active{context};

    // Seat A: keyboard only. Seat B: pad 0 only. Same bindings, same snapshot, different views.
    const SeatInputView viewA{
        input,
        SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false}};
    const SeatInputView viewB{
        input,
        SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId(0), .WantsGamepad = false}};

    const ActionState a = ResolveActions(active, viewA, {});
    const ActionState b = ResolveActions(active, viewB, {});

    // Divergence is the core proof: A reads Move.y from the keyboard, B reads Move.x + Jump from
    // pad 0 — the routing produces per-seat state from one shared snapshot.
    CHECK(a.GetValue(Move).y == doctest::Approx(1.0f));
    CHECK(a.GetValue(Move).x == doctest::Approx(0.0f));
    CHECK_FALSE(a.IsHeld(Jump));

    CHECK(b.GetValue(Move).x == doctest::Approx(1.0f));
    CHECK(b.GetValue(Move).y == doctest::Approx(0.0f));
    CHECK(b.IsHeld(Jump));

    CHECK(a.GetValue(Move) != b.GetValue(Move));
}

TEST_CASE("DeviceAssignmentSystem fills the first WantsGamepad seat and skips a non-opted-in seat")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // Seat 0: does not want a pad. Seat 1: wants one, unassigned. The connected pad must reach
    // seat 1, skipping seat 0, in deterministic creation order.
    const Entity seat0 = scene->CreateEntity();
    scene->Add<SeatInput>(
        seat0,
        SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false});
    const Entity seat1 = scene->CreateEntity();
    scene->Add<SeatInput>(
        seat1,
        SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId::None, .WantsGamepad = true});

    Input input(nullptr);
    input.BeginFrame();
    GamepadState pad;
    pad.Connected = true;
    IngestPad(input, 0, pad);

    DeviceAssignmentSystem assignment;
    ContextStorage storage{.HeadlessInput = input};
    assignment.OnUpdate(*scene, 0.016f, storage.Make());

    CHECK(scene->Get<SeatInput>(seat0).Gamepad == GamepadId::None);
    CHECK(scene->Get<SeatInput>(seat1).Gamepad == GamepadId(0));

    // On disconnect the assigned seat is cleared back to None.
    input.BeginFrame();
    input.IngestGamepadStates(std::array<GamepadState, 16>{});
    assignment.OnUpdate(*scene, 0.016f, storage.Make());

    CHECK(scene->Get<SeatInput>(seat1).Gamepad == GamepadId::None);
}

TEST_CASE("DeviceAssignmentSystem respects a level-authored pad slot")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A seat authored onto pad slot 1 while wanting a pad: the policy fills only None slots, so
    // the authored slot survives even though pad 0 is the connected one.
    const Entity seat = scene->CreateEntity();
    scene->Add<SeatInput>(
        seat, SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId(1), .WantsGamepad = true});

    Input input(nullptr);
    input.BeginFrame();
    GamepadState pad;
    pad.Connected = true;
    IngestPad(input, 1, pad);

    DeviceAssignmentSystem assignment;
    ContextStorage storage{.HeadlessInput = input};
    assignment.OnUpdate(*scene, 0.016f, storage.Make());

    CHECK(scene->Get<SeatInput>(seat).Gamepad == GamepadId(1));
}
