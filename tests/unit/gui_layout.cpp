// Gui retained tree + Yoga layout: build trees with the imperative API, Solve at a known
// available size, and assert every Element::Layout rect against hand-computed flexbox results.
// Device-free — layout is pure CPU (no ICD, no font resource; the text leaf measures through an
// injected measurer). A second case drives Build into a DrawList and pins the run count/order.

#include <algorithm>
#include <cmath>
#include <utility>

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    constexpr f32 Eps = 0.01f;

    void CheckRect(const Rect& rect, f32 x, f32 y, f32 w, f32 h)
    {
        CHECK(rect.Min.x == doctest::Approx(x).epsilon(Eps));
        CHECK(rect.Min.y == doctest::Approx(y).epsilon(Eps));
        CHECK(rect.Size.x == doctest::Approx(w).epsilon(Eps));
        CHECK(rect.Size.y == doctest::Approx(h).epsilon(Eps));
    }
}

TEST_CASE("gui layout: a row of equal-grow children splits the width")
{
    Document doc;

    Style rowStyle;
    rowStyle.Direction = FlexDirection::Row;
    doc.SetStyle(doc.Root(), rowStyle);

    Style grow;
    grow.FlexGrow = 1.0f;

    Element& a = doc.Add(doc.Root(), ElementKind::Panel);
    Element& b = doc.Add(doc.Root(), ElementKind::Panel);
    Element& c = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(a, grow);
    doc.SetStyle(b, grow);
    doc.SetStyle(c, grow);

    doc.Solve(vec2(300.0f, 100.0f));

    // Root fills the available region; three equal-grow children split its width, full height.
    CheckRect(doc.Root().Layout, 0.0f, 0.0f, 300.0f, 100.0f);
    CheckRect(a.Layout, 0.0f, 0.0f, 100.0f, 100.0f);
    CheckRect(b.Layout, 100.0f, 0.0f, 100.0f, 100.0f);
    CheckRect(c.Layout, 200.0f, 0.0f, 100.0f, 100.0f);
}

TEST_CASE("gui layout: nested padding offsets and shrinks a stretched child")
{
    Document doc;

    Style padded;
    padded.Direction = FlexDirection::Column;
    padded.Padding = Insets::All(20.0f);
    doc.SetStyle(doc.Root(), padded);

    Style fill;
    fill.FlexGrow = 1.0f;
    Element& inner = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(inner, fill);

    doc.Solve(vec2(200.0f, 200.0f));

    // The inner box is inset by the padding on every edge and fills the remaining content box.
    CheckRect(inner.Layout, 20.0f, 20.0f, 160.0f, 160.0f);
}

TEST_CASE("gui layout: fixed-size children wrap onto a second row")
{
    Document doc;

    Style wrapRow;
    wrapRow.Direction = FlexDirection::Row;
    wrapRow.Wrap = FlexWrap::Wrap;
    wrapRow.AlignItems = Align::FlexStart;
    doc.SetStyle(doc.Root(), wrapRow);

    Style box;
    box.Width = Length::Points(60.0f);
    box.Height = Length::Points(40.0f);
    box.FlexShrink = 0.0f;

    Element& a = doc.Add(doc.Root(), ElementKind::Panel);
    Element& b = doc.Add(doc.Root(), ElementKind::Panel);
    Element& c = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(a, box);
    doc.SetStyle(b, box);
    doc.SetStyle(c, box);

    // Width 100 fits one 60px box per line, so each of the three wraps onto its own row.
    doc.Solve(vec2(100.0f, 200.0f));

    CheckRect(a.Layout, 0.0f, 0.0f, 60.0f, 40.0f);
    CheckRect(b.Layout, 0.0f, 40.0f, 60.0f, 40.0f);
    CheckRect(c.Layout, 0.0f, 80.0f, 60.0f, 40.0f);
}

