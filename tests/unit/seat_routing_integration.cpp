// The two-seat routing integration guard: two Viewer seats sharing one Veng::Input snapshot and
// one per-frame PointerRouting resolve to distinct PlayerInputs, proving the device-id and
// pointer-region routing paths compose under the both-gates and capture rules with no human, no
// window, and no golden. Seat A holds the keyboard/mouse (no pad); seat B holds pad 0 (no
// keyboard/mouse). Two left/right regions are associated in the router's order. A scripted
// keyboard + pad-slot-0 state and scripted free-cursor positions drive both seats' SeatInputView
// through ResolveActions each step; the assertions are that each seat carries only its own
// devices, that the pointer follows the cursor's region across the seam, and that a captured
// cursor routes wholly to the keyboard seat. Headless (Input over a null window), device-free
// like seat_routing — it drives the point→seat selection (SelectPointerOwner) over quadrant
// regions rather than a real Viewport, which would need a Context.

#include <doctest/doctest.h>

#include <array>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/RawInput.h>
#include <Veng/InputRouter.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

using namespace Veng;
using Veng::Renderer::ViewportRegion;

namespace
{
    // Action ids for the fixture — arbitrary distinct non-zero values.
    constexpr ActionId Move{0x11};
    constexpr ActionId Jump{0x22};
    constexpr ActionId Look{0x33};
    constexpr ActionId Aim{0x44};

    // Keyboard control code the binding and the injected key event agree on.
    constexpr u32 KeyW = u32(Key::W);

    // Two seat entities — arbitrary distinct non-null handles standing in for two Viewer seats.
    constexpr Entity SeatA{.Index = 1, .Generation = 1};
    constexpr Entity SeatB{.Index = 2, .Generation = 1};

    // A 1280×720 window split into left/right regions, association order A then B — the router's
    // hit-test priority order for the quadrant reconfigure.
    constexpr ViewportRegion LeftRegion{.Offset = {0, 0}, .Extent = {640, 720}};
    constexpr ViewportRegion RightRegion{.Offset = {640, 0}, .Extent = {640, 720}};

    std::array<PointerRegionSeat, 2> Quadrants()
    {
        return {PointerRegionSeat{.Region = LeftRegion, .Viewer = SeatA},
                PointerRegionSeat{.Region = RightRegion, .Viewer = SeatB}};
    }

    // The seats' shared gameplay context: W → Move.y and A → Jump on the keyboard/pad, left-stick-X
    // → Move.x on the pad, raw mouse delta → Look, and viewport-local mouse position → Aim. One
    // binding set feeds both seats, so any divergence is the routing, not the bindings.
    ResolvedContext GameplayContext()
    {
        return ResolvedContext{
            .Actions = {InputAction{.Id = Move, .Name = "Move", .Kind = ActionKind::Axis2D},
                        InputAction{.Id = Jump, .Name = "Jump", .Kind = ActionKind::Button},
                        InputAction{.Id = Look, .Name = "Look", .Kind = ActionKind::Axis2D},
                        InputAction{.Id = Aim, .Name = "Aim", .Kind = ActionKind::Axis2D}},
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
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::MouseAxis,
                                            .Control = RawInput::MouseAxisX},
                                 .Action = Look,
                                 .Axis = AxisComponent::X,
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::MouseAxis,
                                            .Control = SeatInputView::MousePositionX},
                                 .Action = Aim,
                                 .Axis = AxisComponent::X,
                                 .Scale = 1.0f}}};
    }

    // Seat A: keyboard + mouse, no pad. The keyboard human seat.
    constexpr SeatInput KeyboardSeat{
        .UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false};

    // Seat B: pad 0, no keyboard/mouse. The controller guest seat.
    constexpr SeatInput PadSeat{
        .UsesKeyboardMouse = false, .Gamepad = GamepadId(0), .WantsGamepad = false};

    // Holds W, pushes pad 0's left stick +X, and presses pad 0's A — the shared device state both
    // seats' filtered views narrow. Pad 1 pushes the opposite way to prove no other pad leaks in.
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
}

TEST_CASE("Two seats sharing one snapshot resolve to their own devices only")
{
    Input input(nullptr);
    input.BeginFrame();
    SeedDevices(input);

    // Free cursor over seat A's left region: the router resolves seat A as the pointer owner.
    const std::array regions = Quadrants();
    const PointerRouting pointer = SelectPointerOwner(regions, ivec2(100, 200));
    REQUIRE(pointer.Owner == SeatA);

    const ResolvedContext context = GameplayContext();
    const std::array active{context};

    // Both seats resolve against ONE shared Input snapshot and ONE pointer routing — the composed
    // path InputMappingSystem runs per seat.
    const SeatInputView viewA{input, KeyboardSeat, pointer, SeatA};
    const SeatInputView viewB{input, PadSeat, pointer, SeatB};
    const ActionState a = ResolveActions(active, viewA, {});
    const ActionState b = ResolveActions(active, viewB, {});

    // Seat A: keyboard drives Move.y; no pad, so Move.x and Jump stay neutral.
    CHECK(a.GetValue(Move).y == doctest::Approx(1.0f));
    CHECK(a.GetValue(Move).x == doctest::Approx(0.0f));
    CHECK_FALSE(a.IsHeld(Jump));

    // Seat B: pad 0 drives Move.x and Jump; the keyboard is gated off, so Move.y stays neutral,
    // and pad 1's opposite stick never leaks in.
    CHECK(b.GetValue(Move).x == doctest::Approx(1.0f));
    CHECK(b.GetValue(Move).y == doctest::Approx(0.0f));
    CHECK(b.IsHeld(Jump));

    // The two seats diverge from one shared snapshot — the whole point of the routing.
    CHECK(a.GetValue(Move) != b.GetValue(Move));
}

