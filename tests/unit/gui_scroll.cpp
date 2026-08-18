// Overflow-driven scrolling and its widget-owned scrollbars — headless, device-free. Covers the
// property replacing the kind (any element styled `overflow: scroll` scrolls; ScrollView is the
// preset that defaults it on), per-axis independence, the ScrollBar/ScrollBarThumb shadow elements
// and their geometry, thumb drag, gutter-vs-overlay reservation, and the invariant that the parts
// stay out of every content-shaped walk (item slots, focus order, list sync). Also covers the
// Slider's fill and thumb, which are widget-owned parts on the same mechanism.

#include <algorithm>

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
    Length Points(f32 value)
    {
        return Length{.Kind = LengthKind::Points, .Value = value};
    }

    // A fixed-size box holding one taller child, so the vertical axis has real travel.
    struct ScrollFixture
    {
        Document Doc;
        Element* View = nullptr;
        Element* Content = nullptr;

        ScrollFixture(Overflow x, Overflow y, ScrollbarLayout bars = ScrollbarLayout::Overlay,
                      ElementKind kind = ElementKind::Panel)
        {
            Doc.SetInteractive(true);

            View = &Doc.Add(Doc.Root(), kind);
            Style view;
            view.Width = Points(100.0f);
            view.Height = Points(100.0f);
            view.OverflowX = x;
            view.OverflowY = y;
            view.Scrollbar = bars;
            Doc.SetStyle(*View, view);

            Content = &Doc.Add(*View, ElementKind::Panel);
            Style content;
            // Width fills whatever content box the container offers, so a gutter's reservation is
            // observable; height overflows to give the vertical axis its travel.
            content.Width = Length{.Kind = LengthKind::Percent, .Value = 100.0f};
            content.Height = Points(300.0f);
            // Scrollable content holds its size rather than shrinking to the box — the authoring
            // requirement a scroll container has in CSS too, and what gives the axis its travel.
            content.FlexShrink = 0.0f;
            Doc.SetStyle(*Content, content);

            Settle();
        }

        // One full frame: Update reconciles the bars against the resolved style, Solve lays out.
        void Settle()
        {
            Doc.Update(0.0f);
            Doc.Solve(vec2(400.0f, 400.0f));
        }

        // Every element under the root, parts included — what a recursively-growing tree moves.
        [[nodiscard]] static usize CountSubtree(const Element& element)
        {
            usize count = 1;
            for (const Element* child : element.Children)
            {
                count += CountSubtree(*child);
            }
            return count;
        }

        [[nodiscard]] const Element* Bar(bool vertical) const
        {
            for (const Element* child : View->Children)
            {
                if (child->Kind == ElementKind::ScrollBar && child->Widget.Vertical == vertical)
                {
                    return child;
                }
            }
            return nullptr;
        }
    };
}

TEST_CASE("gui scroll: any element styled overflow: scroll scrolls, with no ScrollView kind")
{
    ScrollFixture fixture(Overflow::Scroll, Overflow::Scroll);

    // A plain Panel — the capability is the property, not the kind.
    CHECK(fixture.View->Kind == ElementKind::Panel);
    fixture.Doc.ScrollBy(*fixture.View, vec2(0.0f, 50.0f));
    CHECK(fixture.View->Widget.ScrollOffset.y == doctest::Approx(50.0f));

    // The offset clamps to the content's travel (300 content − 100 box) rather than running on.
    fixture.Doc.ScrollBy(*fixture.View, vec2(0.0f, 1000.0f));
    CHECK(fixture.View->Widget.ScrollOffset.y == doctest::Approx(200.0f));
}

TEST_CASE("gui scroll: ScrollView is the preset, and an authored axis still overrides it")
{
    // The kind seeds the base style, so it scrolls with nothing authored.
    const ScrollFixture preset(Overflow::Scroll, Overflow::Scroll, ScrollbarLayout::Overlay,
                               ElementKind::ScrollView);
    CHECK(preset.Bar(true) != nullptr);

    // A ScrollView whose style names an axis takes the authored value — the seed is a default the
    // cascade writes over, not a behavior forced by the kind.
    Document doc;
    Element& view = doc.Add(doc.Root(), ElementKind::ScrollView);
    CHECK(view.BaseStyle.OverflowY == Overflow::Scroll);
    Style style = view.BaseStyle;
    style.OverflowY = Overflow::Hidden;
    doc.SetStyle(view, style);
    doc.Update(0.0f);
    CHECK(view.ComputedStyle.OverflowY == Overflow::Hidden);
}

