// Gui input, focus, events, and the reflection data-binding layer — headless integration cases.
// Device-free: hit-testing, capture/bubble routing, hover/active/focus state transitions, keyboard
// and gamepad directional focus navigation, a named handler firing on click and on gamepad-confirm,
// and a {obj.field} binding resolving through reflection and updating on a field change. The whole
// input/focus/binding pipeline is pure CPU (no ICD, no font resource); trees and styles are built
// directly, an in-test reflected struct is the binding data object, and the consumer cooperation is
// exercised through plan 06's router registry.

#include <doctest/doctest.h>

#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    // The binding data object: a reflected view-model with a nested struct, so the {obj.field} and
    // {obj.sub.field} path walks are both exercised.
    struct Health
    {
        i32 Current = 0;
        i32 Max = 0;
    };

    struct PlayerModel
    {
        string Name;
        i32 Score = 0;
        Health Vitals;
    };

    // Lays out an element to a fixed document-space rect directly, bypassing the flex solve — the
    // hit-test and focus-navigation cases pin geometry rather than layout, so an explicit rect keeps
    // them independent of the layout solver.
    void PlaceAt(Element& element, vec2 min, vec2 size)
    {
        element.Layout = Rect{.Min = min, .Size = size};
    }
}

// The in-test reflected types carry fake ids — no engine change, exactly as the reflection cases do.
VE_REFLECT(::Health, 0x5107000000000001ULL)
VE_FIELD(Current)
VE_FIELD(Max)
VE_REFLECT_END();

VE_REFLECT(::PlayerModel, 0x5107000000000002ULL)
VE_FIELD(Name)
VE_FIELD(Score)
VE_FIELD(Vitals)
VE_REFLECT_END();

TEST_CASE("gui hit-test: the topmost element under a point wins")
{
    Document doc;
    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {300, 300});

    // Two overlapping children; the later-added one paints over the earlier and so hit-tests first.
    Element& under = doc.Add(root, ElementKind::Panel);
    PlaceAt(under, {10, 10}, {100, 100});
    Element& over = doc.Add(root, ElementKind::Panel);
    PlaceAt(over, {50, 50}, {100, 100});

    // A point in the overlap resolves to the topmost (over).
    CHECK(doc.HitTest(vec2(60, 60)) == &over);
    // A point only in the lower one resolves to it.
    CHECK(doc.HitTest(vec2(20, 20)) == &under);
    // A point covered by neither child hits the root.
    CHECK(doc.HitTest(vec2(280, 280)) == &root);
}

TEST_CASE("gui hit-test: a clipping element hides the clipped-away part of its children")
{
    Document doc;
    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {300, 300});

    // A clipping element with a child that extends past the clip; a point in the clipped-away region
    // does not hit the child (it falls through to the root, which no other element covers).
    Element& clipper = doc.Add(root, ElementKind::Panel);
    clipper.ComputedStyle.OverflowX = Overflow::Hidden;
    clipper.ComputedStyle.OverflowY = Overflow::Hidden;
    PlaceAt(clipper, {0, 0}, {40, 40});
    Element& child = doc.Add(clipper, ElementKind::Panel);
    PlaceAt(child, {0, 0}, {120, 120}); // extends past the 40×40 clip

    CHECK(doc.HitTest(vec2(20, 20)) == &child); // inside the clip → hits the child
    CHECK(doc.HitTest(vec2(20, 80)) == &root);  // child's clipped-away region → misses the child
    CHECK(doc.HitTest(vec2(80, 20)) == &root);  // clipped away on the other axis → misses too
}