TEST_CASE("The pointer follows the cursor's region and never reaches the pad seat")
{
    // Seed a known raw window delta (+15,+7) so Look is non-neutral, then land the cursor in seat
    // A's left region: first move seeds the position, BeginFrame resets the delta, the second sets
    // both.
    Input input(nullptr);
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(85, 193)});
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(100, 200)});

    const std::array regions = Quadrants();
    const PointerRouting pointer = SelectPointerOwner(regions, ivec2(100, 200));
    REQUIRE(pointer.Owner == SeatA);

    const ResolvedContext context = GameplayContext();
    const std::array active{context};

    // Seat A owns the region the cursor is over AND holds the keyboard/mouse: it reads the raw look
    // delta and the region-local Aim position.
    const SeatInputView viewA{input, KeyboardSeat, pointer, SeatA};
    const ActionState a = ResolveActions(active, viewA, {});
    CHECK(a.GetValue(Look).x == doctest::Approx(15.0f));
    CHECK(a.GetValue(Aim).x == doctest::Approx(100.0f));

    // Seat B is pad-only: even though the cursor is nowhere near its region, the both-gates rule
    // (UsesKeyboardMouse AND ownership) keeps its mouse neutral — a pad seat never reads the mouse.
    const SeatInputView viewB{input, PadSeat, pointer, SeatB};
    const ActionState b = ResolveActions(active, viewB, {});
    CHECK(b.GetValue(Look).x == doctest::Approx(0.0f));
    CHECK(b.GetValue(Aim).x == doctest::Approx(0.0f));
}

TEST_CASE("Moving the cursor across the seam transfers the look action to the region's owner")
{
    Input input(nullptr);

    const std::array regions = Quadrants();
    const ResolvedContext context = GameplayContext();
    const std::array active{context};

    // The cursor sweeps left → right → left across the seam. Only seat A is a keyboard/mouse seat,
    // so it is the only seat that can ever read the pointer; the assertion is that it reads it only
    // while it owns the region, and seat B stays pointer-neutral throughout.
    struct Step
    {
        ivec2 Cursor;
        Entity ExpectedOwner;
    };
    constexpr std::array steps{
        Step{.Cursor = {100, 200}, .ExpectedOwner = SeatA},
        Step{.Cursor = {700, 200}, .ExpectedOwner = SeatB},
        Step{.Cursor = {320, 360}, .ExpectedOwner = SeatA},
    };

    for (const Step& step : steps)
    {
        input.BeginFrame();
        input.ApplyEvent(MouseMovedEvent{vec2(step.Cursor)});

        const PointerRouting pointer = SelectPointerOwner(regions, step.Cursor);
        REQUIRE(pointer.Owner == step.ExpectedOwner);

        const SeatInputView viewA{input, KeyboardSeat, pointer, SeatA};
        const SeatInputView viewB{input, PadSeat, pointer, SeatB};
        const ActionState a = ResolveActions(active, viewA, {});
        const ActionState b = ResolveActions(active, viewB, {});

        // Seat A reads the region-local Aim position only on the frames it owns the region; a
        // frame its quadrant lost the cursor reads neutral (the region hit-test moved to seat B,
        // which cannot read the mouse).
        if (step.ExpectedOwner == SeatA)
        {
            const f32 expectedLocalX = static_cast<f32>(step.Cursor.x - LeftRegion.Offset.x);
            CHECK(a.GetValue(Aim).x == doctest::Approx(expectedLocalX));
        }
        else
        {
            CHECK(a.GetValue(Aim).x == doctest::Approx(0.0f));
        }

        // Seat B never reads the pointer, whichever region the cursor is over.
        CHECK(b.GetValue(Aim).x == doctest::Approx(0.0f));
    }
}

TEST_CASE("A captured cursor routes the pointer wholly to the keyboard seat regardless of position")
{
    // Under capture the router skips the region hit-test and names the single keyboard/mouse seat
    // as owner (look reads raw delta, position unused). Build that routing directly — the branch
    // InputRouter::ResolvePointer takes when captured — with the OS cursor scripted deep in seat
    // B's region to prove position is irrelevant.
    Input input(nullptr);
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(700, 200)});
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(715, 207)});

    const PointerRouting captured{.Owner = SeatA, .LocalPosition = {}};

    const ResolvedContext context = GameplayContext();
    const std::array active{context};

    // Seat A is the captured owner: it reads the raw look delta wherever the OS cursor sits, and
    // its position-based Aim stays neutral (position is unused under capture).
    const SeatInputView viewA{input, KeyboardSeat, captured, SeatA};
    const ActionState a = ResolveActions(active, viewA, {});
    CHECK(a.GetValue(Look).x == doctest::Approx(15.0f));
    CHECK(a.GetValue(Aim).x == doctest::Approx(0.0f));

    // Every other seat reads neutral mouse under capture, wherever the OS cursor is.
    const SeatInputView viewB{input, PadSeat, captured, SeatB};
    const ActionState b = ResolveActions(active, viewB, {});
    CHECK(b.GetValue(Look).x == doctest::Approx(0.0f));
    CHECK(b.GetValue(Aim).x == doctest::Approx(0.0f));
}
