// The within-document popup layer — headless cases over placement, paint/hit order, and dismissal.
// Device-free: trees are built directly and panels only, so no font or ICD is involved. Covers the
// two behaviors the layer exists for — a popup hit-tests ahead of the main tree and escapes every
// ancestor clip — plus the anchor-destruction rule that keeps a popup from outliving the row a
// repeater destroys under it, and the dismissal policy (light dismiss, Cancel, LIFO close).

#include <doctest/doctest.h>

#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    // One row a List repeats its item template over.
    struct Row
    {
        Veng::string Label;
    };

    // The binding data object whose array the repeater's item count follows.
    struct RowModel
    {
        Veng::vector<Row> Rows;
    };

    // Pins an element's document-space rect directly, bypassing the flex solve — the hit-order and
    // dismissal cases want fixed geometry, not a layout result.
    void PlaceAt(Element& element, vec2 min, vec2 size)
    {
        element.Layout = Rect{.Min = min, .Size = size};
    }

    // A sized panel style, the popup content's stand-in for a menu item.
    Style Box(f32 width, f32 height)
    {
        Style style;
        style.Width = Length::Points(width);
        style.Height = Length::Points(height);
        style.FlexShrink = 0.0f;
        return style;
    }
}

VE_REFLECT(::Row, 0x5109000000000001ULL)
VE_FIELD(Label)
VE_REFLECT_END();

VE_REFLECT(::RowModel, 0x5109000000000002ULL)
VE_ARRAY_FIELD(Rows)
VE_REFLECT_END();

TEST_CASE("gui popup: an open popup hit-tests ahead of the element it covers")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 200});

    Element& anchor = doc.Add(doc.Root(), ElementKind::Button);
    PlaceAt(anchor, {0, 0}, {200, 20});
    Element& covered = doc.Add(doc.Root(), ElementKind::Panel);
    PlaceAt(covered, {0, 20}, {200, 100});

    // Without a popup the point resolves to the main tree, as it always did.
    CHECK(doc.HitTest(vec2(40, 60)) == &covered);

    const PopupId id = doc.OpenPopup(anchor);
    REQUIRE(id.IsValid());
    Element* const root = doc.GetPopupRoot(id);
    REQUIRE(root != nullptr);
    Element& item = doc.Add(*root, ElementKind::Button);
    PlaceAt(*root, {0, 20}, {120, 80});
    PlaceAt(item, {0, 20}, {120, 80});

    // The popup paints over the main tree, so it claims the pointer over it.
    CHECK(doc.HitTest(vec2(40, 60)) == &item);
    // Outside the popup's own box the main tree still answers.
    CHECK(doc.HitTest(vec2(160, 60)) == &covered);

    doc.ClosePopup(id);
    CHECK(doc.GetPopupCount() == 0);
    CHECK(doc.HitTest(vec2(40, 60)) == &covered);
}

TEST_CASE("gui popup: a popup anchored inside a clipping ancestor escapes its clip")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 200});

    Style clipped;
    clipped.OverflowX = Overflow::Hidden;
    clipped.OverflowY = Overflow::Hidden;
    Element& view = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(view, clipped);
    PlaceAt(view, {0, 0}, {200, 40});

    Element& row = doc.Add(view, ElementKind::Button);
    PlaceAt(row, {0, 0}, {200, 20});

    // An absolute-positioned child of the clipping element is still bounded by its scissor: a
    // point past the view's bottom edge does not reach it. This is exactly the limitation the
    // popup layer exists to lift.
    Element& inside = doc.Add(view, ElementKind::Panel);
    PlaceAt(inside, {0, 20}, {200, 100});
    CHECK(doc.HitTest(vec2(40, 80)) == &doc.Root());

    const PopupId id = doc.OpenPopup(row);
    Element* const root = doc.GetPopupRoot(id);
    REQUIRE(root != nullptr);
    Element& item = doc.Add(*root, ElementKind::Button);
    PlaceAt(*root, {0, 20}, {120, 100});
    PlaceAt(item, {0, 20}, {120, 100});

    // The popup is not in the clipping element's subtree, so its scissor does not bound it.
    CHECK(doc.HitTest(vec2(40, 80)) == &item);
}

