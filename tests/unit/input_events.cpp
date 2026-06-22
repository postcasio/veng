// Event-dispatch and headless-input unit cases. EventDispatcher is pure logic (a
// type-tag compare + a cast), so its match/no-match/handled semantics test without a
// device. Input is glued to a Window for state, but its documented headless contract —
// a null window leaves the neutral all-zeros state the smoke/headless path depends on —
// is exercisable with no window and no driver.

#include <doctest/doctest.h>

#include <Veng/Event.h>
#include <Veng/Input.h>
#include <Veng/WindowEvents.h>

using namespace Veng;

TEST_CASE("EventDispatcher: a matching type calls the handler and records Handled")
{
    WindowResizeEvent event(1280, 720);
    EventDispatcher dispatcher(event);

    bool called = false;
    const bool matched = dispatcher.Dispatch<WindowResizeEvent>(
        [&](WindowResizeEvent& e)
        {
            called = true;
            // The event reaches the handler typed, carrying its payload.
            CHECK(e.GetWidth() == 1280);
            CHECK(e.GetHeight() == 720);
            return true; // handled
        });

    CHECK(matched);
    CHECK(called);
    CHECK(event.Handled);
}

TEST_CASE("EventDispatcher: a non-matching type leaves the handler and Handled untouched")
{
    WindowResizeEvent event(800, 600);
    EventDispatcher dispatcher(event);

    bool called = false;
    const bool matched = dispatcher.Dispatch<WindowCloseEvent>(
        [&](WindowCloseEvent&)
        {
            called = true;
            return true;
        });

    CHECK_FALSE(matched);
    CHECK_FALSE(called);
    CHECK_FALSE(event.Handled);
}

TEST_CASE("EventDispatcher: a matching handler that does not handle leaves Handled false")
{
    WindowCloseEvent event;
    EventDispatcher dispatcher(event);

    const bool matched =
        dispatcher.Dispatch<WindowCloseEvent>([](WindowCloseEvent&) { return false; });

    CHECK(matched);             // the type matched
    CHECK_FALSE(event.Handled); // but the handler declined to handle it
}

TEST_CASE("EventDispatcher: Handled OR-accumulates across dispatches")
{
    WindowResizeEvent event(640, 480);
    EventDispatcher dispatcher(event);

    // A non-handling dispatch first, then a handling one — Handled latches true.
    dispatcher.Dispatch<WindowResizeEvent>([](WindowResizeEvent&) { return false; });
    CHECK_FALSE(event.Handled);

    dispatcher.Dispatch<WindowResizeEvent>([](WindowResizeEvent&) { return true; });
    CHECK(event.Handled);

    // A later non-handling dispatch does not clear the latched flag.
    dispatcher.Dispatch<WindowResizeEvent>([](WindowResizeEvent&) { return false; });
    CHECK(event.Handled);
}

TEST_CASE("Event: the EVENT macro wires static and runtime type identity")
{
    const WindowResizeEvent resize(1, 1);
    const WindowCloseEvent close;

    CHECK(WindowResizeEvent::GetStaticType() == EventType::WindowResize);
    CHECK(resize.GetEventType() == EventType::WindowResize);
    CHECK(close.GetEventType() == EventType::WindowClose);

    // GetName is the type name; ToString defaults to it.
    CHECK(string(resize.GetName()) == "WindowResize");
    CHECK(resize.ToString() == "WindowResize");

    // A base-class reference reports the concrete runtime type.
    const Event& asBase = resize;
    CHECK(asBase.GetEventType() == EventType::WindowResize);
}

TEST_CASE("Input: a null window leaves the neutral all-zeros headless state")
{
    Input input(nullptr);

    // Pump several frames: the headless path must keep the zero-initialized state.
    input.BeginFrame();
    input.BeginFrame();

    CHECK_FALSE(input.IsKeyDown(Key::Space));
    CHECK_FALSE(input.WasKeyPressed(Key::W));
    CHECK_FALSE(input.WasKeyReleased(Key::W));

    CHECK_FALSE(input.IsMouseButtonDown(MouseButton::Left));
    CHECK_FALSE(input.WasMouseButtonPressed(MouseButton::Right));
    CHECK_FALSE(input.WasMouseButtonReleased(MouseButton::Middle));

    CHECK(input.GetMousePosition() == vec2{0, 0});
    CHECK(input.GetMouseDelta() == vec2{0, 0});
    CHECK(input.GetScrollDelta() == vec2{0, 0});

    // No window to capture against.
    CHECK_FALSE(input.IsMouseCaptured());
}

TEST_CASE("Input: an out-of-range key code is guarded, not an out-of-bounds read")
{
    Input input(nullptr);
    input.BeginFrame();

    // A key code past the bitset bound returns false rather than indexing out of range.
    const auto outOfRange = static_cast<Key>(60000);
    CHECK_FALSE(input.IsKeyDown(outOfRange));
    CHECK_FALSE(input.WasKeyPressed(outOfRange));
    CHECK_FALSE(input.WasKeyReleased(outOfRange));
}
