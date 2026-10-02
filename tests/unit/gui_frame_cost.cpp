// A document's frame costs what changed in it. Device-free: text sizes through an injected
// measurer. Each case pins one property of the scoped pipeline — a write of a held value marks
// nothing, a change re-resolves and re-pushes only the element it reached, an absolute move lands
// with no solve exactly where a solve would put it, and a retained drive re-emits only the marked
// subtrees while emitting exactly what a fresh build does.

#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    constexpr vec2 Available{400.0f, 300.0f};

    // 10px per character, 20px per line.
    vec2 Measure(string_view text, const Style&, optional<f32>)
    {
        return vec2(static_cast<f32>(text.size()) * 10.0f, 20.0f);
    }

    Style Boxed(const vec4 background)
    {
        Style style;
        style.Background = background;
        style.Padding = Insets::All(4.0f);
        style.BorderStyle = Border{.Width = 2.0f, .Color = vec4(1.0f)};
        return style;
    }

    // A root holding three groups of three labels: the tree every case below changes one part of.
    struct Tree
    {
        Document Doc;
        vector<Element*> Groups;
        vector<Element*> Labels;

        Tree()
        {
            Doc.SetTextMeasurer(Measure);
            for (u32 g = 0; g < 3; ++g)
            {
                Element& group = Doc.Add(Doc.Root(), ElementKind::Panel);
                Doc.SetStyle(group, Boxed(vec4(0.1f * static_cast<f32>(g + 1), 0.2f, 0.3f, 1.0f)));
                Groups.push_back(&group);
                for (u32 l = 0; l < 3; ++l)
                {
                    Element& label = Doc.Add(group, ElementKind::Text);
                    Doc.SetText(label, "LABEL");
                    Doc.SetStyle(label, Boxed(vec4(0.5f, 0.5f, 0.1f * static_cast<f32>(l), 1.0f)));
                    Labels.push_back(&label);
                }
            }
        }

        void Frame(DrawList& out)
        {
            out.Clear();
            Doc.Drive(Available, 0.016f, out);
        }
    };

    void CheckSameList(const DrawList& a, const DrawList& b)
    {
        REQUIRE(a.GetVertices().size() == b.GetVertices().size());
        CHECK(std::memcmp(a.GetVertices().data(), b.GetVertices().data(),
                          a.GetVertices().size() * sizeof(GuiVertex)) == 0);
        CHECK(a.GetIndices() == b.GetIndices());
        REQUIRE(a.GetRuns().size() == b.GetRuns().size());
        for (usize i = 0; i < a.GetRuns().size(); ++i)
        {
            const DrawRun& x = a.GetRuns()[i];
            const DrawRun& y = b.GetRuns()[i];
            CHECK(x.Pipeline == y.Pipeline);
            CHECK(x.FirstIndex == y.FirstIndex);
            CHECK(x.IndexCount == y.IndexCount);
            CHECK(x.HasClip == y.HasClip);
            CHECK(x.Clip.Min == y.Clip.Min);
            CHECK(x.Clip.Size == y.Clip.Size);
        }
    }

    void CheckSameRect(const Rect& a, const Rect& b)
    {
        CHECK(a.Min == b.Min);
        CHECK(a.Size == b.Size);
    }
}

TEST_CASE("gui frame cost: a write of the value an element already holds marks nothing")
{
    Tree tree;
    DrawList out;
    tree.Frame(out);
    const DocumentStats before = tree.Doc.GetStats();

    // Every setter a per-frame driver calls, each handed what the element already holds.
    Element& label = *tree.Labels[4];
    Element& group = *tree.Groups[1];
    tree.Doc.SetVisible(label, true);
    tree.Doc.SetText(label, "LABEL");
    tree.Doc.SetStyle(group, group.BaseStyle);
    tree.Doc.SetBackground(group, group.BaseStyle.Background);
    tree.Doc.SetTextColor(label, label.BaseStyle.TextColor);
    tree.Doc.SetOpacity(label, 1.0f);
    tree.Doc.SetRotation(label, 0.0f);
    CHECK_FALSE(tree.Doc.IsDirty());
    CHECK_FALSE(tree.Doc.IsPaintDirty());

    tree.Frame(out);
    const DocumentStats& after = tree.Doc.GetStats();
    CHECK(after.StyleResolves == before.StyleResolves);
    CHECK(after.Solves == before.Solves);
    CHECK(after.ElementsEmitted == before.ElementsEmitted);
    // The whole document is one unchanged subtree, copied as one.
    CHECK(after.SubtreesReused == before.SubtreesReused + 1);
}

