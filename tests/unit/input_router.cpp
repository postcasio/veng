// InputRouter focus-stack and routing unit cases. The router's logic is device-free: it
// folds events into the Input snapshot by focus and manages a focus stack with the Shift+Esc
// release chord. A null window (no cursor capture) and a null ImGui layer (no UI sink) leave
// exactly the snapshot-routing + focus behavior under test, with no GPU.

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Input.h>
#include <Veng/Input/InputConsumer.h>
#include <Veng/Input/RawInput.h>
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

TEST_CASE("InputRouter: background input holds a gameplay focus across a window-focus loss")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);

    CHECK_FALSE(router.IsBackgroundInput());
    router.SetBackgroundInput(true);
    router.PushFocus(InputFocus::Gameplay);

    // The token survives the blur, so a driven app's focus-gated contexts keep resolving while the
    // operator works in another window.
    WindowFocusEvent lost(false);
    router.Dispatch(lost);
    CHECK(router.IsGameplayFocused());

    // Regaining focus leaves the held token exactly as it was — nothing was pushed to unwind.
    WindowFocusEvent gained(true);
    router.Dispatch(gained);
    CHECK(router.IsGameplayFocused());

    // Clearing it restores the release, so the setting is the whole of the behaviour.
    router.SetBackgroundInput(false);
    WindowFocusEvent lostAgain(false);
    router.Dispatch(lostAgain);
    CHECK(router.GetFocus() == InputFocus::UI);
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
    const Gui::Element& field = FocusedField(document);
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
    const Gui::Element& injectedField = FocusedField(injected);
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

TEST_CASE("InputRouter: injected moves feed the mouse-delta axis")
{
    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);
    const RawInput raw(input);

    // The snapshot seeds its first position with no delta, so a driven run's opening move reports
    // nothing; the moves after it are the look motion an agent means.
    const MouseMovedEvent seed(vec2(100.0f, 100.0f));
    router.PostInjectedEvent(seed);
    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK(raw.GetAxis(InputDeviceType::MouseAxis, RawInput::MouseAxisX) == doctest::Approx(0.0f));

    const MouseMovedEvent move(vec2(140.0f, 115.0f));
    router.PostInjectedEvent(move);
    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK(raw.GetAxis(InputDeviceType::MouseAxis, RawInput::MouseAxisX) == doctest::Approx(40.0f));
    CHECK(raw.GetAxis(InputDeviceType::MouseAxis, RawInput::MouseAxisY) == doctest::Approx(15.0f));

    // Two moves in one batch apply in one segment, so the delta is the whole travel of the batch.
    router.PostInjectedEvent(MouseMovedEvent(vec2(150.0f, 115.0f)));
    router.PostInjectedEvent(MouseMovedEvent(vec2(160.0f, 115.0f)));
    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK(raw.GetAxis(InputDeviceType::MouseAxis, RawInput::MouseAxisX) == doctest::Approx(20.0f));

    // A frame with no injected move reports no delta — the axis is per-frame motion, not a level.
    input.BeginFrame(true);
    router.DrainInjectedEvents();
    CHECK(raw.GetAxis(InputDeviceType::MouseAxis, RawInput::MouseAxisX) == doctest::Approx(0.0f));
}