TEST_CASE("gui layout: an absolutely-positioned overlay ignores flow and honors its insets")
{
    Document doc;

    Style flow;
    flow.Direction = FlexDirection::Column;
    doc.SetStyle(doc.Root(), flow);

    Style header;
    header.Height = Length::Points(30.0f);
    Element& banner = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(banner, header);

    Style overlay;
    overlay.Position = PositionType::Absolute;
    overlay.Inset = {.Left = 10.0f, .Top = 10.0f, .Right = 0.0f, .Bottom = 0.0f};
    overlay.Width = Length::Points(50.0f);
    overlay.Height = Length::Points(50.0f);
    Element& popup = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(popup, overlay);

    doc.Solve(vec2(200.0f, 200.0f));

    // The banner flows at the top; the overlay is removed from flow and placed by its insets.
    CheckRect(banner.Layout, 0.0f, 0.0f, 200.0f, 30.0f);
    CheckRect(popup.Layout, 10.0f, 10.0f, 50.0f, 50.0f);
}

TEST_CASE("gui layout: a text leaf reflows to a taller box under a width constraint")
{
    Document doc;

    // A deterministic measurer: 10px per character wide, 20px per wrapped line tall. It lets a
    // headless test exercise the text-measure leaf with no font resource.
    doc.SetTextMeasurer(
        [](string_view text, const Style&, optional<f32> maxWidth) -> vec2
        {
            const f32 glyph = 10.0f;
            const f32 lineHeight = 20.0f;
            const f32 full = static_cast<f32>(text.size()) * glyph;
            if (!maxWidth || *maxWidth >= full || full == 0.0f)
            {
                return vec2(full, lineHeight);
            }
            const f32 perLine = std::max(1.0f, std::floor(*maxWidth / glyph));
            const f32 lines = std::ceil(full / (perLine * glyph));
            return vec2(std::min(full, perLine * glyph), lines * lineHeight);
        });

    Style column;
    column.Direction = FlexDirection::Column;
    column.AlignItems = Align::FlexStart;
    doc.SetStyle(doc.Root(), column);

    Element& label = doc.Add(doc.Root(), ElementKind::Text);
    doc.SetText(label, "HELLOWORLD"); // 10 chars -> 100px unconstrained.

    // Wide enough: one line, 100x20.
    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(label.Layout, 0.0f, 0.0f, 100.0f, 20.0f);

    // Constrain the container to 50px: the leaf reflows to 5 chars/line -> two lines, 50x40.
    Style narrow = column;
    narrow.Width = Length::Points(50.0f);
    doc.SetStyle(doc.Root(), narrow);
    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(label.Layout, 0.0f, 0.0f, 50.0f, 40.0f);
}

TEST_CASE("gui layout: a button sizes to its label plus padding, like a text leaf")
{
    Document doc;

    // The same deterministic measurer as the text-leaf case: 10px/char, 20px/line.
    doc.SetTextMeasurer([](string_view text, const Style&, optional<f32>) -> vec2
                        { return vec2(static_cast<f32>(text.size()) * 10.0f, 20.0f); });

    Style column;
    column.Direction = FlexDirection::Column;
    column.AlignItems = Align::FlexStart;
    doc.SetStyle(doc.Root(), column);

    Element& button = doc.Add(doc.Root(), ElementKind::Button);
    doc.SetText(button, "GO"); // 2 chars -> 20px content.

    Style padded;
    padded.Padding = Insets::All(6.0f);
    doc.SetStyle(button, padded);

    // The measured label is the content box; padding grows the border box around it.
    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(button.Layout, 0.0f, 0.0f, 32.0f, 32.0f);
}