TEST_CASE("gui popup: a popup sizes to its content and clamps into the document extent")
{
    Document doc;

    Style rootStyle;
    rootStyle.Direction = FlexDirection::Column;
    doc.SetStyle(doc.Root(), rootStyle);

    Element& anchor = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(anchor, Box(60.0f, 30.0f));

    const PopupId id = doc.OpenPopup(
        anchor, PopupOptions{.Side = PopupSide::Below, .Offset = vec2(4.0f, 2.0f), .Margin = 5.0f});
    Element* const root = doc.GetPopupRoot(id);
    REQUIRE(root != nullptr);
    Style column;
    column.Direction = FlexDirection::Column;
    doc.SetStyle(*root, column);
    Element& first = doc.Add(*root, ElementKind::Panel);
    doc.SetStyle(first, Box(90.0f, 25.0f));
    Element& second = doc.Add(*root, ElementKind::Panel);
    doc.SetStyle(second, Box(90.0f, 25.0f));

    doc.Solve(vec2(200.0f, 200.0f));

    // The root took its content's extent: two 90x25 items stacked.
    CHECK(root->Layout.Size.x == doctest::Approx(90.0f));
    CHECK(root->Layout.Size.y == doctest::Approx(50.0f));
    // Placed below the anchor's box (which sits at the document origin, 30px tall), plus the
    // offset — with the x offset of 4 raised to the 5px margin the clamp keeps clear.
    CHECK(root->Layout.Min.x == doctest::Approx(5.0f));
    CHECK(root->Layout.Min.y == doctest::Approx(32.0f));
    // The children rode the placement, so the subtree is laid out in document space.
    CHECK(second.Layout.Min.y == doctest::Approx(57.0f));

    // A popup that would overflow the document is slid back inside, the margin kept clear: at a
    // 70px-tall extent the 50px-tall popup can only reach y = 70 - 5 - 50.
    doc.Solve(vec2(120.0f, 70.0f));
    CHECK(root->Layout.Min.x == doctest::Approx(5.0f));
    CHECK(root->Layout.Min.y == doctest::Approx(15.0f));
}

TEST_CASE("gui popup: destroying the anchor closes the popup")
{
    // The repeater case the anchor handle exists for: a popup opened from a list row, whose bound
    // array then shrinks. The row's whole subtree is destroyed by the re-sync, and the popup must
    // go with it rather than placing itself against a freed rect.
    Document doc;
    TypeRegistry registry;
    BindingContext context;
    RowModel model;

    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 400});

    Element& list = doc.Add(doc.Root(), ElementKind::List);
    list.Bindings["items"] = "Rows";
    doc.InitWidget(list);
    Element& rowTemplate = doc.Add(list, ElementKind::Button);
    rowTemplate.Bindings["text"] = "Label";

    registry.Register<RowModel>();
    model.Rows = {Row{.Label = "a"}, Row{.Label = "b"}, Row{.Label = "c"}};
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();
    REQUIRE(list.Children.size() == 3);

    Element& lastRow = *list.Children[2];
    PlaceAt(lastRow, {0, 80}, {200, 40});
    const ElementHandle anchorHandle = doc.GetHandle(lastRow);
    const PopupId id = doc.OpenPopup(lastRow);
    Element& item = doc.Add(*doc.GetPopupRoot(id), ElementKind::Button);
    PlaceAt(*doc.GetPopupRoot(id), {0, 120}, {120, 40});
    PlaceAt(item, {0, 120}, {120, 40});
    CHECK(doc.HitTest(vec2(40, 140)) == &item);
    CHECK(doc.IsPopupOpen(id));

    // Shrink the bound array: the re-sync destroys the third row, which is the popup's anchor.
    model.Rows.pop_back();
    context.Invalidate();
    doc.UpdateBindings();
    REQUIRE(list.Children.size() == 2);

    CHECK_FALSE(doc.IsPopupOpen(id));
    CHECK(doc.GetPopupCount() == 0);
    CHECK(doc.GetPopupRoot(id) == nullptr);
    // The handle to the destroyed row resolves to nothing rather than to whatever re-used its
    // storage — the generation check the anchor is held through.
    CHECK(doc.Resolve(anchorHandle) == nullptr);
    // Nothing dangles: the popup's elements are gone and the hit-test falls back to the main tree.
    CHECK(doc.HitTest(vec2(40, 140)) == &doc.Root());
    // A re-solve over the shrunken tree walks no freed popup.
    doc.Solve(vec2(200.0f, 400.0f));
}

