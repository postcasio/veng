// Per-seat device routing: the SeatInputView filters the shared Veng::Input snapshot to one
// seat's assigned devices AND region-gates the pointer, so two seats resolve to distinct
// PlayerInputs, only the pointer's current owner reads the mouse, and the DeviceAssignmentSystem
// auto-assigns a connected pad to the first opted-in seat and clears it on disconnect. Headless
// (Input over a null window), so it runs with no ICD; the view's device gating, the pointer
// point→seat selection (SelectPointerOwner over quadrant regions), and the assignment policy are
// what is under test. The pointer hit-test math is planset-31's already-tested WindowToViewport;
// this drives the selection + arm gating over regions directly, since a real Viewport needs a
// Context.

#include <doctest/doctest.h>

#include <array>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Input/Actions.h>
#include <Veng/Input/RawInput.h>
#include <Veng/InputRouter.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/DeviceAssignmentSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

using namespace Veng;
using Veng::Renderer::ViewportRegion;

namespace
{
    // Action ids for the fixture — arbitrary distinct non-zero values.
    constexpr ActionId Move{0xA1};
    constexpr ActionId Jump{0xB2};
    constexpr ActionId Aim{0xC3};

    // Keyboard control code the bindings and the injected key event agree on.
    constexpr u32 KeyW = u32(Key::W);

    // Two seat entities for the pointer fixture — arbitrary distinct non-null handles.
    constexpr Entity SeatA{.Index = 1, .Generation = 1};
    constexpr Entity SeatB{.Index = 2, .Generation = 1};

    // A 1280×720 window split into left/right quadrant regions, association order A then B.
    constexpr ViewportRegion LeftRegion{.Offset = {0, 0}, .Extent = {640, 720}};
    constexpr ViewportRegion RightRegion{.Offset = {640, 0}, .Extent = {640, 720}};

    std::array<PointerRegionSeat, 2> QuadrantRegions()
    {
        return {PointerRegionSeat{.Region = LeftRegion, .Viewer = SeatA},
                PointerRegionSeat{.Region = RightRegion, .Viewer = SeatB}};
    }

    // A Look → mouse delta + Aim → mouse position context, so the resolved state distinguishes the
    // raw look delta (sensitivity-invariant) from the region-local pointer position.
    ResolvedContext PointerContext()
    {
        return ResolvedContext{
            .Actions = {InputAction{.Id = Move, .Name = "Look", .Kind = ActionKind::Axis2D},
                        InputAction{.Id = Aim, .Name = "Aim", .Kind = ActionKind::Axis2D}},
            .Bindings = {Binding{.Source = {.Device = InputDeviceType::MouseAxis,
                                            .Control = RawInput::MouseAxisX},
                                 .Action = Move,
                                 .Axis = AxisComponent::X,
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::MouseAxis,
                                            .Control = SeatInputView::MousePositionX},
                                 .Action = Aim,
                                 .Axis = AxisComponent::X,
                                 .Scale = 1.0f},
                         Binding{.Source = {.Device = InputDeviceType::MouseAxis,
                                            .Control = SeatInputView::MousePositionY},
                                 .Action = Aim,
                                 .Axis = AxisComponent::Y,
                                 .Scale = 1.0f}}};
    }

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
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
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
        SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false},
        PointerRouting{}, Entity::Null};
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
        SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId(0), .WantsGamepad = false},
        PointerRouting{}, Entity::Null};
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
        SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId(1), .WantsGamepad = false},
        PointerRouting{}, Entity::Null};
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
        SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false},
        PointerRouting{}, Entity::Null};
    const SeatInputView viewB{
        input,
        SeatInput{.UsesKeyboardMouse = false, .Gamepad = GamepadId(0), .WantsGamepad = false},
        PointerRouting{}, Entity::Null};

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

TEST_CASE("A free pointer resolves to the seat whose quadrant it is over, viewport-local")
{
    const std::array regions = QuadrantRegions();

    // A point in the left quadrant resolves to seat A; its region-local position is the point minus
    // the region offset (offset zero here, so the window point unchanged).
    const PointerRouting left = SelectPointerOwner(regions, ivec2(100, 200));
    CHECK(left.Owner == SeatA);
    CHECK(left.LocalPosition.x == doctest::Approx(100.0f));
    CHECK(left.LocalPosition.y == doctest::Approx(200.0f));

    // A point in the right quadrant resolves to seat B; its region-local position subtracts the
    // 640-pixel offset, so the same window x reads a smaller local x — position is viewport-local.
    const PointerRouting right = SelectPointerOwner(regions, ivec2(700, 200));
    CHECK(right.Owner == SeatB);
    CHECK(right.LocalPosition.x == doctest::Approx(60.0f));
    CHECK(right.LocalPosition.y == doctest::Approx(200.0f));

    // A point outside every region owns no seat.
    const PointerRouting outside = SelectPointerOwner(regions, ivec2(2000, 200));
    CHECK(outside.Owner == Entity::Null);
}

