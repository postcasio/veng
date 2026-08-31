// World-space Gui input — the ray → UV → document-coordinate adapter, the seat gate, and the
// router-consumer routing, all headless. Device-free: the ray-vs-panel intersection is pure glm,
// documents are built imperatively with elements placed directly (bypassing the flex solve and any
// font resource), and the SurfaceInputConsumer routes synthetic events through the delivered
// capture/bubble pipeline. No ICD, no GPU target — input is orthogonal to how the panel is shaded.

#include <doctest/doctest.h>

#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Gui/SurfaceInput.h>
#include <Veng/Input.h>
#include <Veng/Input/SeatFocusScope.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/Entity.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    // Places an element to a fixed document-space rect directly, bypassing the flex solve — the
    // hit geometry is pinned, so the adapter cases are independent of the layout solver.
    void PlaceAt(Element& element, vec2 min, vec2 size)
    {
        element.Layout = Rect{.Min = min, .Size = size};
    }

    // The panel used across the cases: the identity-placed 2×2 rectangle in the XY plane facing +Z,
    // centered at the world origin. Its half-extents are ±1 world unit, so a document point maps to a
    // simple world coordinate the rays below are aimed at.
    SurfacePlacement UnitPanel()
    {
        return SurfacePlacement{.Transform = mat4(1.0f), .Size = vec2(2.0f, 2.0f)};
    }

    // A ray from +Z looking toward -Z that hits the panel's world point (0.5, -0.5, 0). On a 200×200
    // document that resolves to (150, 150): u = (0.5 + 1)/2 = 0.75, v = (1 - (-0.5))/2 = 0.75.
    Ray AimedRay()
    {
        return Ray{.Origin = vec3(0.5f, -0.5f, 5.0f), .Direction = vec3(0.0f, 0.0f, -1.0f)};
    }

    // Builds a surface owning a document with a single clickable button covering (150, 150), bound to
    // a handler that increments *clicks*. The document is injected imperatively (no GPU target).
    struct TestPanel
    {
        Unique<Document> DocOwner; // moved into the surface; Doc/Button stay valid (stable storage)
        Document* Doc = nullptr;
        Element* Button = nullptr;
        BindingContext Context;
        TypeRegistry Registry;
        int Clicks = 0;
        GuiSurface Surface;

        TestPanel()
        {
            DocOwner = CreateUnique<Document>();
            Doc = DocOwner.get();
            Element& root = Doc->Root();
            PlaceAt(root, {0, 0}, {200, 200});
            Button = &Doc->Add(root, ElementKind::Button);
            Button->Focusable = true;
            PlaceAt(*Button, {100, 100}, {100, 100});
            Button->Bindings["onClick"] = "start";

            Doc->BindContext(&Context, &Registry);
            Context.SetHandler("start", [this](Element&) { ++Clicks; });

            Surface.Resolution = {200, 200};
            Surface.SetDocument(std::move(DocOwner));
        }
    };
}

TEST_CASE("gui surface adapter: a ray resolves to the document point over the element it hits")
{
    TestPanel panel;
    const SurfacePlacement placement = UnitPanel();
    const Ray ray = AimedRay();

    const optional<SurfaceRayHit> hit = IntersectSurface(ray, placement, panel.Surface);
    REQUIRE(hit);
    CHECK(hit->DocumentPoint.x == doctest::Approx(150.0f));
    CHECK(hit->DocumentPoint.y == doctest::Approx(150.0f));
    CHECK(hit->Distance == doctest::Approx(5.0f));

    // The resolved point lands on the button — the document-local hit-test agrees with the ray.
    CHECK(panel.Doc->HitTest(hit->DocumentPoint) == panel.Button);

    // A ray that misses the panel rectangle yields no document point.
    const Ray outside{.Origin = vec3(5.0f, 5.0f, 5.0f), .Direction = vec3(0.0f, 0.0f, -1.0f)};
    CHECK_FALSE(IntersectSurface(outside, placement, panel.Surface));

    // A ray parallel to the panel (in its plane) also misses.
    const Ray parallel{.Origin = vec3(0.0f, 0.0f, 5.0f), .Direction = vec3(0.0f, 1.0f, 0.0f)};
    CHECK_FALSE(IntersectSurface(parallel, placement, panel.Surface));
}

