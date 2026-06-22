// InputRouter focus-stack and routing unit cases. The router's logic is device-free: it
// folds events into the Input snapshot by focus and manages a focus stack with the Shift+Esc
// release chord. A null window (no cursor capture) and a null ImGui layer (no UI sink) leave
// exactly the snapshot-routing + focus behavior under test, with no GPU.

#include <doctest/doctest.h>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/WindowEvents.h>

using namespace Veng;

TEST_CASE("InputRouter: defaults to UI focus")
{
    Input input(nullptr);
    const InputRouter router(nullptr, input, nullptr);

    CHECK(router.GetFocus() == InputFocus::UI);
    CHECK_FALSE(router.IsGameplayFocused());
}

TEST_CASE("InputRouter: under UI focus an input event folds into the snapshot")
{
    Input input(nullptr);
    InputRouter router(nullptr, input, nullptr);

    input.BeginFrame();
    KeyPressedEvent press(Key::W, 0, 0);
    router.Dispatch(press);

    CHECK(input.IsKeyDown(Key::W));
    CHECK(input.WasKeyPressed(Key::W));
}

TEST_CASE("InputRouter: pushing and popping gameplay focus moves the stack top")
{
    Input input(nullptr);
    InputRouter router(nullptr, input, nullptr);

    router.PushFocus(InputFocus::Gameplay);
    CHECK(router.IsGameplayFocused());

    router.PopFocus();
    CHECK(router.GetFocus() == InputFocus::UI);

    // Popping past the implicit UI base is a no-op, never an underflow.
    router.PopFocus();
    CHECK(router.GetFocus() == InputFocus::UI);
}

TEST_CASE("InputRouter: under gameplay focus the game still receives input through the snapshot")
{
    Input input(nullptr);
    InputRouter router(nullptr, input, nullptr);

    router.PushFocus(InputFocus::Gameplay);

    input.BeginFrame();
    KeyPressedEvent press(Key::Space, 0, 0);
    router.Dispatch(press);

    // The gameplay snapshot is the one a SceneSystem reads via SystemContext.Input.
    CHECK(input.IsKeyDown(Key::Space));
}

TEST_CASE("InputRouter: Shift+Esc releases gameplay focus and is not delivered to the game")
{
    Input input(nullptr);
    InputRouter router(nullptr, input, nullptr);

    router.PushFocus(InputFocus::Gameplay);

    input.BeginFrame();
    // Shift is applied first (the game sees the modifier), then Escape triggers the chord.
    KeyPressedEvent shift(Key::LeftShift, 0, 0);
    router.Dispatch(shift);
    CHECK(input.IsKeyDown(Key::LeftShift));

    KeyPressedEvent escape(Key::Escape, 0, 0);
    router.Dispatch(escape);

    // The chord popped focus and swallowed the Escape, so the game never sees it.
    CHECK(router.GetFocus() == InputFocus::UI);
    CHECK_FALSE(input.IsKeyDown(Key::Escape));
}

TEST_CASE("InputRouter: a bare Escape without Shift is delivered, not a release")
{
    Input input(nullptr);
    InputRouter router(nullptr, input, nullptr);

    router.PushFocus(InputFocus::Gameplay);

    input.BeginFrame();
    KeyPressedEvent escape(Key::Escape, 0, 0);
    router.Dispatch(escape);

    // No Shift held, so Escape is ordinary gameplay input and focus is unchanged.
    CHECK(router.IsGameplayFocused());
    CHECK(input.IsKeyDown(Key::Escape));
}

TEST_CASE("InputRouter: window-focus loss pops a held gameplay focus")
{
    Input input(nullptr);
    InputRouter router(nullptr, input, nullptr);

    router.PushFocus(InputFocus::Gameplay);

    WindowFocusEvent lost(false);
    router.Dispatch(lost);
    CHECK(router.GetFocus() == InputFocus::UI);

    // Regaining focus does not re-capture on its own.
    router.PushFocus(InputFocus::Gameplay);
    WindowFocusEvent gained(true);
    router.Dispatch(gained);
    CHECK(router.IsGameplayFocused());
}