TEST_CASE("gui events: bubble order runs target→root, hover/active state track the pointer")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& child = doc.Add(root, ElementKind::Button);
    child.Focusable = true;
    PlaceAt(child, {10, 10}, {100, 100});

    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);

    vector<string> order;
    // Only bubble (onPointer) handlers, so the whole path fires and its order is observable: a
    // handler firing consumes, so a capture handler would stop the child from ever seeing it.
    root.Bindings["onPointer"] = "rootBubble";
    child.Bindings["onPointer"] = "childBubble";
    context.SetHandler("rootBubble", [&](Element&) { order.emplace_back("rootBubble"); });
    context.SetHandler("childBubble", [&](Element&) { order.emplace_back("childBubble"); });

    // A move over the child sets the Hovered bit on it.
    PointerEvent move{.Kind = PointerEventKind::Move, .Position = vec2(50, 50)};
    doc.DispatchPointer(move);
    CHECK((child.State & ElementState::Hovered) == ElementState::Hovered);

    // A press sets Active on the target and focuses the (focusable) child.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(50, 50)};
    doc.DispatchPointer(down);
    CHECK((child.State & ElementState::Active) == ElementState::Active);
    CHECK(doc.GetFocused() == &child);
    // The first handler on the bubble path (target→root) fires and consumes, so only the child's
    // handler runs — the child is nearer the target than the root.
    CHECK(order == vector<string>{"childBubble"});

    // Moving the pointer off the child clears its hover.
    PointerEvent moveAway{.Kind = PointerEventKind::Move, .Position = vec2(150, 150)};
    doc.DispatchPointer(moveAway);
    CHECK((child.State & ElementState::Hovered) == ElementState::None);

    // A release clears Active.
    PointerEvent up{.Kind = PointerEventKind::Up, .Position = vec2(50, 50)};
    doc.DispatchPointer(up);
    CHECK((child.State & ElementState::Active) == ElementState::None);
}

TEST_CASE("gui events: a capture handler consumes before the target's bubble handler")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& child = doc.Add(root, ElementKind::Panel);
    PlaceAt(child, {10, 10}, {100, 100});

    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);

    vector<string> order;
    // The root claims the event during capture (root→target), so the child's bubble handler never
    // runs — a scroll view claiming a drag its children would otherwise take.
    root.Bindings["onPointerCapture"] = "rootCapture";
    child.Bindings["onPointer"] = "childBubble";
    context.SetHandler("rootCapture", [&](Element&) { order.emplace_back("rootCapture"); });
    context.SetHandler("childBubble", [&](Element&) { order.emplace_back("childBubble"); });

    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(50, 50)};
    doc.DispatchPointer(down);
    CHECK(order == vector<string>{"rootCapture"});
}

TEST_CASE("gui events: a click fires the element's onClick handler")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& button = doc.Add(root, ElementKind::Button);
    button.Focusable = true;
    PlaceAt(button, {10, 10}, {100, 100});
    button.Bindings["onClick"] = "start";

    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);

    int clicks = 0;
    context.SetHandler("start", [&](Element&) { ++clicks; });

    // A press then release on the same element is a click.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(50, 50)};
    doc.DispatchPointer(down);
    PointerEvent up{.Kind = PointerEventKind::Up, .Position = vec2(50, 50)};
    doc.DispatchPointer(up);
    CHECK(clicks == 1);

    // A press on the button then a release elsewhere is not a click.
    doc.DispatchPointer(down);
    PointerEvent upElsewhere{.Kind = PointerEventKind::Up, .Position = vec2(180, 180)};
    doc.DispatchPointer(upElsewhere);
    CHECK(clicks == 1);
}

