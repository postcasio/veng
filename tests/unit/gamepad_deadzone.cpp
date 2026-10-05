// The device-level pad shaping: the pure radial stick and trigger zones, and their application as
// Veng::Input ingests a pad. Headless (Input over a null window), so no device or ICD is involved.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <glm/geometric.hpp>

#include <Veng/Input.h>

using namespace Veng;

namespace
{
    constexpr f32 Zone = 0.15f;

    // A single connected pad in slot 0, ingested into an Input.
    void IngestOnePad(Input& input, const GamepadState& pad)
    {
        std::array<GamepadState, Input::MaxGamepads> slots{};
        slots[0] = pad;
        input.BeginFrame();
        input.IngestGamepadStates(slots);
    }

    GamepadState ConnectedPad()
    {
        GamepadState pad;
        pad.Connected = true;
        return pad;
    }
}

TEST_CASE("A deflection keeps its direction through the radial stick zone")
{
    // Diagonals and off-axis angles at several lengths past the zone, so no axis clears first.
    f32 worstCross = 0.0f;
    f32 leastDot = 1.0f;
    for (const f32 degrees : {45.0f, 135.0f, 200.0f, 315.0f, 10.0f})
    {
        const f32 radians = glm::radians(degrees);
        const vec2 direction{std::cos(radians), std::sin(radians)};
        for (const f32 length : {0.2f, 0.5f, 0.9f, 1.0f})
        {
            const vec2 shaped = ShapeStick(direction * length, Zone);
            const vec2 unit = glm::normalize(shaped);
            worstCross =
                std::max(worstCross, std::abs(unit.x * direction.y - unit.y * direction.x));
            leastDot = std::min(leastDot, glm::dot(unit, direction));
        }
    }
    CHECK(worstCross < 1e-5f);
    CHECK(leastDot > 0.9999f);
}

TEST_CASE("The stick zone is continuous at its edge and keeps full deflection full")
{
    const vec2 diagonal = glm::normalize(vec2{1.0f, -1.0f});
    CHECK(ShapeStick(diagonal * Zone, Zone) == vec2{0.0f});
    CHECK(glm::length(ShapeStick(diagonal * (Zone + 1e-4f), Zone)) < 1e-3f);
    CHECK(glm::length(ShapeStick(diagonal, Zone)) == doctest::Approx(1.0f));
    // A square gate's corner reads full deflection rather than past it.
    CHECK(glm::length(ShapeStick(vec2{1.0f, 1.0f}, Zone)) == doctest::Approx(1.0f));

    // The shaped length never falls as the raw length grows.
    f32 previous = 0.0f;
    bool monotone = true;
    for (i32 step = 0; step <= 40; ++step)
    {
        const f32 length =
            glm::length(ShapeStick(diagonal * (static_cast<f32>(step) / 40.0f), Zone));
        monotone = monotone && length >= previous;
        previous = length;
    }
    CHECK(monotone);
}

TEST_CASE("The trigger zone reads zero inside, is continuous at its edge and full at a full pull")
{
    constexpr f32 TriggerZone = 0.05f;
    CHECK(ShapeTrigger(0.04f, TriggerZone) == 0.0f);
    CHECK(ShapeTrigger(TriggerZone, TriggerZone) == 0.0f);
    CHECK(ShapeTrigger(TriggerZone + 1e-4f, TriggerZone) < 1e-3f);
    CHECK(ShapeTrigger(1.0f, TriggerZone) == doctest::Approx(1.0f));
}

TEST_CASE("Resting noise inside the zones reads exactly zero, and the raw value stays readable")
{
    Input input(nullptr);
    GamepadState pad = ConnectedPad();
    pad.Axes[usize(GamepadAxis::LeftX)] = 0.06f;
    pad.Axes[usize(GamepadAxis::LeftY)] = -0.08f;
    pad.Axes[usize(GamepadAxis::RightX)] = -0.1f;
    pad.Axes[usize(GamepadAxis::RightTrigger)] = 0.03f;
    IngestOnePad(input, pad);

    const GamepadId slot{0};
    for (const GamepadAxis axis : {GamepadAxis::LeftX, GamepadAxis::LeftY, GamepadAxis::RightX,
                                   GamepadAxis::RightY, GamepadAxis::RightTrigger})
    {
        CHECK(input.GetGamepadAxis(slot, axis) == 0.0f);
        CHECK(input.GetSimGamepadAxis(slot, axis) == 0.0f);
    }
    CHECK(input.GetRawGamepadAxis(slot, GamepadAxis::LeftX) == 0.06f);
    CHECK(input.GetRawGamepadAxis(slot, GamepadAxis::RightTrigger) == 0.03f);
}

TEST_CASE("A touchpad position is not a deflection and passes through unshaped")
{
    Input input(nullptr);
    GamepadState pad = ConnectedPad();
    pad.Buttons[usize(GamepadButton::TouchpadTouch)] = true;
    pad.Axes[usize(GamepadAxis::TouchpadX)] = 0.1f;
    pad.Axes[usize(GamepadAxis::TouchpadY)] = 0.05f;
    IngestOnePad(input, pad);

    CHECK(input.GetGamepadAxis(GamepadId{0}, GamepadAxis::TouchpadX) == 0.1f);
    CHECK(input.GetGamepadAxis(GamepadId{0}, GamepadAxis::TouchpadY) == 0.05f);
}

TEST_CASE("A pad's zones are settable and revert to the defaults when it disconnects")
{
    Input input(nullptr);
    GamepadState pad = ConnectedPad();
    pad.Axes[usize(GamepadAxis::LeftX)] = 0.06f;
    IngestOnePad(input, pad);

    const GamepadId slot{0};
    input.SetGamepadDeadzones(slot, 0.0f, 0.0f);
    CHECK(input.GetGamepadDeadzones(slot) == GamepadDeadzones{.Stick = 0.0f, .Trigger = 0.0f});
    IngestOnePad(input, pad);
    CHECK(input.GetGamepadAxis(slot, GamepadAxis::LeftX) == doctest::Approx(0.06f));

    IngestOnePad(input, GamepadState{});
    IngestOnePad(input, pad);
    CHECK(input.GetGamepadDeadzones(slot) == GamepadDeadzones{});
    CHECK(input.GetGamepadAxis(slot, GamepadAxis::LeftX) == 0.0f);

    // An empty slot takes no zones.
    input.SetGamepadDeadzones(GamepadId{5}, 0.5f, 0.5f);
    CHECK(input.GetGamepadDeadzones(GamepadId{5}) == GamepadDeadzones{});
}
