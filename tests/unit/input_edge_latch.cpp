// Fixed-timestep input edge-latch cases. Under the accumulator a frame can run zero Sim ticks
// (frame rate above the tick rate), so a pressed edge must survive until a tick-running frame reads
// it: BeginFrame(rollEdges) rolls the edges only when the previous frame consumed them. Input is
// headless (a null window leaves the neutral state), and ApplyEvent injects fabricated events, so
// the latch is exercisable with no device — the same seam a synthetic-input driver feeds through.

#include <doctest/doctest.h>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>

using namespace Veng;

namespace
{
    void Press(Input& input, const Key key)
    {
        const KeyPressedEvent event(key, 0, 0);
        input.ApplyEvent(event);
    }

    void Release(Input& input, const Key key)
    {
        const KeyReleasedEvent event(key, 0, 0);
        input.ApplyEvent(event);
    }

    void Press(Input& input, const MouseButton button)
    {
        const MouseButtonPressedEvent event(button, 0);
        input.ApplyEvent(event);
    }

    void Release(Input& input, const MouseButton button)
    {
        const MouseButtonReleasedEvent event(button, 0);
        input.ApplyEvent(event);
    }
}

TEST_CASE(
    "input latch: a held press on a zero-tick frame survives until a tick-running frame reads it")
{
    Input input(nullptr);

    // A press arrives on a frame that runs no tick. It is observable as an edge but not yet consumed.
    input.BeginFrame(true);
    Press(input, Key::Space);
    CHECK(input.WasKeyPressed(Key::Space));

    // Following zero-tick frames hold the edge (rollEdges = false): the press is not dropped while no
    // tick has read it, and the key stays down.
    input.BeginFrame(false);
    CHECK(input.WasKeyPressed(Key::Space));
    CHECK(input.IsKeyDown(Key::Space));

    input.BeginFrame(false);
    CHECK(input.WasKeyPressed(Key::Space));

    // A tick-running frame consumes the edge, so the next frame rolls (rollEdges = true) and the edge
    // clears — read exactly once across the latched frames, never doubled onto a later frame.
    input.BeginFrame(true);
    CHECK_FALSE(input.WasKeyPressed(Key::Space));
    CHECK(input.IsKeyDown(Key::Space));
}

TEST_CASE("input latch: held state is unaffected by the roll gate")
{
    Input input(nullptr);

    input.BeginFrame(true);
    Press(input, Key::A);
    CHECK(input.IsKeyDown(Key::A));

    // Across several held (non-rolling) frames the down state persists regardless of the gate.
    input.BeginFrame(false);
    CHECK(input.IsKeyDown(Key::A));
    input.BeginFrame(false);
    CHECK(input.IsKeyDown(Key::A));
    input.BeginFrame(true);
    CHECK(input.IsKeyDown(Key::A));
}

TEST_CASE("input latch: rolling every frame gives the ordinary one-frame press and release edges")
{
    Input input(nullptr);

    // The steady case (frame rate == tick rate): every frame ran a tick, so every BeginFrame rolls
    // and press/release are single-frame edges, identical to the pre-accumulator behavior.
    input.BeginFrame(true);
    Press(input, Key::Space);
    CHECK(input.WasKeyPressed(Key::Space));
    CHECK(input.IsKeyDown(Key::Space));

    input.BeginFrame(true);
    CHECK_FALSE(input.WasKeyPressed(Key::Space));
    CHECK(input.IsKeyDown(Key::Space));

    Release(input, Key::Space);
    CHECK(input.WasKeyReleased(Key::Space));
    CHECK_FALSE(input.IsKeyDown(Key::Space));

    input.BeginFrame(true);
    CHECK_FALSE(input.WasKeyReleased(Key::Space));
}

TEST_CASE("input latch: a press and release within one latched window still reads down at the tick")
{
    Input input(nullptr);

    // A tap (press then release) that lands entirely across zero-tick frames must not vanish: the
    // action layer reads the level (IsKeyDown), so the key has to stay down until a tick-running frame
    // observes it, then release. Without the deferral the level would fall low at every tick and the
    // action would never fire — the "a click only registers if held" symptom.
    input.BeginFrame(true);
    Press(input, Key::Space);

    // Release arrives on a later zero-tick frame in the same window: it is deferred, the level stays
    // down so any tick this window would see the press.
    input.BeginFrame(false);
    Release(input, Key::Space);
    CHECK(input.IsKeyDown(Key::Space));

    // Still latched, still down — the frame a tick actually runs on reads the press.
    input.BeginFrame(false);
    CHECK(input.IsKeyDown(Key::Space));

    // The tick consumed the window, so this frame rolls and the deferred release applies: down clears
    // and the released edge fires exactly once, a tick after the press was seen.
    input.BeginFrame(true);
    CHECK_FALSE(input.IsKeyDown(Key::Space));
    CHECK(input.WasKeyReleased(Key::Space));

    input.BeginFrame(true);
    CHECK_FALSE(input.WasKeyReleased(Key::Space));
}

TEST_CASE("input latch: a mouse tap within one latched window survives to the tick, then releases")
{
    Input input(nullptr);

    // The galaxy-map select-click path: a quick left-button tap between two Sim ticks must be seen
    // down by a tick (so the orbit action triggers) and released on a later tick (so it fires the
    // click), exactly as the keyboard tap above.
    input.BeginFrame(true);
    Press(input, MouseButton::Left);

    input.BeginFrame(false);
    Release(input, MouseButton::Left);
    CHECK(input.IsMouseButtonDown(MouseButton::Left));

    input.BeginFrame(false);
    CHECK(input.IsMouseButtonDown(MouseButton::Left));

    input.BeginFrame(true);
    CHECK_FALSE(input.IsMouseButtonDown(MouseButton::Left));
    CHECK(input.WasMouseButtonReleased(MouseButton::Left));
}

TEST_CASE("input latch: a key held across a roll releases immediately, not deferred")
{
    Input input(nullptr);

    // The deferral only applies to a tap whose press has not yet crossed a roll. A key pressed in one
    // window and released in a later one has already been observed down by ticks, so its release takes
    // effect at once.
    input.BeginFrame(true);
    Press(input, Key::A);

    // A tick ran, so this frame rolls (the press is now committed) and the key is still held.
    input.BeginFrame(true);
    CHECK(input.IsKeyDown(Key::A));

    // Release in this later window applies immediately — not held to the next roll.
    Release(input, Key::A);
    CHECK_FALSE(input.IsKeyDown(Key::A));
    CHECK(input.WasKeyReleased(Key::A));
}