TEST_CASE("gui focus: keyboard Tab and directional gamepad navigation visit focusables in order")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {300, 300});

    // Three focusable buttons laid out in a column, plus a non-focusable panel between them.
    Element& top = doc.Add(root, ElementKind::Button);
    top.Focusable = true;
    PlaceAt(top, {100, 0}, {100, 40});
    Element& decoration = doc.Add(root, ElementKind::Panel);
    PlaceAt(decoration, {0, 50}, {300, 20});
    Element& middle = doc.Add(root, ElementKind::Button);
    middle.Focusable = true;
    PlaceAt(middle, {100, 100}, {100, 40});
    Element& bottom = doc.Add(root, ElementKind::Button);
    bottom.Focusable = true;
    PlaceAt(bottom, {100, 200}, {100, 40});

    // Tab: nothing focused → first focusable, then tree order, wrapping.
    CHECK(doc.Navigate(NavAction::Next));
    CHECK(doc.GetFocused() == &top);
    CHECK((top.State & ElementState::Focused) == ElementState::Focused);
    doc.Navigate(NavAction::Next);
    CHECK(doc.GetFocused() == &middle);
    doc.Navigate(NavAction::Next);
    CHECK(doc.GetFocused() == &bottom);
    doc.Navigate(NavAction::Next);
    CHECK(doc.GetFocused() == &top); // wrapped, skipping the non-focusable panel

    // Shift-Tab reverses.
    doc.Navigate(NavAction::Previous);
    CHECK(doc.GetFocused() == &bottom);

    // Directional (gamepad d-pad): from the top, Down moves to the nearest focusable below.
    doc.SetFocus(&top);
    CHECK(doc.Navigate(NavAction::MoveDown));
    CHECK(doc.GetFocused() == &middle);
    doc.Navigate(NavAction::MoveDown);
    CHECK(doc.GetFocused() == &bottom);
    // Up from the bottom returns toward the middle.
    doc.Navigate(NavAction::MoveUp);
    CHECK(doc.GetFocused() == &middle);
}

TEST_CASE("gui focus: gamepad confirm activates the focused element, cancel routes as an event")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& button = doc.Add(root, ElementKind::Button);
    button.Focusable = true;
    PlaceAt(button, {10, 10}, {100, 100});
    button.Bindings["onClick"] = "confirm";
    button.Bindings["onCancel"] = "cancel";

    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);

    int confirms = 0;
    int cancels = 0;
    context.SetHandler("confirm", [&](Element&) { ++confirms; });
    context.SetHandler("cancel", [&](Element&) { ++cancels; });

    doc.SetFocus(&button);
    // Confirm fires the same onClick a mouse click would.
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(confirms == 1);
    // Cancel fires the onCancel handler a menu registers.
    CHECK(doc.Navigate(NavAction::Cancel));
    CHECK(cancels == 1);
}

TEST_CASE("gui binding: a {obj.field} path resolves through reflection and updates on change")
{
    Document doc;

    Element& root = doc.Root();
    Element& nameLabel = doc.Add(root, ElementKind::Text);
    nameLabel.Bindings["text"] = "Name";
    Element& scoreLabel = doc.Add(root, ElementKind::Text);
    scoreLabel.Bindings["text"] = "Score";
    Element& hpLabel = doc.Add(root, ElementKind::Text);
    hpLabel.Bindings["text"] = "Vitals.Current"; // nested-struct path

    TypeRegistry registry;
    registry.Register<PlayerModel>();

    PlayerModel model;
    model.Name = "Ada";
    model.Score = 42;
    model.Vitals.Current = 7;

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);

    // The first binding pass resolves each path to its value.
    doc.UpdateBindings();
    CHECK(nameLabel.Text == "Ada");
    CHECK(scoreLabel.Text == "42");
    CHECK(hpLabel.Text == "7");

    // A pass with no version bump is a no-op — the values are unchanged even after a field mutation
    // the game has not yet signalled.
    model.Score = 99;
    doc.UpdateBindings();
    CHECK(scoreLabel.Text == "42");

    // Signalling the change re-reads the dirtied bindings and updates the element.
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(scoreLabel.Text == "99");
    CHECK(hpLabel.Text == "7");
}

