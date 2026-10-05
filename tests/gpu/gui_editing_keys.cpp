// Editing-key routing into a focused Gui text field, and its precedence over role navigation (GPU
// band): a real keyboard's Backspace, Delete, arrows and Home/End arrive as KeyPressed events
// carrying a key code, not as typed characters, so the only route that proves them is the production
// one — InputRouter::Dispatch of a KeyPressedEvent shaped exactly as Window.cpp's GLFW key callback
// shapes it, offered to the router's consumer registry, where the real Gui::GuiConsumer maps the key
// and drives the document attached to a real viewport. Navigation arrives the production way too: a
// UI input map's role actions, resolved each frame by the engine's role resolver against the same
// snapshot and dispatched back through the router. Nothing here stands in for a production part.
//
// The band is GPU only because GuiConsumer routes through Renderer::Viewport and Viewport::Create
// needs a live Context; the behaviour under test is device-free.
//
// The cases pin:
//   (a) Backspace deletes the codepoint before the caret, including a multi-byte one;
//   (b) Left/Right move the caret one codepoint and leave focus alone;
//   (c) caret movement clamps at both ends and still claims the key, so a clamped arrow never
//       leaks out into focus navigation;
//   (d) Delete forward-deletes and Home/End jump the caret;
//   (e) the precedence rule both ways — an arrow moves the caret while a field is focused, and
//       still moves focus when a non-text element holds it;
//   (f) holding a key repeats the edit — the platform's auto-repeat arrives as KeyRepeatEvent and
//       drives the same deletion/caret step, so a held Backspace empties the field and a held arrow
//       walks the caret;
//   (g) a held arrow on a focused field never moves focus, past the role's repeat delay as well;
//   (h) a platform repeat never walks focus; a held direction walks it only through the role's own
//       repeat, at its delay and rate.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/WorldRunner.h>

#include <Input/RoleResolver.h>
#include <gpu/fixture.h>

#include <vector>

using namespace Veng;

namespace
{
    // The UI map's directions repeat after half a second, then every quarter: binary-exact, so a run
    // of eighth-second frames lands on each deadline exactly.
    constexpr f32 RepeatDelay = 0.5f;
    constexpr f32 RepeatRate = 0.25f;
    constexpr f32 FrameDelta = 0.125f;

