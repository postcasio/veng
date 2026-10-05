// The device-free half of the gamepad backend: the slot table that maps growing device connection
// ids onto stable slots, virtual pads, the focus mask, the touchpad's two delta cadences in
// Veng::Input, and the router's pacing of virtual-pad edits. No SDL and no window, so it runs in the
// default band.

#include <doctest/doctest.h>

#include <array>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Renderer/ViewportRegistry.h>

#include "Platform/GamepadSlots.h"

using namespace Veng;

namespace
{
    using PadStates = std::array<GamepadState, GamepadSlots::SlotCount>;

    /// Reads a device as a pad holding A with the left stick half right, the way the SDL layer
    /// fills a physical pad.
    void ReadFakeDevice(u64, GamepadState& state)
    {
        state.Connected = true;
        state.Buttons.fill(false);
        state.Axes.fill(0.0f);
        state.Buttons[usize(GamepadButton::A)] = true;
        state.Axes[usize(GamepadAxis::LeftX)] = 0.5f;
        state.Type = GamepadType::XboxOne;
        state.Name = "Fake Pad";
    }

    /// Counts the connect and disconnect events a slot table raised since the last take.
    struct EventCounts
    {
        usize Connected = 0;
        usize Disconnected = 0;
    };

    EventCounts TakeCounts(GamepadSlots& slots)
    {
        EventCounts counts;
        for (const Unique<Event>& event : slots.TakeEvents())
        {
            counts.Connected += event->GetEventType() == EventType::GamepadConnected ? 1 : 0;
            counts.Disconnected += event->GetEventType() == EventType::GamepadDisconnected ? 1 : 0;
        }
        return counts;
    }

    /// One frame of touchpad input: a finger down at a position, or lifted.
    void IngestTouch(Input& input, const bool down, const vec2 position)
    {
        PadStates states{};
        states[0].Connected = true;
        states[0].Buttons[usize(GamepadButton::TouchpadTouch)] = down;
        states[0].Axes[usize(GamepadAxis::TouchpadX)] = position.x;
        states[0].Axes[usize(GamepadAxis::TouchpadY)] = position.y;
        input.IngestGamepadStates(states);
    }
}

TEST_CASE("Appended gamepad controls leave every existing control index where it was")
{
    // Cooked input maps store these indices; the original controls must keep them.
    CHECK(u32(GamepadButton::A) == 0);
    CHECK(u32(GamepadButton::Guide) == 8);
    CHECK(u32(GamepadButton::DpadLeft) == 14);
    CHECK(u32(GamepadButton::Misc) == 15);
    CHECK(u32(GamepadButton::Count) == 22);
    CHECK(u32(GamepadAxis::LeftX) == 0);
    CHECK(u32(GamepadAxis::RightTrigger) == 5);
    CHECK(u32(GamepadAxis::TouchpadX) == 6);
    CHECK(u32(GamepadAxis::Count) == 10);
}

TEST_CASE("A connecting pad takes the lowest free slot and frees it on disconnect")
{
    GamepadSlots slots;
    slots.Attach(1000);
    CHECK_FALSE(slots.Find(1000).has_value());
    slots.Commit();
    REQUIRE(slots.Find(1000).has_value());
    CHECK(*slots.Find(1000) == GamepadId{0});
    CHECK(slots.GetDevice(GamepadId{0}) == optional<u64>(1000));
    CHECK(TakeCounts(slots).Connected == 1);

    slots.Detach(1000);
    CHECK_FALSE(slots.Find(1000).has_value());
    CHECK_FALSE(slots.GetDevice(GamepadId{0}).has_value());
    CHECK(TakeCounts(slots).Disconnected == 1);
}

TEST_CASE("A freed slot reads disconnected for a frame before another pad can take it")
{
    GamepadSlots slots;
    slots.Attach(1);
    slots.Commit();
    REQUIRE(slots.Find(1) == optional<GamepadId>(GamepadId{0}));

    // The pad reconnects within one frame under a new connection id: it may not land back in the
    // slot it just left, or a seat bound there would never see it go.
    slots.Detach(1);
    slots.Attach(2);
    slots.Commit();
    CHECK(slots.Find(2) == optional<GamepadId>(GamepadId{1}));

    PadStates states{};
    slots.Fill(states, true, ReadFakeDevice);
    CHECK_FALSE(states[0].Connected);
    CHECK(states[1].Connected);

    // A frame later the slot is free again, and the next pad takes it.
    slots.Attach(3);
    slots.Commit();
    CHECK(slots.Find(3) == optional<GamepadId>(GamepadId{0}));
}