TEST_CASE("gui layout: a table widens every cell to its column's widest")
{
    Document doc;

    // 10px per character: cell widths are the text lengths, so the column maxima are known.
    doc.SetTextMeasurer([](string_view text, const Style&, optional<f32>) -> vec2
                        { return vec2(static_cast<f32>(text.size()) * 10.0f, 20.0f); });

    Style column;
    column.Direction = FlexDirection::Column;
    column.AlignItems = Align::FlexStart;
    doc.SetStyle(doc.Root(), column);

    Element& table = doc.Add(doc.Root(), ElementKind::Table);
    doc.SetStyle(table, column);

    Style rowStyle;
    rowStyle.Direction = FlexDirection::Row;
    rowStyle.AlignItems = Align::FlexStart;

    const auto addRow = [&](string_view name, string_view stat) -> std::pair<Element*, Element*>
    {
        Element& row = doc.Add(table, ElementKind::Panel);
        doc.SetStyle(row, rowStyle);
        Element& a = doc.Add(row, ElementKind::Text);
        doc.SetText(a, name);
        Element& b = doc.Add(row, ElementKind::Text);
        doc.SetText(b, stat);
        return {&a, &b};
    };

    const auto [nameA, statA] = addRow("LONGNAME", "1"); // 80px, 10px
    const auto [nameB, statB] = addRow("AB", "STATS");   // 20px, 50px

    doc.Solve(vec2(400.0f, 200.0f));

    // Column 0 widens to the longer name (80), column 1 to the longer stat (50); the second
    // column's cells start at the same x in both rows.
    CHECK(nameA->Layout.Size.x == doctest::Approx(80.0f).epsilon(Eps));
    CHECK(nameB->Layout.Size.x == doctest::Approx(80.0f).epsilon(Eps));
    CHECK(statA->Layout.Min.x == doctest::Approx(80.0f).epsilon(Eps));
    CHECK(statB->Layout.Min.x == doctest::Approx(80.0f).epsilon(Eps));
    CHECK(statA->Layout.Size.x == doctest::Approx(50.0f).epsilon(Eps));
    CHECK(statB->Layout.Size.x == doctest::Approx(50.0f).epsilon(Eps));

    // The alignment survives a clean re-solve and re-applies after a text change re-dirties.
    doc.Solve(vec2(400.0f, 200.0f));
    CHECK(statB->Layout.Min.x == doctest::Approx(80.0f).epsilon(Eps));

    doc.SetText(*nameB, "THELONGESTNAME"); // 140px: column 0 now follows row B.
    doc.Solve(vec2(400.0f, 200.0f));
    CHECK(nameA->Layout.Size.x == doctest::Approx(140.0f).epsilon(Eps));
    CHECK(statA->Layout.Min.x == doctest::Approx(140.0f).epsilon(Eps));
    CHECK(statB->Layout.Min.x == doctest::Approx(140.0f).epsilon(Eps));
}

TEST_CASE("gui layout: a table cell's margins count toward its column but stay its own")
{
    Document doc;

    doc.SetTextMeasurer([](string_view text, const Style&, optional<f32>) -> vec2
                        { return vec2(static_cast<f32>(text.size()) * 10.0f, 20.0f); });

    Style column;
    column.Direction = FlexDirection::Column;
    column.AlignItems = Align::FlexStart;
    doc.SetStyle(doc.Root(), column);

    Element& table = doc.Add(doc.Root(), ElementKind::Table);
    doc.SetStyle(table, column);

    Style rowStyle;
    rowStyle.Direction = FlexDirection::Row;
    rowStyle.AlignItems = Align::FlexStart;

    // Row A's first cell carries a 5px right margin; row B's is bare. The column width is the
    // margin-box maximum (40 + 5), so row B's bare cell widens to 45 and both second cells
    // start at x = 45.
    Element& rowA = doc.Add(table, ElementKind::Panel);
    doc.SetStyle(rowA, rowStyle);
    Element& cellA0 = doc.Add(rowA, ElementKind::Text);
    doc.SetText(cellA0, "FOUR");
    Style margined;
    margined.Margin = Insets{.Left = 0.0f, .Top = 0.0f, .Right = 5.0f, .Bottom = 0.0f};
    doc.SetStyle(cellA0, margined);
    Element& cellA1 = doc.Add(rowA, ElementKind::Text);
    doc.SetText(cellA1, "X");

    Element& rowB = doc.Add(table, ElementKind::Panel);
    doc.SetStyle(rowB, rowStyle);
    Element& cellB0 = doc.Add(rowB, ElementKind::Text);
    doc.SetText(cellB0, "AB");
    Element& cellB1 = doc.Add(rowB, ElementKind::Text);
    doc.SetText(cellB1, "Y");

    doc.Solve(vec2(400.0f, 200.0f));

    CHECK(cellA1.Layout.Min.x == doctest::Approx(45.0f).epsilon(Eps));
    CHECK(cellB1.Layout.Min.x == doctest::Approx(45.0f).epsilon(Eps));
    CHECK(cellB0.Layout.Size.x == doctest::Approx(45.0f).epsilon(Eps));
}