TEST_CASE("Overlapping regions resolve topmost: the later association wins the pointer")
{
    // A full-window base viewport (seat A) with a full-window overlay viewport (seat B) associated
    // after it — the stacked-overlay shape, where the later-registered viewport composites on top.
    constexpr ViewportRegion window{.Offset = {0, 0}, .Extent = {1280, 720}};
    const std::array regions{PointerRegionSeat{.Region = window, .Viewer = SeatA},
                             PointerRegionSeat{.Region = window, .Viewer = SeatB}};

    // The pointer routes to the overlay's seat — the topmost viewport the user sees — not the
    // covered base's, even though both regions contain the point.
    const PointerRouting routing = SelectPointerOwner(regions, ivec2(100, 200));
    CHECK(routing.Owner == SeatB);
    CHECK(routing.LocalPosition.x == doctest::Approx(100.0f));
    CHECK(routing.LocalPosition.y == doctest::Approx(200.0f));

    // A sub-region overlay leaves the base owning the pointer outside the overlay's region.
    constexpr ViewportRegion inset{.Offset = {320, 180}, .Extent = {640, 360}};
    const std::array insetRegions{PointerRegionSeat{.Region = window, .Viewer = SeatA},
                                  PointerRegionSeat{.Region = inset, .Viewer = SeatB}};
    CHECK(SelectPointerOwner(insetRegions, ivec2(400, 300)).Owner == SeatB);
    CHECK(SelectPointerOwner(insetRegions, ivec2(100, 100)).Owner == SeatA);
}

TEST_CASE("A seat reads the pointer only while it owns the quadrant, position local + delta raw")
{
    // Seed a known raw window delta (+15, +7) and a final window position in the right quadrant:
    // first move seeds the position, BeginFrame resets the delta, the second move sets both.
    Input input(nullptr);
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(685, 193)});
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(700, 200)});

    const std::array regions = QuadrantRegions();
    const PointerRouting routing = SelectPointerOwner(regions, ivec2(700, 200));
    REQUIRE(routing.Owner == SeatB);

    const ResolvedContext context = PointerContext();
    const std::array active{context};

    const SeatInput keyboardSeat{
        .UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false};

    // Seat B owns the pointer: it reads the region-local position (Aim) and the RAW window delta
    // (Look) — the delta is untouched by the region so look stays sensitivity-invariant.
    const SeatInputView ownerView{input, keyboardSeat, routing, SeatB};
    const ActionState owner = ResolveActions(active, ownerView, {});
    CHECK(owner.GetValue(Aim).x == doctest::Approx(60.0f));
    CHECK(owner.GetValue(Aim).y == doctest::Approx(200.0f));
    CHECK(owner.GetValue(Move).x == doctest::Approx(15.0f));

    // Seat A does not own the pointer this frame (the cursor is in B's quadrant): neutral mouse,
    // even though it holds the keyboard/mouse. A quadrant the cursor left reads no pointer.
    const SeatInputView otherView{input, keyboardSeat, routing, SeatA};
    const ActionState other = ResolveActions(active, otherView, {});
    CHECK(other.GetValue(Aim).x == doctest::Approx(0.0f));
    CHECK(other.GetValue(Aim).y == doctest::Approx(0.0f));
    CHECK(other.GetValue(Move).x == doctest::Approx(0.0f));
}

TEST_CASE("A UsesKeyboardMouse=false seat reads neutral mouse even over its own region")
{
    Input input(nullptr);
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(85, 193)});
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(100, 200)});

    const std::array regions = QuadrantRegions();
    const PointerRouting routing = SelectPointerOwner(regions, ivec2(100, 200));
    REQUIRE(routing.Owner == SeatA);

    const ResolvedContext context = PointerContext();
    const std::array active{context};

    // Seat A owns the region the cursor is over, but does not hold the keyboard/mouse — the
    // pointer requires BOTH the gate and region ownership, so it reads neutral mouse.
    const SeatInput padSeat{
        .UsesKeyboardMouse = false, .Gamepad = GamepadId(0), .WantsGamepad = false};
    const SeatInputView view{input, padSeat, routing, SeatA};
    const ActionState state = ResolveActions(active, view, {});
    CHECK(state.GetValue(Aim).x == doctest::Approx(0.0f));
    CHECK(state.GetValue(Aim).y == doctest::Approx(0.0f));
    CHECK(state.GetValue(Move).x == doctest::Approx(0.0f));
}