TEST_CASE("gui frame cost: a change re-resolves and re-lays out only the elements it reached")
{
    Tree tree;
    DrawList out;
    tree.Frame(out);

    // A layout write on one label: one resolve, one push, the rest untouched.
    DocumentStats before = tree.Doc.GetStats();
    Style wider = tree.Labels[0]->BaseStyle;
    wider.Width = Length::Points(120.0f);
    tree.Doc.SetStyle(*tree.Labels[0], wider);
    tree.Frame(out);
    CHECK(tree.Doc.GetStats().StyleResolves == before.StyleResolves + 1);
    CHECK(tree.Doc.GetStats().StylePushes == before.StylePushes + 1);
    CHECK(tree.Doc.GetStats().Solves == before.Solves + 1);
    CHECK(tree.Labels[0]->Layout.Size.x == doctest::Approx(120.0f));

    // An inheritable state bit on a group reaches that group's subtree and nothing else.
    before = tree.Doc.GetStats();
    tree.Doc.SetState(*tree.Groups[2], ElementState::Selected);
    tree.Frame(out);
    CHECK(tree.Doc.GetStats().StyleResolves == before.StyleResolves + 4);

    // A paint-only write resolves nothing and solves nothing.
    before = tree.Doc.GetStats();
    tree.Doc.SetBackground(*tree.Labels[8], vec4(1.0f, 0.0f, 0.0f, 1.0f));
    tree.Frame(out);
    CHECK(tree.Doc.GetStats().StyleResolves == before.StyleResolves);
    CHECK(tree.Doc.GetStats().Solves == before.Solves);
}

TEST_CASE("gui frame cost: an absolute move re-solves nothing and lands where a solve would")
{
    // A framed parent holding a padded, bordered, margined marker that carries a label.
    const auto build = [](Document& doc, const vec2 pin)
    {
        doc.SetTextMeasurer(Measure);
        Element& frame = doc.Add(doc.Root(), ElementKind::Panel);
        doc.SetStyle(frame, Boxed(vec4(0.2f)));
        Element& marker = doc.Add(frame, ElementKind::Panel);
        Style markerStyle = Boxed(vec4(0.6f));
        markerStyle.Margin = Insets::All(2.0f);
        doc.SetStyle(marker, markerStyle);
        doc.SetPinnedPosition(marker, pin);
        Element& label = doc.Add(marker, ElementKind::Text);
        doc.SetText(label, "MARK");
        doc.Update(0.0f);
        doc.Solve(Available);
        return std::pair<Element*, Element*>{&marker, &label};
    };

    // Fractional insets, as a marker tracking a projected point writes them.
    constexpr vec2 To{137.4f, 52.6f};

    Document moved;
    const auto [marker, label] = build(moved, vec2(10.0f, 10.0f));
    const DocumentStats before = moved.GetStats();
    moved.SetPinnedPosition(*marker, To);
    CHECK_FALSE(moved.IsDirty());
    moved.Update(0.0f);
    moved.Solve(Available);
    CHECK(moved.GetStats().Solves == before.Solves);
    CHECK(moved.GetStats().DirectMoves == before.DirectMoves + 1);

    Document solved;
    const auto [solvedMarker, solvedLabel] = build(solved, To);
    CheckSameRect(marker->Layout, solvedMarker->Layout);
    CheckSameRect(label->Layout, solvedLabel->Layout);

    // A solve run later for another reason keeps the marker where the move put it.
    Element& other = moved.Add(moved.Root(), ElementKind::Text);
    moved.SetText(other, "ELSEWHERE");
    moved.Update(0.0f);
    moved.Solve(Available);
    CheckSameRect(marker->Layout, solvedMarker->Layout);
    CheckSameRect(label->Layout, solvedLabel->Layout);
}

TEST_CASE("gui frame cost: a drive re-emits only what changed, and emits what a fresh build does")
{
    Tree tree;
    // A clip and a rotation in the tree, so a copied range must carry both exactly.
    Style clipped = tree.Groups[0]->BaseStyle;
    clipped.OverflowX = Overflow::Hidden;
    clipped.OverflowY = Overflow::Hidden;
    tree.Doc.SetStyle(*tree.Groups[0], clipped);
    tree.Doc.SetRotation(*tree.Groups[1], 10.0f);

    DrawList out;
    tree.Frame(out);

    const auto checkAgainstFresh = [&]
    {
        DrawList fresh;
        tree.Doc.Build(fresh);
        CheckSameList(out, fresh);
    };

    // One label recoloured: it and its two ancestors are emitted; the two other groups and the
    // label's two siblings are copied.
    DocumentStats before = tree.Doc.GetStats();
    tree.Doc.SetBackground(*tree.Labels[1], vec4(0.9f, 0.1f, 0.1f, 1.0f));
    tree.Frame(out);
    CHECK(tree.Doc.GetStats().ElementsEmitted == before.ElementsEmitted + 3);
    CHECK(tree.Doc.GetStats().SubtreesReused == before.SubtreesReused + 4);
    checkAgainstFresh();

    // A group turned: its children sit under a new transform, so they are emitted too.
    before = tree.Doc.GetStats();
    tree.Doc.SetRotation(*tree.Groups[1], -15.0f);
    tree.Frame(out);
    CHECK(tree.Doc.GetStats().ElementsEmitted == before.ElementsEmitted + 5);
    checkAgainstFresh();

    // A label removed and one hidden: the survivors are copied around the gap.
    tree.Doc.Remove(*tree.Labels[7]);
    tree.Doc.SetVisible(*tree.Labels[3], false);
    tree.Frame(out);
    checkAgainstFresh();

    // Nothing changed: the whole document is copied as one range.
    before = tree.Doc.GetStats();
    tree.Frame(out);
    CHECK(tree.Doc.GetStats().ElementsEmitted == before.ElementsEmitted);
    CHECK(tree.Doc.GetStats().SubtreesReused == before.SubtreesReused + 1);
    checkAgainstFresh();
}
