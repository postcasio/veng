// Editing-key routing into a focused Gui text field (GPU band): a real keyboard's Backspace,
// Delete, arrows and Home/End arrive as KeyPressed events carrying a key code, not as typed
// characters, so the only route that proves them is the production one — InputRouter::Dispatch of a
// KeyPressedEvent shaped exactly as Window.cpp's GLFW key callback shapes it, offered to the
// router's consumer registry, where the real Gui::GuiConsumer maps the key and drives the document
// attached to a real viewport. Nothing here stands in for a production part.
//
// The band is GPU only because GuiConsumer routes through Renderer::Viewport and Viewport::Create
// needs a live Context; the behaviour under test is device-free.
//
// The cases pin:
//   (a) Backspace deletes the codepoint before the caret, including a multi-byte one;
//   (b) Left/Right move the caret one codepoint and leave focus alone;
//   (c) caret movement clamps at both ends and still consumes the key, so a clamped arrow never
//       leaks out into focus navigation;
//   (d) Delete forward-deletes and Home/End jump the caret;
//   (e) the precedence rule both ways — an arrow moves the caret while a field is focused, and
//       still moves focus when a non-text element holds it;
//   (f) holding a key repeats the edit — the platform's auto-repeat arrives as KeyRepeatEvent and
//       drives the same deletion/caret step, so a held Backspace empties the field and a held arrow
//       walks the caret;
//   (g) a repeat drives editing only — it never walks focus, which is a discrete step.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegistry.h>

#include <gpu/fixture.h>

#include <vector>

using namespace Veng;

namespace
{
    // The production routing stack for a key event, assembled once per case: a headless snapshot, a
    // router over the context's viewport registry, one offscreen viewport, and the real GuiConsumer
    // registered in the router's consumer registry. A Dispatch of a KeyPressedEvent then travels
    // exactly the path a window-sourced key press travels.
    struct KeyRoute
    {
        KeyRoute(Renderer::Context& context, AssetManager& assets)
            : Router(nullptr, Snapshot, context.GetViewportRegistry()),
              View(Renderer::Viewport::Create({
                  .Context = context,
                  .Assets = assets,
                  .Region = {.Offset = {0, 0}, .Extent = {128, 128}},
                  .Settings = {},
                  .Role = Renderer::ViewportRole::Offscreen,
              })),
              Consumer(Router, Snapshot, nullptr, Viewports)
        {
            Viewports.push_back(View.get());
            Router.RegisterConsumer(Consumer);
        }

        // Presses one key the way the window's GLFW key callback does: a KeyPressedEvent with the
        // key code, through the router.
        void Press(Key key)
        {
            KeyPressedEvent event(key, 0, 0);
            Router.Dispatch(event);
        }

        // One tick of the platform's auto-repeat on a key already held, the way the window's GLFW
        // key callback shapes a GLFW_REPEAT: a KeyRepeatEvent through the same router.
        void Repeat(Key key)
        {
            KeyRepeatEvent event(key, 0, 0);
            Router.Dispatch(event);
        }

        Input Snapshot{nullptr};
        InputRouter Router;
        std::vector<Renderer::Viewport*> Viewports;
        Unique<Renderer::Viewport> View;
        Gui::GuiConsumer Consumer;
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

    KeyRoute route(Context, assets);
    Gui::Document document;
    // "Hié" — the last codepoint is two UTF-8 bytes, so a byte-wise delete would leave a broken
    // trailing byte instead of removing the glyph.
    Gui::Element& field = AttachFocusedField(route, document, "Hi\xc3\xa9");
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

    KeyRoute route(Context, assets);
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

    KeyRoute route(Context, assets);
    Gui::Document document;
    Gui::Element& field = AttachFocusedField(route, document, "Hi\xc3\xa9");

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

    KeyRoute route(Context, assets);
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

    KeyRoute route(Context, assets);
    Gui::Document document;
    Gui::Element& field = AttachFocusedField(route, document, "Hello\xc3\xa9");
    CHECK(field.Widget.Caret == 6);

    // The physical press deletes the first codepoint, exactly as before.
    route.Press(Key::Backspace);
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
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "gui editing keys: a held arrow walks the caret")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets);
    Gui::Document document;
    Gui::Element& field = AttachFocusedField(route, document, "Hi\xc3\xa9!");
    CHECK(field.Widget.Caret == 4);

    // Hold Left: the press steps once, then every repeat steps one more codepoint — the multi-byte
    // glyph included — until the caret clamps at the start.
    route.Press(Key::Left);
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

    // The same holds the other way.
    route.Press(Key::Right);
    route.Repeat(Key::Right);
    route.Repeat(Key::Right);
    CHECK(field.Widget.Caret == 3);
    CHECK(document.GetFocused() == &field);

    // Delete repeats too: held forward-delete eats the rest of the value.
    route.Press(Key::Home);
    route.Press(Key::Delete);
    route.Repeat(Key::Delete);
    route.Repeat(Key::Delete);
    CHECK(field.Text == "!");
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "gui editing keys: a repeat never walks focus")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    KeyRoute route(Context, assets);
    Gui::Document document;
    InstallMeasurer(document);
    document.SetInteractive(true);

    // Three buttons in a row, the leftmost focused. Focus navigation is a discrete step, so holding
    // an arrow must not skate through them the way a held arrow walks a caret.
    Gui::Element& left = document.Add(document.Root(), Gui::ElementKind::Button);
    Gui::Element& middle = document.Add(document.Root(), Gui::ElementKind::Button);
    Gui::Element& right = document.Add(document.Root(), Gui::ElementKind::Button);
    document.InitWidget(left);
    document.InitWidget(middle);
    document.InitWidget(right);
    left.Layout = Gui::Rect{.Min = {0.0f, 0.0f}, .Size = {40.0f, 20.0f}};
    middle.Layout = Gui::Rect{.Min = {60.0f, 0.0f}, .Size = {40.0f, 20.0f}};
    right.Layout = Gui::Rect{.Min = {120.0f, 0.0f}, .Size = {40.0f, 20.0f}};
    document.SetFocus(&left);
    route.View->AttachDocument(document);

    // The press moves focus one step, as it always has.
    route.Press(Key::Right);
    CHECK(document.GetFocused() == &middle);

    // Holding it does not carry focus onward.
    route.Repeat(Key::Right);
    route.Repeat(Key::Right);
    route.Repeat(Key::Right);
    CHECK(document.GetFocused() == &middle);

    // Nor do the other navigation keys repeat: Tab, Enter and Escape are discrete choices.
    route.Repeat(Key::Tab);
    route.Repeat(Key::Tab);
    CHECK(document.GetFocused() == &middle);

    // A fresh press still steps, so nothing about the discrete route was lost.
    route.Press(Key::Right);
    CHECK(document.GetFocused() == &right);
}