TEST_CASE("Scroll axes read the wheel delta, region-gated like the other pointer arms")
{
    // Seed a wheel delta and park the cursor in seat B's quadrant.
    Input input(nullptr);
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(700, 200)});
    input.ApplyEvent(MouseScrolledEvent{vec2(0.5f, -2.0f)});

    const std::array regions = QuadrantRegions();
    const PointerRouting routing = SelectPointerOwner(regions, ivec2(700, 200));
    REQUIRE(routing.Owner == SeatB);

    const ResolvedContext context{
        .Actions = {InputAction{.Id = Move, .Name = "Zoom", .Kind = ActionKind::Axis2D}},
        .Bindings = {Binding{.Source = {.Device = InputDeviceType::MouseAxis,
                                        .Control = RawInput::MouseScrollX},
                             .Action = Move,
                             .Axis = AxisComponent::X,
                             .Scale = 1.0f},
                     Binding{.Source = {.Device = InputDeviceType::MouseAxis,
                                        .Control = RawInput::MouseScrollY},
                             .Action = Move,
                             .Axis = AxisComponent::Y,
                             .Scale = 1.0f}}};
    const std::array active{context};

    // The shared adapter reads the wheel unconditionally.
    const RawInput raw{input};
    const ActionState shared = ResolveActions(active, raw, {});
    CHECK(shared.GetValue(Move).x == doctest::Approx(0.5f));
    CHECK(shared.GetValue(Move).y == doctest::Approx(-2.0f));

    const SeatInput keyboardSeat{
        .UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false};

    // The pointer's owner reads the wheel; a seat that does not own the pointer reads neutral.
    const SeatInputView ownerView{input, keyboardSeat, routing, SeatB};
    const ActionState owner = ResolveActions(active, ownerView, {});
    CHECK(owner.GetValue(Move).x == doctest::Approx(0.5f));
    CHECK(owner.GetValue(Move).y == doctest::Approx(-2.0f));

    const SeatInputView otherView{input, keyboardSeat, routing, SeatA};
    const ActionState other = ResolveActions(active, otherView, {});
    CHECK(other.GetValue(Move).x == doctest::Approx(0.0f));
    CHECK(other.GetValue(Move).y == doctest::Approx(0.0f));
}

TEST_CASE("A captured cursor routes wholly to the single keyboard seat regardless of position")
{
    // Under capture the router skips the region hit-test and marks the single keyboard/mouse seat
    // as owner; look reads raw delta and position is unused. Build the captured routing directly
    // (owner = seat A, no position), the branch InputRouter::ResolvePointer takes when captured.
    Input input(nullptr);
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(0, 0)});
    input.BeginFrame();
    input.ApplyEvent(MouseMovedEvent{vec2(15, 7)});

    const PointerRouting captured{.Owner = SeatA, .LocalPosition = {}};

    const ResolvedContext context = PointerContext();
    const std::array active{context};

    const SeatInput keyboardSeat{
        .UsesKeyboardMouse = true, .Gamepad = GamepadId::None, .WantsGamepad = false};

    // Seat A is the captured owner: it reads the raw look delta, position stays neutral (unused
    // under capture — the OS position is invisible and unbounded).
    const SeatInputView ownerView{input, keyboardSeat, captured, SeatA};
    const ActionState owner = ResolveActions(active, ownerView, {});
    CHECK(owner.GetValue(Move).x == doctest::Approx(15.0f));
    CHECK(owner.GetValue(Aim).x == doctest::Approx(0.0f));

    // Every other seat reads neutral mouse under capture, wherever the OS cursor is.
    const SeatInputView otherView{input, keyboardSeat, captured, SeatB};
    const ActionState other = ResolveActions(active, otherView, {});
    CHECK(other.GetValue(Move).x == doctest::Approx(0.0f));
    CHECK(other.GetValue(Aim).x == doctest::Approx(0.0f));
}