TEST_CASE("A seventeenth pad waits for a slot and takes the first that frees")
{
    GamepadSlots slots;
    for (u64 device = 0; device < 17; ++device)
    {
        slots.Attach(device);
    }
    slots.Commit();
    CHECK(TakeCounts(slots).Connected == 16);
    CHECK_FALSE(slots.Find(16).has_value());

    slots.Detach(5);
    slots.Commit();
    CHECK_FALSE(slots.Find(16).has_value());

    slots.Commit();
    CHECK(slots.Find(16) == optional<GamepadId>(GamepadId{5}));
}

TEST_CASE("A virtual pad reads through Input exactly as a physical pad does, and disconnects")
{
    GamepadSlots slots;
    slots.Attach(77);
    slots.Commit();
    slots.ApplyVirtual(VirtualGamepadEvent::Connect(GamepadId{3}, GamepadType::PS5));
    slots.ApplyVirtual(VirtualGamepadEvent::SetButton(GamepadId{3}, GamepadButton::A, true));
    slots.ApplyVirtual(VirtualGamepadEvent::SetAxis(GamepadId{3}, GamepadAxis::LeftX, 0.5f));
    CHECK(slots.IsVirtual(GamepadId{3}));
    CHECK(TakeCounts(slots).Connected == 2);

    // A physical pad can never take a virtual pad's slot.
    slots.Attach(78);
    slots.Commit();
    CHECK(slots.Find(78) == optional<GamepadId>(GamepadId{1}));

    PadStates states{};
    slots.Fill(states, true, ReadFakeDevice);
    Input input(nullptr);
    input.BeginFrame();
    input.IngestGamepadStates(states);

    const GamepadId physical{0};
    const GamepadId virtualPad{3};
    CHECK(input.IsGamepadConnected(virtualPad));
    CHECK(input.IsGamepadButtonDown(virtualPad, GamepadButton::A) ==
          input.IsGamepadButtonDown(physical, GamepadButton::A));
    CHECK(input.WasGamepadButtonPressed(virtualPad, GamepadButton::A));
    CHECK(input.GetGamepadAxis(virtualPad, GamepadAxis::LeftX) ==
          input.GetGamepadAxis(physical, GamepadAxis::LeftX));
    CHECK(input.GetGamepadType(virtualPad) == GamepadType::PS5);
    CHECK_FALSE(input.GetGamepadName(virtualPad).empty());

    // An occupied slot refuses a second connect, and edits to a slot without a virtual pad do
    // nothing to the physical pad there.
    slots.ApplyVirtual(VirtualGamepadEvent::Connect(physical, GamepadType::PS4));
    slots.ApplyVirtual(VirtualGamepadEvent::SetButton(physical, GamepadButton::B, true));
    CHECK_FALSE(slots.IsVirtual(physical));

    slots.ApplyVirtual(VirtualGamepadEvent::Disconnect(virtualPad));
    CHECK(TakeCounts(slots).Disconnected == 1);
    slots.Fill(states, true, ReadFakeDevice);
    input.BeginFrame();
    input.IngestGamepadStates(states);
    CHECK_FALSE(input.IsGamepadConnected(virtualPad));
    CHECK_FALSE(input.IsGamepadButtonDown(physical, GamepadButton::B));
}

TEST_CASE("Unfocused, a physical pad stays connected and reads neutral; a virtual pad does not")
{
    GamepadSlots slots;
    slots.Attach(5);
    slots.Commit();
    slots.ApplyVirtual(VirtualGamepadEvent::Connect(GamepadId{1}, GamepadType::Standard));
    slots.ApplyVirtual(VirtualGamepadEvent::SetButton(GamepadId{1}, GamepadButton::A, true));

    PadStates states{};
    slots.Fill(states, false, ReadFakeDevice);
    CHECK(states[0].Connected);
    CHECK(states[0].Type == GamepadType::XboxOne);
    CHECK_FALSE(states[0].Buttons[usize(GamepadButton::A)]);
    CHECK(states[0].Axes[usize(GamepadAxis::LeftX)] == 0.0f);
    CHECK(states[1].Buttons[usize(GamepadButton::A)]);
}