TEST_CASE("gui layout: dirty tracking makes a clean re-solve at the same size a no-op")
{
    Document doc;
    doc.Add(doc.Root(), ElementKind::Panel);

    CHECK(doc.IsDirty());
    doc.Solve(vec2(100.0f, 100.0f));
    CHECK_FALSE(doc.IsDirty());

    // A structural change re-dirties; a mutation-free re-solve at the same size stays clean.
    Element& added = doc.Add(doc.Root(), ElementKind::Panel);
    CHECK(doc.IsDirty());
    doc.Solve(vec2(100.0f, 100.0f));
    CHECK_FALSE(doc.IsDirty());

    doc.Remove(added);
    CHECK(doc.IsDirty());
}

TEST_CASE("gui layout: Build emits background, border, and children in tree order")
{
    Document doc;

    Style panel;
    panel.Background = vec4(0.1f, 0.1f, 0.1f, 1.0f);
    panel.BorderStyle = Border{.Width = 2.0f, .Color = vec4(1.0f)};
    doc.SetStyle(doc.Root(), panel);

    Style childStyle;
    childStyle.Background = vec4(0.5f, 0.0f, 0.0f, 1.0f);
    childStyle.Width = Length::Points(40.0f);
    childStyle.Height = Length::Points(40.0f);
    Element& child = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(child, childStyle);

    doc.Solve(vec2(100.0f, 100.0f));

    DrawList list;
    doc.Build(list);

    // Root background + root border (border shares the fill run's key) + child background: the
    // three untextured shape quads merge into one run (18 indices), in tree order.
    REQUIRE(list.GetRuns().size() == 1);
    CHECK(list.GetRuns()[0].Pipeline == GuiPipeline::Shape);
    CHECK(list.GetVertices().size() == 12);
    CHECK(list.GetIndices().size() == 18);
}

TEST_CASE("gui layout: a hidden subtree collapses to a zero box and emits nothing")
{
    Document doc;

    Style row;
    row.Direction = FlexDirection::Row;
    doc.SetStyle(doc.Root(), row);

    Style grow;
    grow.FlexGrow = 1.0f;
    grow.Background = vec4(1.0f);
    Element& a = doc.Add(doc.Root(), ElementKind::Panel);
    Element& b = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetStyle(a, grow);
    doc.SetStyle(b, grow);

    doc.SetVisible(b, false);
    doc.Solve(vec2(200.0f, 100.0f));

    // Hidden b is removed from flow, so a takes the whole width; b is a zero box.
    CheckRect(a.Layout, 0.0f, 0.0f, 200.0f, 100.0f);
    CHECK(b.Layout.Size.x == doctest::Approx(0.0f));

    DrawList list;
    doc.Build(list);
    // Only a's background is emitted (one quad); b is skipped.
    REQUIRE(list.GetRuns().size() == 1);
    CHECK(list.GetIndices().size() == 6);
}