TEST_CASE("gui surface adapter: a seated ray delivers a down/up click through plan-07's pipeline")
{
    TestPanel panel;
    panel.Doc->SetInteractive(true);
    panel.Surface.Seat = Entity{.Index = 1, .Generation = 1};

    const SurfacePlacement placement = UnitPanel();
    const Ray ray = AimedRay();

    // A press then a release on the same element synthesizes a Click through the delivered
    // capture→bubble routing — the world ray reaching the same handler a screen pointer would.
    const optional<vec2> down =
        RouteSurfacePointer(panel.Surface, placement, ray, PointerEventKind::Down);
    const optional<vec2> up =
        RouteSurfacePointer(panel.Surface, placement, ray, PointerEventKind::Up);
    REQUIRE(down);
    REQUIRE(up);
    CHECK(panel.Clicks == 1);
}

TEST_CASE("gui surface adapter: the mapping is identical across both material domains")
{
    // The ray→coordinate mapping depends only on the panel geometry and the document extent, never on
    // how the panel is shaded — a translucent and an opaque-emissive panel resolve the same point.
    TestPanel translucent;
    translucent.Surface.Domain = GuiSurfaceDomain::Translucent;
    TestPanel opaque;
    opaque.Surface.Domain = GuiSurfaceDomain::OpaqueEmissive;

    const SurfacePlacement placement = UnitPanel();
    const Ray ray = AimedRay();

    const optional<SurfaceRayHit> a = IntersectSurface(ray, placement, translucent.Surface);
    const optional<SurfaceRayHit> b = IntersectSurface(ray, placement, opaque.Surface);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->DocumentPoint.x == doctest::Approx(b->DocumentPoint.x));
    CHECK(a->DocumentPoint.y == doctest::Approx(b->DocumentPoint.y));
}

TEST_CASE("gui surface seat gate: a panel is display-only until a seat + SeatFocusScope open it")
{
    TestPanel panel;
    panel.Doc->SetInteractive(true);

    Input input(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, input, registry);
    SurfaceInputConsumer consumer(router);
    const SurfacePlacement placement = UnitPanel();
    const Ray ray = AimedRay();
    auto reg = consumer.Register(
        panel.Surface, [&] { return placement; }, [&]() -> optional<Ray> { return ray; });

    const auto press = [&]
    {
        consumer.ForwardEvent(MouseButtonPressedEvent(MouseButton::Left, 0));
        consumer.ForwardEvent(MouseButtonReleasedEvent(MouseButton::Left, 0));
    };

    // No seat: the surface is display-only and the adapter is never consulted, so a click routes
    // nothing even though the document is interactive.
    panel.Surface.Seat = Entity::Null;
    press();
    CHECK(panel.Clicks == 0);

    // Assign a seat the game holds through gameplay focus: the pointer is not the UI's yet.
    const Entity seat{.Index = 3, .Generation = 1};
    panel.Surface.Seat = seat;
    const FocusToken gameplay = router.PushFocus(seat, InputFocus::Gameplay);
    press();
    CHECK(panel.Clicks == 0);

    // Opening a SeatFocusScope flips the seat's focus top to UI — the panel becomes interactive and
    // the same ray now delivers its click.
    {
        const SeatFocusScope scope(router, InputSeat{.Viewer = seat, .World = nullptr}, nullptr);
        press();
        CHECK(panel.Clicks == 1);
    }

    // Closing the scope restores gameplay focus: the panel is display-only again and ignores the ray.
    press();
    CHECK(panel.Clicks == 1);
    router.PopFocus(gameplay);
}

