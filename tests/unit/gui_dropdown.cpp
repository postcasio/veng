// The Gui Dropdown widget — headless behavior cases over the plan-07 dispatch paths. Device-free:
// trees are built directly, geometry pinned where the pointer path needs it, and a reflected in-test
// view-model supplies the data-bound options. No ICD and no font is needed — a dropdown's logic is
// pure CPU. Covers: the value surface is the selected index (not a string), a keyboard round-trip
// (open on confirm, arrow-move, commit on Enter, fires onChange once), Escape leaving the value
// unchanged and firing nothing, the pointer round-trip (click to open, click an option to commit),
// data-bound options rendering one item per entry with matching labels and re-populating on change,
// and an inline dropdown resolving its selected label with no bound context.

#include <doctest/doctest.h>

#include <span>

#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    // One selectable option the dropdown's popup List repeats its item template over.
    struct Option
    {
        string Label;
    };

    // The binding data object: the option array the dropdown is data-bound to, plus the selected
    // index a driver maps to/from an option id (the widget itself speaks indices).
    struct DropdownModel
    {
        vector<Option> Options;
        i32 Selected = 0;
    };

    // Pins an element's document-space rect directly, bypassing the flex solve.
    void PlaceAt(Element& element, vec2 min, vec2 size)
    {
        element.Layout = Rect{.Min = min, .Size = size};
    }

    // Builds a data-bound dropdown: an anchor with an `items`/`value`/`onChange` triple and a single
    // Text option template bound to `Label`. Returns the anchor.
    Element& BuildDataBoundDropdown(Document& doc, Element& parent)
    {
        Element& dropdown = doc.Add(parent, ElementKind::Dropdown);
        dropdown.Bindings["items"] = "Options";
        dropdown.Bindings["value"] = "Selected";
        dropdown.Bindings["onChange"] = "changed";
        Element& item = doc.Add(dropdown, ElementKind::Text);
        item.Bindings["text"] = "Label";
        doc.InitWidget(dropdown);
        return dropdown;
    }

    // The popup List of the currently-open dropdown popup, or nullptr when none is open.
    Element* OpenOptionList(Document& doc)
    {
        const PopupId top = doc.GetTopPopup();
        Element* const root = doc.GetPopupRoot(top);
        return root != nullptr && !root->Children.empty() ? root->Children.front() : nullptr;
    }
}

VE_REFLECT(::Option, 0x5110000000000001ULL)
VE_FIELD(Label)
VE_REFLECT_END();

VE_REFLECT(::DropdownModel, 0x5110000000000002ULL)
VE_ARRAY_FIELD(Options)
VE_FIELD(Selected)
VE_REFLECT_END();

TEST_CASE("gui dropdown: the value surface is the selected index and reflects a one-way binding")
{
    Document doc;
    const Element& dropdown = BuildDataBoundDropdown(doc, doc.Root());
    CHECK(dropdown.Focusable);

    TypeRegistry registry;
    registry.Register<DropdownModel>();
    DropdownModel model;
    model.Options = {Option{.Label = "Low"}, Option{.Label = "Medium"}, Option{.Label = "High"}};
    model.Selected = 1;

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    // The bound index reaches the widget value verbatim, and the selected option's label reaches
    // the anchor — the index, not a string, is the whole value surface.
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(1.0f));
    CHECK(dropdown.Text == "Medium");

    // A model move re-reads the one-way binding and re-labels the anchor.
    model.Selected = 2;
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(2.0f));
    CHECK(dropdown.Text == "High");
}