TEST_CASE("Input: a Sim tick reads the motion since the previous tick at any frame-to-tick ratio")
{
    // The property a fixed-rate consumer needs, and the one a per-frame delta cannot give it. The
    // frame rate and the tick rate are unrelated, so a tick reading the per-frame delta drops the
    // motion of every frame no tick observed and re-reads one frame's motion once per tick when a
    // frame runs several. What must hold instead is conservation: the deltas a run of ticks observes
    // sum to the motion the device produced, whatever ratio the two rates sat at.
    Input input(nullptr);

    // Seeds the position with no delta, as the snapshot's first move always does.
    input.BeginFrame(true);
    input.ApplyEvent(MouseMovedEvent(vec2(100.0f, 100.0f)));

    const auto moveTo = [&input](const f32 x)
    { input.ApplyEvent(MouseMovedEvent(vec2(x, 100.0f))); };

    SUBCASE("one tick per frame is the baseline")
    {
        f32 observed = 0.0f;
        for (u32 frame = 0; frame < 4; ++frame)
        {
            input.BeginFrame(true);
            moveTo(110.0f + 10.0f * static_cast<f32>(frame));
            input.BeginSimTick();
            observed += input.GetSimMouseDelta().x;
        }
        CHECK(observed == doctest::Approx(40.0f));
    }

    SUBCASE("several frames per tick lose nothing")
    {
        // The high-frame-rate case: four frames of motion, one tick. Every frame's travel has to
        // reach that tick, where a per-frame delta would have kept only the last frame's.
        for (u32 frame = 0; frame < 4; ++frame)
        {
            input.BeginFrame(true);
            moveTo(110.0f + 10.0f * static_cast<f32>(frame));
        }
        // What the per-frame accessor holds at this point is the *last* frame's travel alone, which is
        // exactly the motion a tick reading it would have kept of the four frames.
        CHECK(input.GetMouseDelta().x == doctest::Approx(10.0f));

        input.BeginSimTick();
        CHECK(input.GetSimMouseDelta().x == doctest::Approx(40.0f));
    }

    SUBCASE("several ticks per frame count nothing twice")
    {
        // The low-frame-rate case, and the one that reads as hypersensitivity: one frame of motion,
        // three ticks. The first tick takes it and the rest observe nothing, so the sum is the travel
        // once. A per-frame delta would have handed all three the same 40 and moved the hull 120.
        input.BeginFrame(true);
        moveTo(140.0f);

        f32 observed = 0.0f;
        f32 perFrameObserved = 0.0f;
        for (u32 tick = 0; tick < 3; ++tick)
        {
            input.BeginSimTick();
            observed += input.GetSimMouseDelta().x;
            perFrameObserved += input.GetMouseDelta().x;
        }
        CHECK(observed == doctest::Approx(40.0f));
        // The defect, stated as a number: reading the per-frame delta per tick triples it.
        CHECK(perFrameObserved == doctest::Approx(120.0f));
    }

    SUBCASE("a tick's delta is stable for every reader within the tick")
    {
        // Several Sim systems read within one tick and must agree — the latch is what makes the
        // quantity a tick's rather than the first reader's.
        input.BeginFrame(true);
        moveTo(140.0f);
        input.BeginSimTick();
        CHECK(input.GetSimMouseDelta().x == doctest::Approx(40.0f));
        CHECK(input.GetSimMouseDelta().x == doctest::Approx(40.0f));
        CHECK(input.GetSimMouseDelta().x == doctest::Approx(40.0f));
    }

    SUBCASE("motion made while nothing simulates is dropped rather than banked")
    {
        // A stopped or paused stretch must not arrive as the resuming tick's look.
        input.BeginFrame(true);
        moveTo(400.0f);
        input.DropSimDeltas();
        input.BeginSimTick();
        CHECK(input.GetSimMouseDelta().x == doctest::Approx(0.0f));

        // And the accumulation resumes cleanly afterwards.
        input.BeginFrame(true);
        moveTo(410.0f);
        input.BeginSimTick();
        CHECK(input.GetSimMouseDelta().x == doctest::Approx(10.0f));
    }

    SUBCASE("the per-frame cadence is unchanged for a per-frame consumer")
    {
        // A UI drag or a debug panel is sampled once per frame and must still see this frame's travel
        // and nothing else, whether or not a tick ran.
        input.BeginFrame(true);
        moveTo(130.0f);
        CHECK(input.GetMouseDelta().x == doctest::Approx(30.0f));
        input.BeginFrame(true);
        CHECK(input.GetMouseDelta().x == doctest::Approx(0.0f));
    }
}