TEST_CASE(
    "gui surface adapter: typed text and editing keys reach the focused field through the consumer")
{
    // A text field on a world panel is edited over the router consumer, not the document directly:
    // a KeyTyped inserts a codepoint and a Backspace deletes one, both routed by ForwardEvent under
    // the same seat gate the pointer takes. A caret-less editing key carries no character, so it
    // reaches the field only through the KeyPressed → DispatchTextEdit route this asserts — the half
    // of the world-space adapter a pointer-only forward never covered.
    Unique<Document> docOwner = CreateUnique<Document>();
    Document* const doc = docOwner.get();
    doc->SetInteractive(true);
    Element& root = doc->Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& input = doc->Add(root, ElementKind::TextInput);
    PlaceAt(input, {0, 0}, {200, 40});
    doc->InitWidget(input);
    doc->SetFocus(&input);

    // Moving the Unique into the surface transfers ownership without moving the Document object, so
    // `input` stays a live reference the assertions read back.
    GuiSurface surface;
    surface.Resolution = {200, 200};
    const Entity seat{.Index = 4, .Generation = 1};
    surface.Seat = seat;
    surface.SetDocument(std::move(docOwner));

    Input inputDevice(nullptr);
    const Renderer::ViewportRegistry registry;
    InputRouter router(nullptr, inputDevice, registry);
    SurfaceInputConsumer consumer(router);
    const SurfacePlacement placement = UnitPanel();
    const Ray ray = AimedRay();
    auto reg = consumer.Register(
        surface, [&] { return placement; }, [&]() -> optional<Ray> { return ray; });

    // A gameplay-focused seat gates the keystroke out exactly as it gates a pointer.
    const FocusToken gameplay = router.PushFocus(seat, InputFocus::Gameplay);
    CHECK_FALSE(consumer.ForwardEvent(KeyTypedEvent('X')));
    CHECK(input.Text.empty());

    // Opening a SeatFocusScope flips the seat's focus top to UI, so the field now receives input.
    const SeatFocusScope scope(router, InputSeat{.Viewer = seat, .World = nullptr}, nullptr);

    // Interactive document + UI-focused seat: a typed character routes into the focused field.
    CHECK(consumer.ForwardEvent(KeyTypedEvent('H')));
    CHECK(consumer.ForwardEvent(KeyTypedEvent('i')));
    CHECK(input.Text == "Hi");

    // Backspace carries no character: it reaches the field only as a key press mapped to an edit.
    CHECK(consumer.ForwardEvent(KeyPressedEvent(Key::Backspace, 0, 0)));
    CHECK(input.Text == "H");
}

TEST_CASE("gui surface focus nav: gamepad directional navigation needs the seat, not the ray")
{
    // A gamepad-driven in-world menu needs only the seat + scope, not the coordinate adapter — the
    // focus model is 2D-within-the-tree and never touched the ray.
    Document doc;
    doc.SetInteractive(true);
    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {300, 300});

    Element& top = doc.Add(root, ElementKind::Button);
    top.Focusable = true;
    PlaceAt(top, {100, 0}, {100, 40});
    Element& middle = doc.Add(root, ElementKind::Button);
    middle.Focusable = true;
    PlaceAt(middle, {100, 100}, {100, 40});
    Element& bottom = doc.Add(root, ElementKind::Button);
    bottom.Focusable = true;
    PlaceAt(bottom, {100, 200}, {100, 40});

    // Directional navigation moves focus down the column with no ray involved at all.
    doc.SetFocus(&top);
    CHECK(doc.Navigate(NavAction::MoveDown));
    CHECK(doc.GetFocused() == &middle);
    doc.Navigate(NavAction::MoveDown);
    CHECK(doc.GetFocused() == &bottom);
    doc.Navigate(NavAction::MoveUp);
    CHECK(doc.GetFocused() == &middle);
}