TEST_CASE("gui scroll: the axes are independent — a hidden axis has no travel and no bar")
{
    ScrollFixture fixture(Overflow::Hidden, Overflow::Scroll);

    // A diagonal scroll moves only the axis that scrolls; the hidden axis clips without moving.
    fixture.Doc.ScrollBy(*fixture.View, vec2(80.0f, 80.0f));
    CHECK(fixture.View->Widget.ScrollOffset.x == doctest::Approx(0.0f));
    CHECK(fixture.View->Widget.ScrollOffset.y == doctest::Approx(80.0f));

    // Only the scrollable axis owns a bar.
    CHECK(fixture.Bar(true) != nullptr);
    CHECK(fixture.Bar(false) == nullptr);
}

TEST_CASE("gui scroll: a scrollable axis owns a ScrollBar carrying a ScrollBarThumb")
{
    ScrollFixture fixture(Overflow::Hidden, Overflow::Scroll);

    const Element* const bar = fixture.Bar(true);
    REQUIRE(bar != nullptr);
    REQUIRE(bar->Children.size() == 1);
    CHECK(bar->Children.front()->Kind == ElementKind::ScrollBarThumb);

    // The parts carry an axis class, so `ScrollBar.vertical { … }` is an ordinary class selector.
    CHECK(std::ranges::find(bar->Classes, "vertical") != bar->Classes.end());

    // The bar pins to the box's right edge at its styled thickness, full height.
    CHECK(bar->Layout.Max().x == doctest::Approx(fixture.View->Layout.Max().x));
    CHECK(bar->Layout.Size.y == doctest::Approx(fixture.View->Layout.Size.y));

    // The thumb's length is the visible fraction of the content: 100 of 300 over a 100px track.
    const Element& thumb = *bar->Children.front();
    CHECK(thumb.Layout.Size.y == doctest::Approx(100.0f / 3.0f).epsilon(0.05));
    CHECK(thumb.Layout.Min.y == doctest::Approx(bar->Layout.Min.y));

    // Scrolling to the end drives the thumb to the bottom of the track.
    fixture.Doc.ScrollBy(*fixture.View, vec2(0.0f, 1000.0f));
    fixture.Settle();
    CHECK(thumb.Layout.Max().y == doctest::Approx(bar->Layout.Max().y).epsilon(0.05));
}

TEST_CASE("gui scroll: a bar exists while the axis is scrollable and hides when there is no travel")
{
    ScrollFixture fixture(Overflow::Hidden, Overflow::Scroll);
    REQUIRE(fixture.Bar(true) != nullptr);
    CHECK(fixture.Bar(true)->Visible);

    // Shrinking the content below the box leaves the bar in place but hidden: presence follows the
    // style, visibility follows whether there is anything to scroll.
    Style content = fixture.Content->BaseStyle;
    content.Height = Points(50.0f);
    fixture.Doc.SetStyle(*fixture.Content, content);
    fixture.Settle();
    CHECK(fixture.Bar(true) != nullptr);
    CHECK(!fixture.Bar(true)->Visible);
}

TEST_CASE("gui scroll: a scrollbar never takes scrollbars of its own")
{
    // A part inherits its host's classes, so a bare class rule carrying `overflow: scroll` resolves
    // onto the very bar the host's own rule created. Left unguarded that bar takes a bar, whose
    // parts inherit the classes again, and the tree grows a level per frame until the process dies —
    // a stylesheet must not be able to hang the app. So a scrollbar part is excluded from the sync
    // outright: a bar is never itself a scroll container, whatever style lands on it.
    ScrollFixture fixture(Overflow::Scroll, Overflow::Scroll);

    const Element* const bar = fixture.Bar(/*vertical=*/true);
    REQUIRE(bar != nullptr);

    // Style the bar exactly as an inherited class rule would, then run several frames: the count is
    // what a growing tree moves, so it is asserted after settling rather than per frame.
    Style scrolling = bar->ComputedStyle;
    scrolling.OverflowX = Overflow::Scroll;
    scrolling.OverflowY = Overflow::Scroll;
    fixture.Doc.SetStyle(const_cast<Element&>(*bar), scrolling);

    const usize before = ScrollFixture::CountSubtree(fixture.Doc.Root());
    for (int frame = 0; frame < 8; ++frame)
    {
        fixture.Settle();
    }
    CHECK(ScrollFixture::CountSubtree(fixture.Doc.Root()) == before);

    // And the bar's own children are still just its thumb — no bar of its own was made.
    CHECK(std::ranges::none_of(bar->Children, [](const Element* child)
                               { return child->Kind == ElementKind::ScrollBar; }));
}

