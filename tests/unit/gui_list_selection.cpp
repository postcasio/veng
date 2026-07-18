// Item-host selection — headless cases over the pointer and navigation dispatch paths. Device-free:
// trees are built directly, geometry is pinned, and a reflected in-test view-model supplies the bound
// array. Covers the four selection modes and their chords (Single replaces, Multiple toggles,
// Extended replaces / Control-toggles / Shift-ranges), the Selected state bit projected onto whole
// item subtrees, selection surviving a re-sync, the item-index seam a control inside an item reads,
// and the programmatic setters staying one-way.

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
    // One row the list repeats its item template against.
    struct Track
    {
        string Title;
        i32 Length = 0;
    };

    // The binding data object: the array the item host repeats over.
    struct Playlist
    {
        vector<Track> Tracks;
    };

    void PlaceAt(Element& element, vec2 min, vec2 size)
    {
        element.Layout = Rect{.Min = min, .Size = size};
    }

    // Builds a List over `count` tracks whose item template is a Panel wrapping a Text and a
    // Button — the point being that an item is an arbitrary subtree, not a text leaf.
    struct Fixture
    {
        Document Doc;
        TypeRegistry Registry;
        BindingContext Context;
        Playlist Model;
        Element* List = nullptr;

        explicit Fixture(SelectionMode mode, usize count = 4)
        {
            Doc.SetInteractive(true);
            PlaceAt(Doc.Root(), {0, 0}, {200, 400});

            List = &Doc.Add(Doc.Root(), ElementKind::List);
            List->Bindings["items"] = "Tracks";
            List->Bindings["selection"] = mode == SelectionMode::Single     ? "single"
                                          : mode == SelectionMode::Multiple ? "multiple"
                                          : mode == SelectionMode::Extended ? "extended"
                                                                            : "none";
            Doc.InitWidget(*List);

            Element& row = Doc.Add(*List, ElementKind::Panel);
            Element& title = Doc.Add(row, ElementKind::Text);
            title.Bindings["text"] = "Title";
            Element& remove = Doc.Add(row, ElementKind::Button);
            remove.Bindings["onClick"] = "remove";

            Registry.Register<Playlist>();
            for (usize i = 0; i < count; ++i)
            {
                Model.Tracks.push_back(Track{.Title = fmt::format("track {}", i), .Length = 0});
            }
            Context.SetData(Model);
            Doc.BindContext(&Context, &Registry);
            Doc.UpdateBindings();

            // Pin each instantiated row (and its children) to a 40px-tall band so the pointer and
            // directional-navigation cases have real geometry without running a flex solve.
            for (usize i = 0; i < List->Children.size(); ++i)
            {
                Element& item = *List->Children[i];
                PlaceAt(item, {0, static_cast<f32>(i) * 40.0f}, {200, 40});
                for (Element* child : item.Children)
                {
                    PlaceAt(*child, item.Layout.Min, {100, 40});
                }
            }
        }

        // A completed primary click at a point, with the given chord held.
        void ClickAt(vec2 point, InputModifiers modifiers = InputModifiers::None)
        {
            PointerEvent down{
                .Kind = PointerEventKind::Down, .Position = point, .Modifiers = modifiers};
            Doc.DispatchPointer(down);
            PointerEvent up{
                .Kind = PointerEventKind::Up, .Position = point, .Modifiers = modifiers};
            Doc.DispatchPointer(up);
        }

        // The center of row `index`, inside the Text child rather than the row root — a click lands
        // on a leaf deep in the item template, which is the case selection has to resolve.
        [[nodiscard]] vec2 RowPoint(usize index) const
        {
            return vec2(50.0f, static_cast<f32>(index) * 40.0f + 20.0f);
        }

        [[nodiscard]] vector<u32> Selection() const
        {
            const std::span<const u32> items = Doc.GetSelectedItems(*List);
            return vector<u32>(items.begin(), items.end());
        }
    };
}

VE_REFLECT(::Track, 0x51A9000000000001ULL)
VE_FIELD(Title)
VE_FIELD(Length)
VE_REFLECT_END();

VE_REFLECT(::Playlist, 0x51A9000000000002ULL)
VE_ARRAY_FIELD(Tracks)
VE_REFLECT_END();

TEST_CASE("gui selection: an unselectable List selects nothing and keeps its items unfocusable")
{
    Fixture fixture(SelectionMode::None);

    CHECK(fixture.Doc.GetSelectionMode(*fixture.List) == SelectionMode::None);
    fixture.ClickAt(fixture.RowPoint(1));
    CHECK(fixture.Selection().empty());
    CHECK(!fixture.List->Children[1]->Focusable);
}

TEST_CASE("gui selection: a Single list replaces its one selection on each click")
{
    Fixture fixture(SelectionMode::Single);

    fixture.ClickAt(fixture.RowPoint(1));
    CHECK(fixture.Selection() == vector<u32>{1});
    CHECK((fixture.List->Children[1]->State & ElementState::Selected) == ElementState::Selected);

    // A second click moves the selection rather than growing it, and the old item drops its bit.
    fixture.ClickAt(fixture.RowPoint(3));
    CHECK(fixture.Selection() == vector<u32>{3});
    CHECK((fixture.List->Children[1]->State & ElementState::Selected) == ElementState::None);
    CHECK((fixture.List->Children[3]->State & ElementState::Selected) == ElementState::Selected);
}