TEST_CASE("gui interactivity: a display-only document hit-tests but routes no input")
{
    Document doc;
    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& button = doc.Add(root, ElementKind::Button);
    button.Focusable = true;
    PlaceAt(button, {10, 10}, {100, 100});
    button.Bindings["onClick"] = "start";

    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);
    int clicks = 0;
    context.SetHandler("start", [&](Element&) { ++clicks; });

    // Display-only by default: a click routes nothing.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(50, 50)};
    CHECK_FALSE(doc.DispatchPointer(down));
    PointerEvent up{.Kind = PointerEventKind::Up, .Position = vec2(50, 50)};
    CHECK_FALSE(doc.DispatchPointer(up));
    CHECK(clicks == 0);
    CHECK(doc.GetFocused() == nullptr);
    CHECK_FALSE(doc.Navigate(NavAction::Next));

    // Opening interactivity (the game's SeatFocusScope flips this) makes the same click live.
    doc.SetInteractive(true);
    doc.DispatchPointer(down);
    doc.DispatchPointer(up);
    CHECK(clicks == 1);
}

TEST_CASE("gui input: pointer-events children passes the element through but keeps its children")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});

    // A full-bleed backdrop over the whole region, holding one small control.
    Element& backdrop = doc.Add(root, ElementKind::Panel);
    PlaceAt(backdrop, {0, 0}, {200, 200});
    Element& control = doc.Add(backdrop, ElementKind::Button);
    PlaceAt(control, {10, 10}, {40, 40});

    // Auto: the backdrop claims every point its box covers, including away from the control.
    CHECK(doc.HitTest(vec2(100, 100)) == &backdrop);
    CHECK(doc.HitTest(vec2(20, 20)) == &control);

    // Children: the backdrop declines itself, so a point away from the control falls through to
    // the root — while the control inside it still hit-tests.
    backdrop.ComputedStyle.Pointer = PointerEvents::Children;
    CHECK(doc.HitTest(vec2(100, 100)) == &root);
    CHECK(doc.HitTest(vec2(20, 20)) == &control);

    // None remains the stronger form: the subtree goes with it, control included.
    backdrop.ComputedStyle.Pointer = PointerEvents::None;
    CHECK(doc.HitTest(vec2(100, 100)) == &root);
    CHECK(doc.HitTest(vec2(20, 20)) == &root);
}

TEST_CASE("gui events: hover marks every box the pointer is inside")
{
    // A composite control — a button spelled as a button wrapping its own texts — is the case this
    // exists for: the texts are pointer-transparent, so they can never be a hit target and could
    // never carry the bit on their own, and the selector grammar has no way to reach them from the
    // host's rule. Their host's hover is the only hover they have.
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& row = doc.Add(root, ElementKind::Button);
    PlaceAt(row, {0, 0}, {200, 60});
    Element& label = doc.Add(row, ElementKind::Text);
    PlaceAt(label, {0, 0}, {100, 60});
    Element& stamp = doc.Add(row, ElementKind::Text);
    PlaceAt(stamp, {100, 0}, {100, 60});

    Style transparent;
    transparent.Pointer = PointerEvents::None;
    doc.SetStyle(label, transparent);
    doc.SetStyle(stamp, transparent);
    doc.Update(0.0f);

    // Over the left text: the row is the hit target, and both of its texts take its state — the
    // one under the pointer and the one beside it alike, since neither has a state of its own.
    PointerEvent move{.Kind = PointerEventKind::Move, .Position = vec2(50, 30)};
    doc.DispatchPointer(move);
    CHECK((row.State & ElementState::Hovered) == ElementState::Hovered);
    CHECK((label.State & ElementState::Hovered) == ElementState::Hovered);
    CHECK((stamp.State & ElementState::Hovered) == ElementState::Hovered);
    // Every ancestor containing the pointer is hovered too, which is CSS's own rule.
    CHECK((root.State & ElementState::Hovered) == ElementState::Hovered);

    // Off the row: the whole set clears, ancestors and content together.
    PointerEvent away{.Kind = PointerEventKind::Move, .Position = vec2(50, 150)};
    doc.DispatchPointer(away);
    CHECK((row.State & ElementState::Hovered) == ElementState::None);
    CHECK((label.State & ElementState::Hovered) == ElementState::None);
    CHECK((stamp.State & ElementState::Hovered) == ElementState::None);
}