TEST_CASE("gui popup: a press outside the top popup dismisses it and is consumed")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 200});

    i32 clicks = 0;
    const TypeRegistry registry;
    BindingContext context;
    context.SetHandler("hit", [&](Element&) { ++clicks; });
    doc.BindContext(&context, &registry);

    Element& anchor = doc.Add(doc.Root(), ElementKind::Button);
    PlaceAt(anchor, {0, 0}, {200, 20});
    Element& below = doc.Add(doc.Root(), ElementKind::Button);
    below.Bindings["onClick"] = "hit";
    PlaceAt(below, {0, 140}, {200, 40});

    const PopupId id = doc.OpenPopup(anchor);
    Element& item = doc.Add(*doc.GetPopupRoot(id), ElementKind::Button);
    PlaceAt(*doc.GetPopupRoot(id), {0, 20}, {120, 60});
    PlaceAt(item, {0, 20}, {120, 60});

    // A press inside the popup does not dismiss it.
    PointerEvent inside{.Kind = PointerEventKind::Down, .Position = vec2(40, 40)};
    CHECK(doc.DispatchPointer(inside));
    CHECK(doc.IsPopupOpen(id));
    PointerEvent insideUp{.Kind = PointerEventKind::Up, .Position = vec2(40, 40)};
    doc.DispatchPointer(insideUp);

    // A press outside closes it, and the click never reaches the button it landed on.
    PointerEvent outside{.Kind = PointerEventKind::Down, .Position = vec2(40, 160)};
    CHECK(doc.DispatchPointer(outside));
    CHECK_FALSE(doc.IsPopupOpen(id));
    PointerEvent outsideUp{.Kind = PointerEventKind::Up, .Position = vec2(40, 160)};
    doc.DispatchPointer(outsideUp);
    CHECK(clicks == 0);

    // With the popup gone the same press/release is an ordinary click again.
    PointerEvent again{.Kind = PointerEventKind::Down, .Position = vec2(40, 160)};
    doc.DispatchPointer(again);
    PointerEvent againUp{.Kind = PointerEventKind::Up, .Position = vec2(40, 160)};
    doc.DispatchPointer(againUp);
    CHECK(clicks == 1);
}

TEST_CASE("gui popup: LightDismiss off keeps a popup open under an outside press")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 200});

    Element& anchor = doc.Add(doc.Root(), ElementKind::Button);
    PlaceAt(anchor, {0, 0}, {200, 20});

    const PopupId id = doc.OpenPopup(anchor, PopupOptions{.LightDismiss = false});
    PlaceAt(*doc.GetPopupRoot(id), {0, 20}, {120, 60});

    PointerEvent outside{.Kind = PointerEventKind::Down, .Position = vec2(40, 160)};
    doc.DispatchPointer(outside);
    CHECK(doc.IsPopupOpen(id));
}

TEST_CASE("gui popup: Cancel closes the top popup before it reaches the focused element")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 200});

    i32 cancels = 0;
    const TypeRegistry registry;
    BindingContext context;
    context.SetHandler("bail", [&](Element&) { ++cancels; });
    doc.BindContext(&context, &registry);

    Element& anchor = doc.Add(doc.Root(), ElementKind::Button);
    anchor.Bindings["onCancel"] = "bail";
    PlaceAt(anchor, {0, 0}, {200, 20});
    doc.InitWidget(anchor);
    doc.SetFocus(&anchor);

    const PopupId id = doc.OpenPopup(anchor);
    PlaceAt(*doc.GetPopupRoot(id), {0, 20}, {120, 60});

    CHECK(doc.Navigate(NavAction::Cancel));
    CHECK_FALSE(doc.IsPopupOpen(id));
    CHECK(cancels == 0);

    // With nothing open, Cancel reaches the focused element's handler as it always did.
    CHECK(doc.Navigate(NavAction::Cancel));
    CHECK(cancels == 1);
}