TEST_CASE("gui selection: a Multiple list toggles one item per click, no chord needed")
{
    Fixture fixture(SelectionMode::Multiple);

    fixture.ClickAt(fixture.RowPoint(0));
    fixture.ClickAt(fixture.RowPoint(2));
    CHECK(fixture.Selection() == vector<u32>{0, 2});

    // Clicking a selected item deselects it — the modifier-free multi-select a gamepad needs.
    fixture.ClickAt(fixture.RowPoint(0));
    CHECK(fixture.Selection() == vector<u32>{2});
}

TEST_CASE("gui selection: an Extended list replaces, Control-toggles, and Shift-ranges")
{
    Fixture fixture(SelectionMode::Extended);

    // An unmodified click replaces the selection and sets the anchor.
    fixture.ClickAt(fixture.RowPoint(1));
    CHECK(fixture.Selection() == vector<u32>{1});

    // Control adds without clearing, and moves the anchor to the toggled item.
    fixture.ClickAt(fixture.RowPoint(3), InputModifiers::Control);
    CHECK(fixture.Selection() == vector<u32>{1, 3});

    // Shift extends a contiguous range from the standing anchor (3) back to the clicked item (0),
    // replacing the previous selection.
    fixture.ClickAt(fixture.RowPoint(0), InputModifiers::Shift);
    CHECK(fixture.Selection() == vector<u32>{0, 1, 2, 3});

    // The anchor does not travel with a Shift-click, so re-extending grows from the same origin.
    fixture.ClickAt(fixture.RowPoint(2), InputModifiers::Shift);
    CHECK(fixture.Selection() == vector<u32>{2, 3});

    // Meta is the same toggle as Control, for the platform whose convention is the command key.
    fixture.ClickAt(fixture.RowPoint(0), InputModifiers::Meta);
    CHECK(fixture.Selection() == vector<u32>{0, 2, 3});
}

TEST_CASE("gui selection: onSelectionChanged fires on a user act and not on a programmatic write")
{
    Fixture fixture(SelectionMode::Multiple);
    int changes = 0;
    fixture.List->Bindings["onSelectionChanged"] = "changed";
    fixture.Context.SetHandler("changed", [&](Element&) { ++changes; });

    fixture.ClickAt(fixture.RowPoint(1));
    CHECK(changes == 1);

    // Re-clicking the same item toggles it off — still a change.
    fixture.ClickAt(fixture.RowPoint(1));
    CHECK(changes == 2);

    // The programmatic setters are one-way, like a `{value}` binding: a game writing its model's
    // selection back each frame never re-enters its own handler.
    const vector<u32> wanted{0, 2};
    fixture.Doc.SetSelectedItems(*fixture.List, wanted);
    CHECK(fixture.Selection() == wanted);
    fixture.Doc.SelectItem(*fixture.List, 3, true);
    fixture.Doc.ClearSelection(*fixture.List);
    CHECK(fixture.Selection().empty());
    CHECK(changes == 2);
}

TEST_CASE("gui selection: the Selected bit covers the whole item subtree's root, deep hit or not")
{
    Fixture fixture(SelectionMode::Single);

    // The click lands on the Text leaf inside the row, two levels below the item root; the state
    // bit still resolves onto the item root the `:selected` variant styles.
    fixture.ClickAt(fixture.RowPoint(2));
    CHECK((fixture.List->Children[2]->State & ElementState::Selected) == ElementState::Selected);
    CHECK(fixture.Doc.GetItemElement(*fixture.List, 2) == fixture.List->Children[2]);
}

TEST_CASE("gui selection: a control inside an item reports which item it belongs to")
{
    Fixture fixture(SelectionMode::None);

    // The seam a per-row Button's onClick needs: an item may hold any elements, so a handler that
    // receives the Button has to be able to ask which row raised it.
    Element& row = *fixture.List->Children[2];
    Element& button = *row.Children[1];
    CHECK(fixture.Doc.GetItemIndex(button) == optional<u32>(2));
    CHECK(fixture.Doc.GetItemIndex(row) == optional<u32>(2));
    CHECK(fixture.Doc.GetItemHost(button) == fixture.List);
    CHECK(fixture.Doc.GetItemCount(*fixture.List) == 4);

    // An element outside any item host belongs to no item.
    CHECK(!fixture.Doc.GetItemIndex(fixture.Doc.Root()).has_value());
}