TEST_CASE("gui scroll: turning the axis un-scrollable drops the bar entirely")
{
    ScrollFixture fixture(Overflow::Hidden, Overflow::Scroll);
    REQUIRE(fixture.Bar(true) != nullptr);

    Style style = fixture.View->BaseStyle;
    style.OverflowY = Overflow::Hidden;
    fixture.Doc.SetStyle(*fixture.View, style);
    fixture.Settle();
    CHECK(fixture.Bar(true) == nullptr);
    CHECK(fixture.View->Children.size() == 1); // the content child alone
}

TEST_CASE("gui scroll: dragging the thumb scrolls the content through the track's slack")
{
    ScrollFixture fixture(Overflow::Hidden, Overflow::Scroll);
    const Element* const bar = fixture.Bar(true);
    REQUIRE(bar != nullptr);
    const Element& thumb = *bar->Children.front();

    // Press on the thumb, then drag down a third of the track; the content travels the matching
    // fraction of its own range rather than the raw pixel delta.
    const vec2 grab = thumb.Layout.Center();
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = grab};
    fixture.Doc.DispatchPointer(down);

    const f32 slack = bar->Layout.Size.y - thumb.Layout.Size.y;
    PointerEvent move{.Kind = PointerEventKind::Move, .Position = grab + vec2(0.0f, slack / 3.0f)};
    fixture.Doc.DispatchPointer(move);

    // Range is 200 (300 content − 100 box), so a third of the slack is a third of the range.
    CHECK(fixture.View->Widget.ScrollOffset.y == doctest::Approx(200.0f / 3.0f).epsilon(0.05));
}

TEST_CASE("gui scroll: a gutter narrows the content box while an overlay does not")
{
    const ScrollFixture overlay(Overflow::Hidden, Overflow::Scroll, ScrollbarLayout::Overlay);
    const ScrollFixture gutter(Overflow::Hidden, Overflow::Scroll, ScrollbarLayout::Gutter);

    // The content fills whatever width its container offers, so the reservation is visible in its
    // solved width: the gutter takes the bar's thickness out of it and the overlay takes nothing.
    const f32 thickness = gutter.Bar(true)->Layout.Size.x;
    CHECK(thickness > 0.0f);
    CHECK(gutter.Content->Layout.Size.x ==
          doctest::Approx(overlay.Content->Layout.Size.x - thickness));
}

namespace
{
    struct Row
    {
        string Label;
    };
    struct Model
    {
        vector<Row> Rows;
    };
}

VE_REFLECT(::Row, 0x5C401A0000000001ULL)
VE_FIELD(Label)
VE_REFLECT_END();

VE_REFLECT(::Model, 0x5C401A0000000002ULL)
VE_ARRAY_FIELD(Rows)
VE_REFLECT_END();