TEST_CASE("gui popup: the stack closes LIFO and restores the pre-open focus")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 200});

    Element& anchor = doc.Add(doc.Root(), ElementKind::Button);
    PlaceAt(anchor, {0, 0}, {200, 20});
    doc.InitWidget(anchor);
    doc.SetFocus(&anchor);
    REQUIRE(doc.GetFocused() == &anchor);

    const PopupId menu = doc.OpenPopup(anchor);
    Element& menuItem = doc.Add(*doc.GetPopupRoot(menu), ElementKind::Button);
    doc.InitWidget(menuItem);
    PlaceAt(*doc.GetPopupRoot(menu), {0, 20}, {120, 60});
    PlaceAt(menuItem, {0, 20}, {120, 30});
    doc.SetFocus(&menuItem);

    // A submenu anchored inside the menu — the second stack entry.
    const PopupId submenu = doc.OpenPopup(menuItem, PopupOptions{.Side = PopupSide::RightOf});
    PlaceAt(*doc.GetPopupRoot(submenu), {120, 20}, {120, 60});
    CHECK(doc.GetPopupCount() == 2);
    CHECK(doc.GetTopPopup() == submenu);

    // Focus navigation is scoped to the top popup: the menu's own stops are behind it.
    CHECK_FALSE(doc.Navigate(NavAction::Next));

    // Closing the menu takes its submenu with it, and focus returns to where it was before the
    // menu opened.
    doc.ClosePopup(menu);
    CHECK(doc.GetPopupCount() == 0);
    CHECK_FALSE(doc.IsPopupOpen(submenu));
    CHECK(doc.GetFocused() == &anchor);

    // A stale id names nothing: the serial is monotonic, so it is never re-issued.
    CHECK(doc.GetPopupRoot(menu) == nullptr);
    CHECK_FALSE(doc.IsPopupOpen(menu));
}

TEST_CASE("gui popup: closing the anchor's own popup subtree is safe, and display-only closes all")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 200});

    Element& anchor = doc.Add(doc.Root(), ElementKind::Button);
    PlaceAt(anchor, {0, 0}, {200, 20});

    const PopupId id = doc.OpenPopup(anchor);
    doc.Add(*doc.GetPopupRoot(id), ElementKind::Panel);
    CHECK(doc.GetPopupCount() == 1);

    // Popups belong to interactive documents, so closing interactivity dismisses them.
    doc.SetInteractive(false);
    CHECK(doc.GetPopupCount() == 0);
    CHECK_FALSE(doc.IsPopupOpen(id));
}

TEST_CASE("gui popup: a popup's primitives are emitted after the whole main tree")
{
    Document doc;

    Style rootStyle;
    rootStyle.Direction = FlexDirection::Column;
    rootStyle.Background = vec4(1.0f, 0.0f, 0.0f, 1.0f);
    doc.SetStyle(doc.Root(), rootStyle);

    Style clipped = Box(60.0f, 30.0f);
    clipped.OverflowY = Overflow::Hidden;
    clipped.Background = vec4(0.0f, 1.0f, 0.0f, 1.0f);
    Element& anchor = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(anchor, clipped);

    const PopupId id = doc.OpenPopup(anchor);
    Style fill = Box(90.0f, 40.0f);
    fill.Background = vec4(0.0f, 0.0f, 1.0f, 1.0f);
    doc.SetStyle(*doc.GetPopupRoot(id), fill);

    doc.Solve(vec2(200.0f, 200.0f));

    DrawList list;
    doc.Build(list);

    // The popup's quad is last, so it paints over every main-tree primitive.
    const vector<GuiVertex>& vertices = list.GetVertices();
    REQUIRE(vertices.size() >= 12);
    const GuiVertex& last = vertices[vertices.size() - 1];
    CHECK(last.Color.b == doctest::Approx(1.0f));
    CHECK(last.Color.g == doctest::Approx(0.0f));
}
