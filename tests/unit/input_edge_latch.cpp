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