    // A UI input map carrying the keys Gui navigation has always answered to, as role actions.
    Ref<InputMappingContext> MakeUiMap()
    {
        const auto action = [](const u64 id, const ActionRole role, const bool repeats)
        {
            return InputAction{.Id = ActionId{id},
                               .Name = "ui",
                               .Kind = ActionKind::Button,
                               .Role = role,
                               .RepeatDelay = repeats ? RepeatDelay : 0.0f,
                               .RepeatRate = repeats ? RepeatRate : 0.0f};
        };
        const auto key = [](const u64 id, const Key code)
        {
            return Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = u32(code)},
                           .Action = ActionId{id}};
        };
        return InputMappingContext::Create(
            {action(1, ActionRole::NavigateUp, true), action(2, ActionRole::NavigateDown, true),
             action(3, ActionRole::NavigateLeft, true), action(4, ActionRole::NavigateRight, true),
             action(5, ActionRole::NavigateNext, false), action(6, ActionRole::Confirm, false),
             action(7, ActionRole::Cancel, false)},
            {key(1, Key::Up), key(2, Key::Down), key(3, Key::Left), key(4, Key::Right),
             key(5, Key::Tab), key(6, Key::Enter), key(6, Key::Space), key(7, Key::Escape)});
    }

    // The production routing stack for a key, assembled once per case: a headless snapshot, a router
    // over the context's viewport registry, one offscreen viewport, the real GuiConsumer registered in
    // the router's consumer registry, and the engine's role resolver reading a UI map. A Dispatch of a
    // KeyPressedEvent then travels exactly the path a window-sourced key press travels, and a Frame
    // resolves the roles as the application's frame does once its input has landed.
    struct KeyRoute
    {
        KeyRoute(Renderer::Context& context, AssetManager& assets, TypeRegistry& types)
            : Router(nullptr, Snapshot, context.GetViewportRegistry()),
              View(Renderer::Viewport::Create({
                  .Context = context,
                  .Assets = assets,
                  .Region = {.Offset = {0, 0}, .Extent = {128, 128}},
                  .Settings = {},
                  .Role = Renderer::ViewportRole::Offscreen,
              })),
              Consumer(Router, Snapshot, nullptr, Viewports),
              Runner(WorldRunnerInfo{.Types = &types, .Systems = &Systems})
        {
            Viewports.push_back(View.get());
            Router.RegisterConsumer(Consumer);
        }

        // One application frame's role resolution, after the frame's key events have landed.
        void Frame(const f32 delta = FrameDelta)
        {
            const PointerRouting pointer{};
            Resolver.Update(RoleFrameInfo{.Snapshot = Snapshot,
                                          .Router = Router,
                                          .Worlds = Runner,
                                          .DefaultUi = UiMap.get(),
                                          .Pointer = pointer,
                                          .Delta = delta});
            // The next frame begins: the snapshot rolls, applying any release this frame deferred.
            Snapshot.BeginFrame(true);
        }

        // Puts a key down the way the window's GLFW key callback does, then runs the frame.
        void Hold(Key key)
        {
            KeyPressedEvent event(key, 0, 0);
            Router.Dispatch(event);
            Frame();
        }

        // Lets a held key go, then runs the frame.
        void Release(Key key)
        {
            KeyReleasedEvent event(key, 0, 0);
            Router.Dispatch(event);
            Frame();
        }

        // A tap: down for one frame, then up.
        void Press(Key key)
        {
            Hold(key);
            Release(key);
        }

        // One tick of the platform's auto-repeat on a key already held, the way the window's GLFW
        // key callback shapes a GLFW_REPEAT: a KeyRepeatEvent through the same router, then a frame.
        void Repeat(Key key)
        {
            KeyRepeatEvent event(key, 0, 0);
            Router.Dispatch(event);
            Frame();
        }

        Input Snapshot{nullptr};
        InputRouter Router;
        std::vector<Renderer::Viewport*> Viewports;
        Unique<Renderer::Viewport> View;
        Gui::GuiConsumer Consumer;
        SystemRegistry Systems;
        WorldRunner Runner;
        RoleResolver Resolver;
        Ref<InputMappingContext> UiMap = MakeUiMap();
    };

    // A device-free measurer so a field lays out and paints without a resident font.
    void InstallMeasurer(Gui::Document& document)
    {
        document.SetTextMeasurer([](string_view text, const Gui::Style&, optional<f32>)
                                 { return vec2(static_cast<f32>(text.size()) * 8.0f, 16.0f); });
    }

    // One interactive document holding a single focused text field carrying the given value with
    // the caret at its end, attached to the route's viewport.
    Gui::Element& AttachFocusedField(KeyRoute& route, Gui::Document& document, string_view value)
    {
        InstallMeasurer(document);
        document.SetInteractive(true);
        Gui::Element& field = document.Add(document.Root(), Gui::ElementKind::TextInput);
        document.SetText(field, value);
        document.InitWidget(field);
        document.SetFocus(&field);
        route.View->AttachDocument(document);
        return field;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "gui editing keys: Backspace deletes one codepoint")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    // "Hié" — the last codepoint is two UTF-8 bytes, so a byte-wise delete would leave a broken
    // trailing byte instead of removing the glyph.
    const Gui::Element& field = AttachFocusedField(route, document, "Hi\xc3\xa9");
    CHECK(field.Widget.Caret == 3);

    route.Press(Key::Backspace);
    CHECK(field.Text == "Hi");
    CHECK(field.Widget.Caret == 2);

    route.Press(Key::Backspace);
    CHECK(field.Text == "H");
    CHECK(field.Widget.Caret == 1);

    route.Press(Key::Backspace);
    CHECK(field.Text.empty());
    CHECK(field.Widget.Caret == 0);

    // An empty field's Backspace deletes nothing and moves no caret.
    route.Press(Key::Backspace);
    CHECK(field.Text.empty());
    CHECK(field.Widget.Caret == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui editing keys: arrows move the caret one codepoint without moving focus")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    Gui::Element& field = AttachFocusedField(route, document, "Hi\xc3\xa9");

    // A second focusable sits beside the field: if an arrow drove focus navigation instead of the
    // caret, focus would land here.
    Gui::Element& button = document.Add(document.Root(), Gui::ElementKind::Button);
    document.InitWidget(button);

    route.Press(Key::Left);
    CHECK(field.Widget.Caret == 2);
    CHECK(document.GetFocused() == &field);

    // Left over the multi-byte glyph's neighbour still steps exactly one codepoint.
    route.Press(Key::Left);
    CHECK(field.Widget.Caret == 1);
    route.Press(Key::Left);
    CHECK(field.Widget.Caret == 0);

    // Clamped at the start, and the key is still claimed — focus does not move.
    route.Press(Key::Left);
    CHECK(field.Widget.Caret == 0);
    CHECK(document.GetFocused() == &field);

    route.Press(Key::Right);
    CHECK(field.Widget.Caret == 1);

    // Right over the multi-byte glyph lands past the whole glyph, not inside it: a Backspace here
    // removes it entirely.
    route.Press(Key::Right);
    route.Press(Key::Right);
    CHECK(field.Widget.Caret == 3);

    // Clamped at the end, still claimed.
    route.Press(Key::Right);
    CHECK(field.Widget.Caret == 3);
    CHECK(document.GetFocused() == &field);

    route.Press(Key::Left);
    route.Press(Key::Backspace);
    CHECK(field.Text == "H\xc3\xa9");
    CHECK(document.GetFocused() == &field);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui editing keys: Delete forward-deletes and Home/End jump the caret")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    const Gui::Element& field = AttachFocusedField(route, document, "Hi\xc3\xa9");

    route.Press(Key::Home);
    CHECK(field.Widget.Caret == 0);

    // At the start, forward Delete removes the first codepoint and leaves the caret put.
    route.Press(Key::Delete);
    CHECK(field.Text == "i\xc3\xa9");
    CHECK(field.Widget.Caret == 0);

    route.Press(Key::End);
    CHECK(field.Widget.Caret == 2);

    // At the end there is nothing ahead to delete.
    route.Press(Key::Delete);
    CHECK(field.Text == "i\xc3\xa9");

    // A forward Delete of the multi-byte glyph removes both its bytes as one unit.
    route.Press(Key::Left);
    route.Press(Key::Delete);
    CHECK(field.Text == "i");
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui editing keys: an arrow drives focus only when no text field is focused")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    InstallMeasurer(document);
    document.SetInteractive(true);

    // Two buttons laid out side by side, the left one focused: an arrow here is a focus move, the
    // behaviour that must survive the editing-key precedence.
    Gui::Element& left = document.Add(document.Root(), Gui::ElementKind::Button);
    Gui::Element& right = document.Add(document.Root(), Gui::ElementKind::Button);
    document.InitWidget(left);
    document.InitWidget(right);
    left.Layout = Gui::Rect{.Min = {0.0f, 0.0f}, .Size = {40.0f, 20.0f}};
    right.Layout = Gui::Rect{.Min = {60.0f, 0.0f}, .Size = {40.0f, 20.0f}};
    document.SetFocus(&left);
    route.View->AttachDocument(document);

    route.Press(Key::Right);
    CHECK(document.GetFocused() == &right);

    route.Press(Key::Left);
    CHECK(document.GetFocused() == &left);

    // Focus a text field in the same document and the very same key now edits instead of
    // navigating — the precedence is decided by what holds focus, not by the key.
    Gui::Element& field = document.Add(document.Root(), Gui::ElementKind::TextInput);
    document.SetText(field, "ab");
    document.InitWidget(field);
    field.Layout = Gui::Rect{.Min = {120.0f, 0.0f}, .Size = {40.0f, 20.0f}};
    document.SetFocus(&field);

    route.Press(Key::Left);
    CHECK(document.GetFocused() == &field);
    CHECK(field.Widget.Caret == 1);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "gui editing keys: a held Backspace repeats the deletion")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    const Gui::Element& field = AttachFocusedField(route, document, "Hello\xc3\xa9");
    CHECK(field.Widget.Caret == 6);

    // The physical press deletes the first codepoint, exactly as before.
    route.Hold(Key::Backspace);
    CHECK(field.Text == "Hello");
    CHECK(field.Widget.Caret == 5);

    // The key stays down, so the platform repeats it. Each repeat must delete another codepoint —
    // the whole point of holding Backspace — rather than requiring a fresh press per character.
    route.Repeat(Key::Backspace);
    CHECK(field.Text == "Hell");
    CHECK(field.Widget.Caret == 4);

    route.Repeat(Key::Backspace);
    route.Repeat(Key::Backspace);
    route.Repeat(Key::Backspace);
    CHECK(field.Text == "H");
    CHECK(field.Widget.Caret == 1);

    route.Repeat(Key::Backspace);
    CHECK(field.Text.empty());
    CHECK(field.Widget.Caret == 0);

    // An empty field's repeat deletes nothing, and holding past the start is not an error.
    route.Repeat(Key::Backspace);
    CHECK(field.Text.empty());
    CHECK(field.Widget.Caret == 0);
    route.Release(Key::Backspace);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "gui editing keys: a held arrow walks the caret")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    Gui::Element& field = AttachFocusedField(route, document, "Hi\xc3\xa9!");
    CHECK(field.Widget.Caret == 4);

    // Hold Left: the press steps once, then every repeat steps one more codepoint — the multi-byte
    // glyph included — until the caret clamps at the start.
    route.Hold(Key::Left);
    CHECK(field.Widget.Caret == 3);
    route.Repeat(Key::Left);
    CHECK(field.Widget.Caret == 2);
    route.Repeat(Key::Left);
    CHECK(field.Widget.Caret == 1);
    route.Repeat(Key::Left);
    CHECK(field.Widget.Caret == 0);

    // Clamped, still claimed by the field: a held arrow that runs off the end does not leak into
    // focus navigation.
    route.Repeat(Key::Left);
    CHECK(field.Widget.Caret == 0);
    CHECK(document.GetFocused() == &field);
    route.Release(Key::Left);

    // The same holds the other way.
    route.Hold(Key::Right);
    route.Repeat(Key::Right);
    route.Repeat(Key::Right);
    CHECK(field.Widget.Caret == 3);
    CHECK(document.GetFocused() == &field);
    route.Release(Key::Right);

    // Delete repeats too: held forward-delete eats the rest of the value.
    route.Press(Key::Home);
    route.Hold(Key::Delete);
    route.Repeat(Key::Delete);
    route.Repeat(Key::Delete);
    route.Release(Key::Delete);
    CHECK(field.Text == "!");
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui editing keys: a held arrow on a focused field never moves focus")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    Gui::Element& field = AttachFocusedField(route, document, "abcdefgh");

    // A button left of the field: a Left that reached navigation would land focus on it.
    Gui::Element& button = document.Add(document.Root(), Gui::ElementKind::Button);
    document.InitWidget(button);
    button.Layout = Gui::Rect{.Min = {0.0f, 0.0f}, .Size = {40.0f, 20.0f}};
    field.Layout = Gui::Rect{.Min = {60.0f, 0.0f}, .Size = {80.0f, 20.0f}};

    // Held for four times the role's repeat delay, with the platform repeating all the while: the
    // field claimed the key on its press, so the role resolution reads it as up throughout and the
    // repeat timer never starts.
    route.Hold(Key::Left);
    for (int frame = 0; frame < 16; ++frame)
    {
        route.Repeat(Key::Left);
    }
    CHECK(document.GetFocused() == &field);
    CHECK(field.Widget.Caret == 0);
    route.Release(Key::Left);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui editing keys: a held direction walks focus by the role's repeat alone")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets, Types);
    Gui::Document document;
    InstallMeasurer(document);
    document.SetInteractive(true);

    // Four buttons in a row, the leftmost focused.
    std::vector<Gui::Element*> buttons;
    for (int i = 0; i < 4; ++i)
    {
        Gui::Element& button = document.Add(document.Root(), Gui::ElementKind::Button);
        document.InitWidget(button);
        button.Layout =
            Gui::Rect{.Min = {static_cast<f32>(i) * 60.0f, 0.0f}, .Size = {40.0f, 20.0f}};
        buttons.push_back(&button);
    }
    document.SetFocus(buttons[0]);
    route.View->AttachDocument(document);

    // The press moves focus one step.
    route.Hold(Key::Right);
    CHECK(document.GetFocused() == buttons[1]);

    // The platform's own repeats do not navigate: short of the role's delay, focus stays put
    // however many arrive.
    route.Repeat(Key::Right);
    route.Repeat(Key::Right);
    CHECK(document.GetFocused() == buttons[1]);

    // The role's repeat does, once held its delay (half a second at eighth-second frames: the press
    // frame plus four), and then at its rate.
    route.Frame();
    route.Frame();
    CHECK(document.GetFocused() == buttons[2]);
    route.Frame();
    route.Frame();
    CHECK(document.GetFocused() == buttons[3]);

    // A non-repeating role steps once per press, however long it is held.
    route.Release(Key::Right);
    document.SetFocus(buttons[0]);
    route.Hold(Key::Tab);
    for (int frame = 0; frame < 12; ++frame)
    {
        route.Frame();
    }
    route.Release(Key::Tab);
    CHECK(document.GetFocused() == buttons[1]);
}