TEST_CASE("A virtual pad's values are clamped to each control's range")
{
    GamepadSlots slots;
    slots.ApplyVirtual(VirtualGamepadEvent::Connect(GamepadId{0}, GamepadType::Standard));
    slots.ApplyVirtual(VirtualGamepadEvent::SetAxis(GamepadId{0}, GamepadAxis::LeftY, -3.0f));
    slots.ApplyVirtual(
        VirtualGamepadEvent::SetAxis(GamepadId{0}, GamepadAxis::RightTrigger, -1.0f));
    slots.ApplyVirtual(VirtualGamepadEvent::SetTouch(GamepadId{0}, true, vec2{1.5f, -0.5f}));
    slots.SetMotors(GamepadId{0}, GamepadMotors{.Low = 2.0f, .High = -1.0f});

    PadStates states{};
    slots.Fill(states, true, ReadFakeDevice);
    CHECK(states[0].Axes[usize(GamepadAxis::LeftY)] == -1.0f);
    CHECK(states[0].Axes[usize(GamepadAxis::RightTrigger)] == 0.0f);
    CHECK(states[0].Axes[usize(GamepadAxis::TouchpadX)] == 1.0f);
    CHECK(states[0].Axes[usize(GamepadAxis::TouchpadY)] == 0.0f);
    CHECK(slots.GetMotors(GamepadId{0}) == GamepadMotors{.Low = 1.0f, .High = 0.0f});

    // An empty slot holds no motor levels.
    slots.SetMotors(GamepadId{9}, GamepadMotors{.Low = 1.0f});
    CHECK(slots.GetMotors(GamepadId{9}) == GamepadMotors{});
}

TEST_CASE("Touchpad motion reads per frame and per tick across zero-, one- and three-tick frames")
{
    Input input(nullptr);
    const GamepadId pad{0};
    const auto frameDelta = [&]
    {
        return vec2{input.GetGamepadAxis(pad, GamepadAxis::TouchpadDeltaX),
                    input.GetGamepadAxis(pad, GamepadAxis::TouchpadDeltaY)};
    };
    const auto tickDelta = [&]
    {
        return vec2{input.GetSimGamepadAxis(pad, GamepadAxis::TouchpadDeltaX),
                    input.GetSimGamepadAxis(pad, GamepadAxis::TouchpadDeltaY)};
    };

    // The landing frame and the tick that first sees the finger both read no motion.
    IngestTouch(input, true, {0.2f, 0.5f});
    CHECK(frameDelta() == vec2{0.0f});
    input.BeginGamepadSimTick();
    CHECK(tickDelta() == vec2{0.0f});

    // A frame with no tick: the motion reads per frame and banks for the next tick.
    IngestTouch(input, true, {0.3f, 0.5f});
    CHECK(frameDelta().x == doctest::Approx(0.1f));

    // A one-tick frame: the tick reads both frames' motion.
    IngestTouch(input, true, {0.4f, 0.4f});
    CHECK(frameDelta().x == doctest::Approx(0.1f));
    input.BeginGamepadSimTick();
    CHECK(tickDelta().x == doctest::Approx(0.2f));
    CHECK(tickDelta().y == doctest::Approx(-0.1f));

    // A three-tick frame: the first tick takes the motion, the others read none, so the sum over
    // ticks is the travel.
    IngestTouch(input, true, {0.6f, 0.4f});
    vec2 sum{0.0f};
    for (int tick = 0; tick < 3; ++tick)
    {
        input.BeginGamepadSimTick();
        sum += tickDelta();
    }
    CHECK(sum.x == doctest::Approx(0.2f));
    CHECK(tickDelta() == vec2{0.0f});

    // A lift and a landing elsewhere is never motion, at either cadence.
    IngestTouch(input, false, {});
    input.BeginGamepadSimTick();
    IngestTouch(input, true, {0.9f, 0.9f});
    CHECK(frameDelta() == vec2{0.0f});
    input.BeginGamepadSimTick();
    CHECK(tickDelta() == vec2{0.0f});

    // Motion banked while nothing simulates is dropped, not delivered as one jump.
    IngestTouch(input, true, {0.5f, 0.9f});
    input.DropSimDeltas();
    IngestTouch(input, true, {0.5f, 0.9f});
    input.BeginGamepadSimTick();
    CHECK(tickDelta() == vec2{0.0f});
}

TEST_CASE("The router paces virtual-pad edits so a press and its release straddle a frame")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);
    vector<VirtualGamepadOp> applied;
    router.SetVirtualGamepadSink([&](const VirtualGamepadEvent& edit)
                                 { applied.push_back(edit.GetOp()); });

    const GamepadId slot{2};
    router.PostInjectedEvent(VirtualGamepadEvent::Connect(slot, GamepadType::Standard));
    router.PostInjectedEvent(VirtualGamepadEvent::SetButton(slot, GamepadButton::A, true));
    router.PostInjectedEvent(VirtualGamepadEvent::SetAxis(slot, GamepadAxis::LeftX, 1.0f));
    router.PostInjectedEvent(VirtualGamepadEvent::SetButton(slot, GamepadButton::A, false));
    router.PostInjectedEvent(VirtualGamepadEvent::Disconnect(slot));

    // The connect, press and axis apply together; the release reverses the press and waits.
    router.DrainInjectedEvents();
    CHECK(applied.size() == 3);
    // The release applies; the disconnect reverses the connect of an earlier frame, not this one.
    router.DrainInjectedEvents();
    CHECK(applied.size() == 5);
    CHECK(applied.back() == VirtualGamepadOp::Disconnect);
}