TEST_CASE("gui scroll: a scrollable List's bars stay out of its item slots and focus order")
{
    // The regression the ContentChildren accessor exists to prevent: the bars are Children, so a
    // slot walk, a focus walk, or a shrink that addressed Children directly would treat a bar as
    // an item — and the shrink would delete one.
    Document doc;
    doc.SetInteractive(true);

    Element& list = doc.Add(doc.Root(), ElementKind::List);
    list.Bindings["items"] = "Rows";
    list.Bindings["selection"] = "multiple";
    Style style;
    style.Width = Points(100.0f);
    style.Height = Points(60.0f);
    style.OverflowY = Overflow::Scroll;
    style.Direction = FlexDirection::Column;
    doc.SetStyle(list, style);
    doc.InitWidget(list);

    Element& itemTemplate = doc.Add(list, ElementKind::Text);
    itemTemplate.Bindings["text"] = "Label";
    // The rows need real height to stack: no font is resident headless, so a Text measures zero
    // and directional navigation would have no geometry to pick a neighbour by.
    Style row;
    row.Height = Points(20.0f);
    row.FlexShrink = 0.0f;
    doc.SetStyle(itemTemplate, row);

    TypeRegistry registry;
    registry.Register<Model>();
    Model model;
    for (int i = 0; i < 4; ++i)
    {
        model.Rows.push_back(Row{.Label = "row"});
    }
    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();
    doc.Update(0.0f);
    doc.Solve(vec2(400.0f, 400.0f));

    // The bar is a child but not an item: four rows, and the list reports four slots.
    REQUIRE(doc.GetItemCount(list) == 4);
    CHECK(list.Children.size() == 5); // four items plus the vertical bar
    CHECK(list.Children.back()->Kind == ElementKind::ScrollBar);

    // Selection indexes items, never the bar.
    doc.SelectItem(list, 3, true);
    CHECK(doc.IsItemSelected(list, 3));
    CHECK((list.Children[3]->State & ElementState::Selected) == ElementState::Selected);
    CHECK((list.Children.back()->State & ElementState::Selected) == ElementState::None);

    // A shrink removes items from the content tail, not the bar sitting after them.
    model.Rows.resize(2);
    context.Invalidate();
    doc.UpdateBindings();
    doc.Update(0.0f);
    CHECK(doc.GetItemCount(list) == 2);
    CHECK(list.Children.size() == 3);
    CHECK(list.Children.back()->Kind == ElementKind::ScrollBar);

    // A bar is never a focus stop, so arrowing the list walks items only.
    doc.SetFocus(list.Children[0]);
    CHECK(doc.Navigate(NavAction::MoveDown));
    CHECK(doc.GetFocused() == list.Children[1]);
    CHECK(doc.GetFocused()->Kind != ElementKind::ScrollBar);
}

TEST_CASE("gui widget parts: a Slider's fill and thumb are elements placed from its value")
{
    Document doc;
    Element& slider = doc.Add(doc.Root(), ElementKind::Slider);
    Style style;
    style.Width = Points(100.0f);
    style.Height = Points(10.0f);
    doc.SetStyle(slider, style);
    slider.Bindings["min"] = "0";
    slider.Bindings["max"] = "100";
    doc.InitWidget(slider);

    doc.SetWidgetValue(slider, 50.0f);
    doc.Update(0.0f);
    doc.Solve(vec2(200.0f, 200.0f));

    const Element* fill = nullptr;
    const Element* thumb = nullptr;
    for (const Element* child : slider.Children)
    {
        if (child->Kind == ElementKind::SliderFill)
        {
            fill = child;
        }
        if (child->Kind == ElementKind::SliderThumb)
        {
            thumb = child;
        }
    }
    REQUIRE(fill != nullptr);
    REQUIRE(thumb != nullptr);

    // The fill spans the value's fraction of the track; the square thumb rides it.
    CHECK(fill->Layout.Size.x == doctest::Approx(50.0f));
    CHECK(thumb->Layout.Size.x == doctest::Approx(10.0f));
    CHECK(thumb->Layout.Min.x == doctest::Approx(45.0f));

    // Moving the value re-places both immediately, with no re-solve: a dragged slider must not
    // re-run the flex solve on every pointer move.
    doc.SetWidgetValue(slider, 0.0f);
    CHECK(!doc.IsDirty());
    CHECK(thumb->Layout.Min.x == doctest::Approx(slider.Layout.Min.x));
    CHECK(!fill->Visible);
}

TEST_CASE("gui widget parts: a part inherits its host's classes so it is addressable per instance")
{
    // The selector engine matches one compound selector with no descendant combinator, so a part
    // carrying its host's classes is what makes `SliderThumb.volume` reach one slider's thumb.
    Document doc;
    Element& slider = doc.Add(doc.Root(), ElementKind::Slider);
    slider.Classes.emplace_back("volume");
    doc.InitWidget(slider);

    Element* thumb = nullptr;
    for (Element* child : slider.Children)
    {
        if (child->Kind == ElementKind::SliderThumb)
        {
            thumb = child;
        }
    }
    REQUIRE(thumb != nullptr);
    CHECK(std::ranges::find(thumb->Classes, "volume") != thumb->Classes.end());
}

