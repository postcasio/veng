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

    // A scripted raw input surface: keys down are a set, axes are a small map. Neutral by
    // default (empty), modelling the headless snapshot.
    struct FakeRawInput final : RawInputView
    {
        vector<u32> KeysDown;
        vector<std::pair<u32, f32>> Axes;

        [[nodiscard]] bool IsKeyDown(u32 code) const override
        {
            return std::ranges::find(KeysDown, code) != KeysDown.end();
        }

        [[nodiscard]] bool IsButtonDown(InputDeviceType, u32) const override { return false; }

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