TEST_CASE("gui dropdown: a keyboard round-trip opens, arrow-moves, and commits on Enter")
{
    Document doc;
    doc.SetInteractive(true);
    Element& dropdown = BuildDataBoundDropdown(doc, doc.Root());

    TypeRegistry registry;
    registry.Register<DropdownModel>();
    DropdownModel model;
    model.Options = {Option{.Label = "Low"}, Option{.Label = "Medium"}, Option{.Label = "High"}};
    model.Selected = 0;

    int changes = 0;
    f32 lastIndex = -1.0f;
    BindingContext context;
    context.SetData(model);
    context.SetHandler("changed",
                       [&](Element& e)
                       {
                           ++changes;
                           lastIndex = doc.GetWidgetValue(e);
                       });
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    doc.SetFocus(&dropdown);

    // Confirm on the anchor opens the option popup with one item per entry, labels matching.
    CHECK(doc.Navigate(NavAction::Confirm));
    REQUIRE(doc.GetPopupCount() == 1);
    Element* const list = OpenOptionList(doc);
    REQUIRE(list != nullptr);
    REQUIRE(doc.GetItemCount(*list) == 3);
    CHECK(doc.GetItemElement(*list, 0)->Text == "Low");
    CHECK(doc.GetItemElement(*list, 2)->Text == "High");

    // Pin the options vertically so directional focus navigation has geometry to move through (there
    // is no font, so a Solve would size the text items to zero).
    for (u32 i = 0; i < 3; ++i)
    {
        PlaceAt(*doc.GetItemElement(*list, i), {0, static_cast<f32>(i) * 20.0f}, {200, 20});
    }

    // The current index is the list's live selection, and focus sits on it so the arrow moves from
    // there. An arrow move changes the popup's selection but does not commit — no onChange yet.
    CHECK(doc.Navigate(NavAction::MoveDown));
    CHECK(doc.Navigate(NavAction::MoveDown));
    CHECK(changes == 0);
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(0.0f));

    // Enter commits the focused option: the index lands in the value, onChange fires once with it,
    // the label reaches the anchor, and the popup closes.
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(doc.GetPopupCount() == 0);
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(2.0f));
    CHECK(dropdown.Text == "High");
    CHECK(changes == 1);
    CHECK(lastIndex == doctest::Approx(2.0f));
}

TEST_CASE("gui dropdown: Escape closes the popup with the value unchanged and fires nothing")
{
    Document doc;
    doc.SetInteractive(true);
    Element& dropdown = BuildDataBoundDropdown(doc, doc.Root());

    TypeRegistry registry;
    registry.Register<DropdownModel>();
    DropdownModel model;
    model.Options = {Option{.Label = "Low"}, Option{.Label = "Medium"}, Option{.Label = "High"}};
    model.Selected = 1;

    int changes = 0;
    BindingContext context;
    context.SetData(model);
    context.SetHandler("changed", [&](Element&) { ++changes; });
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    doc.SetFocus(&dropdown);
    CHECK(doc.Navigate(NavAction::Confirm));
    REQUIRE(doc.GetPopupCount() == 1);
    Element* const list = OpenOptionList(doc);
    REQUIRE(list != nullptr);
    for (u32 i = 0; i < 3; ++i)
    {
        PlaceAt(*doc.GetItemElement(*list, i), {0, static_cast<f32>(i) * 20.0f}, {200, 20});
    }

    // Move the popup's highlight, then cancel: the anchor value never moved and no change fired.
    CHECK(doc.Navigate(NavAction::MoveDown));
    CHECK(doc.Navigate(NavAction::Cancel));
    CHECK(doc.GetPopupCount() == 0);
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(1.0f));
    CHECK(dropdown.Text == "Medium");
    CHECK(changes == 0);
}