TEST_CASE("gui widget parts: a scrollbar inherits its host's classes beside its axis tag")
{
    ScrollFixture fixture(Overflow::Hidden, Overflow::Scroll);
    fixture.View->Classes.emplace_back("panel");

    // Re-create the bar now the host carries the class, the way an instantiated tree would have.
    Style style = fixture.View->BaseStyle;
    style.OverflowY = Overflow::Hidden;
    fixture.Doc.SetStyle(*fixture.View, style);
    fixture.Settle();
    style.OverflowY = Overflow::Scroll;
    fixture.Doc.SetStyle(*fixture.View, style);
    fixture.Settle();

    const Element* const bar = fixture.Bar(true);
    REQUIRE(bar != nullptr);
    CHECK(std::ranges::find(bar->Classes, "panel") != bar->Classes.end());
    CHECK(std::ranges::find(bar->Classes, "vertical") != bar->Classes.end());
}

TEST_CASE("gui widget parts: pressing a Slider's thumb drives the slider, not the thumb")
{
    // The thumb is a part *inside* the control it belongs to, and unlike a scrollbar thumb it has
    // no drag of its own — the Slider already maps pointer position to value. So a press on it has
    // to reach the Slider rather than being claimed by the part the pointer happened to land on.
    Document doc;
    doc.SetInteractive(true);

    Element& slider = doc.Add(doc.Root(), ElementKind::Slider);
    Style style;
    style.Width = Points(100.0f);
    style.Height = Points(10.0f);
    doc.SetStyle(slider, style);
    slider.Bindings["min"] = "0";
    slider.Bindings["max"] = "100";
    doc.InitWidget(slider);
    doc.Update(0.0f);
    doc.Solve(vec2(200.0f, 200.0f));
    doc.SetWidgetValue(slider, 50.0f);

    const Element* thumb = nullptr;
    for (const Element* child : slider.Children)
    {
        if (child->Kind == ElementKind::SliderThumb)
        {
            thumb = child;
        }
    }
    REQUIRE(thumb != nullptr);

    // Press squarely on the thumb, then drag toward the track's start.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = thumb->Layout.Center()};
    CHECK(doc.DispatchPointer(down));
    PointerEvent move{.Kind = PointerEventKind::Move,
                      .Position = vec2(slider.Layout.Min.x + 10.0f, thumb->Layout.Center().y)};
    doc.DispatchPointer(move);
    CHECK(doc.GetWidgetValue(slider) == doctest::Approx(10.0f));
}

TEST_CASE("gui widget parts: pressing a vertical Slider's thumb drives it bottom-up")
{
    Document doc;
    doc.SetInteractive(true);

    Element& slider = doc.Add(doc.Root(), ElementKind::Slider);
    Style style;
    style.Width = Points(10.0f);
    style.Height = Points(100.0f);
    doc.SetStyle(slider, style);
    slider.Bindings["min"] = "0";
    slider.Bindings["max"] = "100";
    slider.Bindings["orientation"] = "vertical";
    doc.InitWidget(slider);
    doc.Update(0.0f);
    doc.Solve(vec2(200.0f, 200.0f));
    doc.SetWidgetValue(slider, 50.0f);

    const Element* thumb = nullptr;
    for (const Element* child : slider.Children)
    {
        if (child->Kind == ElementKind::SliderThumb)
        {
            thumb = child;
        }
    }
    REQUIRE(thumb != nullptr);

    PointerEvent down{.Kind = PointerEventKind::Down, .Position = thumb->Layout.Center()};
    CHECK(doc.DispatchPointer(down));
    // Near the bottom edge is Min on a vertical slider.
    PointerEvent move{.Kind = PointerEventKind::Move,
                      .Position = vec2(thumb->Layout.Center().x, slider.Layout.Max().y - 10.0f)};
    doc.DispatchPointer(move);
    CHECK(doc.GetWidgetValue(slider) == doctest::Approx(10.0f));
}
