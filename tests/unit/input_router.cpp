// InputRouter focus-stack and routing unit cases. The router's logic is device-free: it
// folds events into the Input snapshot by focus and manages a focus stack with the Shift+Esc
// release chord. A null window (no cursor capture) and a null ImGui layer (no UI sink) leave
// exactly the snapshot-routing + focus behavior under test, with no GPU.

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Input.h>
#include <Veng/Input/InputConsumer.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/WindowEvents.h>

using namespace Veng;

namespace
{
    // Stands in for the viewport-owning Gui consumer: it turns a routed KeyTyped into the same
    // Document::DispatchText call that consumer makes, so a text event reaching the consumer list
    // reaches the focused field's edit. A consumer registry entry is the router's only text sink,
    // injected and window-sourced alike.
    class TextConsumer final : public InputConsumer
    {
    public:
        explicit TextConsumer(Gui::Document& document) : m_Document(document) {}

        bool ForwardEvent(const Event& event) override
        {
            if (event.GetEventType() != EventType::KeyTyped)
            {
                return false;
            }
            return m_Document.DispatchText(static_cast<const KeyTypedEvent&>(event).GetCodepoint());
        }

    private:
        Gui::Document& m_Document;
    };

    // A document holding one focused, interactive text field — the text sink under test.
    Gui::Element& FocusedField(Gui::Document& document)
    {
        document.SetInteractive(true);
        Gui::Element& field = document.Add(document.Root(), Gui::ElementKind::TextInput);
        document.InitWidget(field);
        document.SetFocus(&field);
        return field;
    }
}

TEST_CASE("InputRouter: defaults to UI focus")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    const InputRouter router(nullptr, input, registry);

    CHECK(router.GetFocus() == InputFocus::UI);
    CHECK_FALSE(router.IsGameplayFocused());
}

TEST_CASE("InputRouter: under UI focus an input event folds into the snapshot")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

    input.BeginFrame();
    KeyPressedEvent press(Key::W, 0, 0);
    router.Dispatch(press);

    CHECK(input.IsKeyDown(Key::W));
    CHECK(input.WasKeyPressed(Key::W));
}

TEST_CASE("InputRouter: pushing and popping gameplay focus moves the stack top")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

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
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

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
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

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
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

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
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

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

TEST_CASE("InputRouter: an injected tap is paced so the press is seen before the release")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

    // Queue one tap. A queued event is not applied until DrainInjectedEvents releases it.
    const KeyPressedEvent down(Key::W, 0, 0);
    const KeyReleasedEvent up(Key::W, 0, 0);
    router.PostInjectedEvent(down);
    router.PostInjectedEvent(up);

    input.BeginFrame(true);
    CHECK_FALSE(input.IsKeyDown(Key::W));

    // First drain applies the press but defers the release (it would reverse W's level this frame),
    // so a tick this frame sees the key down.
    router.DrainInjectedEvents();
    CHECK(input.IsKeyDown(Key::W));

    // Next frame's drain applies the deferred release.
    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK_FALSE(input.IsKeyDown(Key::W));
}

TEST_CASE("InputRouter: two injected taps of one key stay distinct instead of collapsing")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

    // Two back-to-back taps of the same key in one batch.
    for (int i = 0; i < 2; ++i)
    {
        const KeyPressedEvent down(Key::Space, 0, 0);
        const KeyReleasedEvent up(Key::Space, 0, 0);
        router.PostInjectedEvent(down);
        router.PostInjectedEvent(up);
    }

    // Each level change lands on its own frame: down, up, down, up — so a per-tick action reads two
    // separate press→release taps rather than one held span.
    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK(input.IsKeyDown(Key::Space));

    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK_FALSE(input.IsKeyDown(Key::Space));

    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK(input.IsKeyDown(Key::Space));

    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK_FALSE(input.IsKeyDown(Key::Space));
}

TEST_CASE("InputRouter: an injected scroll is queued and applies on the next drain")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

    const MouseScrolledEvent scroll(vec2(0.0f, 3.0f));
    router.PostInjectedEvent(scroll);

    // Queued, not yet applied.
    input.BeginFrame(true);
    CHECK(input.GetScrollDelta().y == doctest::Approx(0.0f));

    // The drain folds it into the snapshot at the pre-tick point, so a tick this frame reads it.
    router.DrainInjectedEvents();
    CHECK(input.GetScrollDelta().y == doctest::Approx(3.0f));
}

TEST_CASE("InputRouter: an injected text event types into the focused field")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

    Gui::Document document;
    Gui::Element& field = FocusedField(document);
    TextConsumer consumer(document);
    router.RegisterConsumer(consumer);

    const KeyTypedEvent typed('H');
    router.PostInjectedEvent(typed);

    // Queued like every other injected kind: nothing is typed until the drain.
    CHECK(field.Text.empty());

    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK(field.Text == "H");
}

TEST_CASE("InputRouter: injected text takes the same route as window-sourced text")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

    Gui::Document injected;
    Gui::Element& injectedField = FocusedField(injected);
    TextConsumer injectedConsumer(injected);

    Gui::Document windowed;
    Gui::Element& windowedField = FocusedField(windowed);
    TextConsumer windowedConsumer(windowed);

    // Two routers so each document is the sole consumer of its own run.
    InputRouter windowRouter(nullptr, input, registry);
    router.RegisterConsumer(injectedConsumer);
    windowRouter.RegisterConsumer(windowedConsumer);

    // A multi-byte codepoint, so the route carries a codepoint rather than a byte.
    for (const u32 codepoint : {static_cast<u32>('H'), 0x00E9U, static_cast<u32>('!')})
    {
        const KeyTypedEvent typed(codepoint);
        router.PostInjectedEvent(typed);

        KeyTypedEvent window(codepoint);
        windowRouter.Dispatch(window);
    }

    input.BeginFrame(true);
    router.DrainInjectedEvents();

    // U+00E9 arrives as one codepoint and lands as its two UTF-8 bytes, not as two edits.
    CHECK(injectedField.Text == "H\xc3\xa9!");
    CHECK(injectedField.Text == windowedField.Text);
}