TEST_CASE("gui dropdown: a click opens the popup and a click on an option commits it")
{
    Document doc;
    doc.SetInteractive(true);
    PlaceAt(doc.Root(), {0, 0}, {200, 300});
    Element& dropdown = BuildDataBoundDropdown(doc, doc.Root());
    PlaceAt(dropdown, {0, 0}, {200, 30});

    TypeRegistry registry;
    registry.Register<DropdownModel>();
    DropdownModel model;
    model.Options = {Option{.Label = "Low"}, Option{.Label = "Medium"}, Option{.Label = "High"}};
    model.Selected = 0;

    int changes = 0;
    BindingContext context;
    context.SetData(model);
    context.SetHandler("changed", [&](Element&) { ++changes; });
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    // A click on the anchor opens the popup.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(20, 15)};
    doc.DispatchPointer(down);
    PointerEvent up{.Kind = PointerEventKind::Up, .Position = vec2(20, 15)};
    doc.DispatchPointer(up);
    REQUIRE(doc.GetPopupCount() == 1);

    Element* const list = OpenOptionList(doc);
    REQUIRE(list != nullptr);
    REQUIRE(doc.GetItemCount(*list) == 3);

    // Pin the popup geometry the hit-test reads (there is no font, so the solve would size the text
    // items to zero), then click the third option.
    PlaceAt(*doc.GetPopupRoot(doc.GetTopPopup()), {0, 30}, {200, 90});
    PlaceAt(*list, {0, 30}, {200, 90});
    for (u32 i = 0; i < 3; ++i)
    {
        PlaceAt(*doc.GetItemElement(*list, i), {0, 30.0f + static_cast<f32>(i) * 30.0f}, {200, 30});
    }

    PointerEvent itemDown{.Kind = PointerEventKind::Down, .Position = vec2(40, 105)};
    doc.DispatchPointer(itemDown);
    PointerEvent itemUp{.Kind = PointerEventKind::Up, .Position = vec2(40, 105)};
    doc.DispatchPointer(itemUp);

    CHECK(doc.GetPopupCount() == 0);
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(2.0f));
    CHECK(dropdown.Text == "High");
    CHECK(changes == 1);
}

TEST_CASE("gui dropdown: data-bound options re-populate the open popup when the array grows")
{
    Document doc;
    doc.SetInteractive(true);
    Element& dropdown = BuildDataBoundDropdown(doc, doc.Root());

    TypeRegistry registry;
    registry.Register<DropdownModel>();
    DropdownModel model;
    model.Options = {Option{.Label = "Low"}, Option{.Label = "High"}};
    model.Selected = 0;

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    doc.SetFocus(&dropdown);
    CHECK(doc.Navigate(NavAction::Confirm));
    Element* const list = OpenOptionList(doc);
    REQUIRE(list != nullptr);
    CHECK(doc.GetItemCount(*list) == 2);

    // Growing the bound array re-populates the still-open popup list on the next drive.
    model.Options.push_back(Option{.Label = "Ultra"});
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(doc.GetItemCount(*list) == 3);
    CHECK(doc.GetItemElement(*list, 2)->Text == "Ultra");
}

TEST_CASE("gui dropdown: an inline dropdown resolves its options with no bound context")
{
    Document doc;
    doc.SetInteractive(true);

    Element& dropdown = doc.Add(doc.Root(), ElementKind::Dropdown);
    dropdown.Bindings["value"] = "1"; // a literal starting index, not a binding path
    Element& low = doc.Add(dropdown, ElementKind::Text);
    doc.SetText(low, "Low");
    Element& medium = doc.Add(dropdown, ElementKind::Text);
    doc.SetText(medium, "Medium");
    Element& high = doc.Add(dropdown, ElementKind::Text);
    doc.SetText(high, "High");
    doc.InitWidget(dropdown);
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(1.0f));

    // Opening the dropdown (its only context-free capture trigger outside Instantiate) lifts the
    // authored children into the option template and builds the popup List from them — no binding
    // context involved — leaving only the widget-owned arrow part on the anchor.
    doc.SetFocus(&dropdown);
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(dropdown.Text == "Medium");
    CHECK(dropdown.Children.size() == 1);
    Element* const list = OpenOptionList(doc);
    REQUIRE(list != nullptr);
    REQUIRE(doc.GetItemCount(*list) == 3);
    CHECK(doc.GetItemElement(*list, 0)->Text == "Low");
    CHECK(doc.GetItemElement(*list, 2)->Text == "High");
    for (u32 i = 0; i < 3; ++i)
    {
        PlaceAt(*doc.GetItemElement(*list, i), {0, static_cast<f32>(i) * 20.0f}, {200, 20});
    }

    // Enter on the highlighted next option commits the index onto the anchor and closes the popup.
    CHECK(doc.Navigate(NavAction::MoveDown));
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(doc.GetPopupCount() == 0);
    CHECK(doc.GetWidgetValue(dropdown) == doctest::Approx(2.0f));
    CHECK(dropdown.Text == "High");
}