TEST_CASE("gui selection: a selection survives a re-sync and drops the items a shrink removed")
{
    Fixture fixture(SelectionMode::Multiple);

    fixture.ClickAt(fixture.RowPoint(1));
    fixture.ClickAt(fixture.RowPoint(3));
    CHECK(fixture.Selection() == vector<u32>{1, 3});

    // Growing the array rebuilds no existing item, and the selection still names the same rows.
    fixture.Model.Tracks.push_back(Track{.Title = "track 4", .Length = 0});
    fixture.Context.Invalidate();
    fixture.Doc.UpdateBindings();
    CHECK(fixture.Selection() == vector<u32>{1, 3});

    // Shrinking past a selected index drops it — the selection indexes the array, so it cannot
    // name a row that no longer exists.
    fixture.Model.Tracks.resize(2);
    fixture.Context.Invalidate();
    fixture.Doc.UpdateBindings();
    CHECK(fixture.Selection() == vector<u32>{1});
    CHECK((fixture.List->Children[1]->State & ElementState::Selected) == ElementState::Selected);
}

TEST_CASE("gui selection: arrowing a Single list carries the selection with focus")
{
    Fixture fixture(SelectionMode::Single);

    fixture.ClickAt(fixture.RowPoint(0));
    CHECK(fixture.Selection() == vector<u32>{0});

    // Single-select follows focus, so a directional move both moves the focus stop and re-selects.
    CHECK(fixture.Doc.Navigate(NavAction::MoveDown));
    CHECK(fixture.Doc.GetFocused() == fixture.List->Children[1]);
    CHECK(fixture.Selection() == vector<u32>{1});
}

TEST_CASE("gui selection: arrowing an Extended list ranges on Shift and detaches on Control")
{
    Fixture fixture(SelectionMode::Extended);

    fixture.ClickAt(fixture.RowPoint(0));
    CHECK(fixture.Selection() == vector<u32>{0});

    // Shift-arrow grows the range from the anchor the click set.
    CHECK(fixture.Doc.Navigate(NavAction::MoveDown, InputModifiers::Shift));
    CHECK(fixture.Selection() == vector<u32>{0, 1});
    CHECK(fixture.Doc.Navigate(NavAction::MoveDown, InputModifiers::Shift));
    CHECK(fixture.Selection() == vector<u32>{0, 1, 2});

    // Control-arrow travels without disturbing the selection, so a user can reach an item and
    // then toggle it with Confirm.
    CHECK(fixture.Doc.Navigate(NavAction::MoveDown, InputModifiers::Control));
    CHECK(fixture.Doc.GetFocused() == fixture.List->Children[3]);
    CHECK(fixture.Selection() == vector<u32>{0, 1, 2});
    CHECK(fixture.Doc.Navigate(NavAction::Confirm, InputModifiers::Control));
    CHECK(fixture.Selection() == vector<u32>{0, 1, 2, 3});
}

TEST_CASE("gui selection: a gamepad Confirm toggles a Multiple list's focused item")
{
    Fixture fixture(SelectionMode::Multiple);

    // No chord is available on a gamepad, so the bare Confirm is the toggle.
    fixture.Doc.SetFocus(fixture.List->Children[2]);
    CHECK(fixture.Doc.Navigate(NavAction::Confirm));
    CHECK(fixture.Selection() == vector<u32>{2});
    CHECK(fixture.Doc.Navigate(NavAction::Confirm));
    CHECK(fixture.Selection().empty());
}

TEST_CASE("gui selection: narrowing the mode narrows the standing selection")
{
    Fixture fixture(SelectionMode::Multiple);

    fixture.ClickAt(fixture.RowPoint(1));
    fixture.ClickAt(fixture.RowPoint(3));
    CHECK(fixture.Selection() == vector<u32>{1, 3});

    // Single keeps one item; None clears the selection and demotes the items' focus stops.
    fixture.Doc.SetSelectionMode(*fixture.List, SelectionMode::Single);
    CHECK(fixture.Selection() == vector<u32>{1});
    fixture.Doc.SetSelectionMode(*fixture.List, SelectionMode::None);
    CHECK(fixture.Selection().empty());
    CHECK(!fixture.List->Children[1]->Focusable);
}

TEST_CASE("gui selection: a selectable Table over a bound array selects whole rows")
{
    // A Table repeating an array is an item host exactly as a List is, so row selection needs no
    // Table-specific path.
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 400});

    Element& table = doc.Add(doc.Root(), ElementKind::Table);
    table.Bindings["items"] = "Tracks";
    table.Bindings["selection"] = "multiple";
    doc.InitWidget(table);
    Element& rowTemplate = doc.Add(table, ElementKind::Panel);
    doc.Add(rowTemplate, ElementKind::Text).Bindings["text"] = "Title";

    TypeRegistry registry;
    registry.Register<Playlist>();
    Playlist model;
    model.Tracks.push_back(Track{.Title = "a", .Length = 0});
    model.Tracks.push_back(Track{.Title = "b", .Length = 0});

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    REQUIRE(table.Children.size() == 2);
    doc.SelectItem(table, 1, true);
    CHECK(doc.IsItemSelected(table, 1));
    CHECK((table.Children[1]->State & ElementState::Selected) == ElementState::Selected);
    CHECK((table.Children[0]->State & ElementState::Selected) == ElementState::None);
}
