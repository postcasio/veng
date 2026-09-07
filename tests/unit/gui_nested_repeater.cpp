// Nested data-bound repeaters — a Dropdown, a Slider, and a plain `{field}` inside a List's item
// template, each resolving against its own enclosing row's array element rather than the component
// or document context. Device-free: trees are built directly and a reflected in-test view-model
// supplies the per-row data — no ICD and no font is needed, since nested binding is pure CPU.
// Covers: per-row dropdowns reporting the right option count and selected label with rows differing
// (row independence), a nested Slider reading per-row min/max/step/value (clamped and snapped), a
// plain `{field}` in a row still reading the row and a top-level dropdown still reading the root
// context (regression), and opening a nested dropdown's popup filling from that row's own array.

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
    // One selectable option a dropdown repeats its label template over.
    struct NestOption
    {
        string Label;
    };

    // One row of the outer list: its own option array and selected index, its own slider bounds and
    // value, and a plain string field a `{Name}` binding reads.
    struct NestRow
    {
        vector<NestOption> Options;
        i32 Value = 0;
        f32 Min = 0.0f;
        f32 Max = 1.0f;
        f32 Step = 0.0f;
        f32 SliderValue = 0.0f;
        string Name;
    };

    // The document's data object: the rows the outer list repeats, plus a top-level dropdown's own
    // option array and index that must keep resolving against this root rather than any row.
    struct NestModel
    {
        vector<NestRow> Rows;
        vector<NestOption> TopOptions;
        i32 TopSelected = 0;
    };

    // Builds one row template root: a Panel holding a Dropdown bound to the row's own Options/Value
    // (with a Text option template bound to Label), a Slider bound to the row's own bounds, and a
    // Text bound to the row's Name. Returns nothing — the panel is the list's authored item child.
    void BuildRowTemplate(Document& doc, Element& list)
    {
        Element& panel = doc.Add(list, ElementKind::Panel);

        Element& dropdown = doc.Add(panel, ElementKind::Dropdown);
        dropdown.Bindings["items"] = "Options";
        dropdown.Bindings["value"] = "Value";
        Element& option = doc.Add(dropdown, ElementKind::Text);
        option.Bindings["text"] = "Label";

        Element& slider = doc.Add(panel, ElementKind::Slider);
        slider.Bindings["min"] = "Min";
        slider.Bindings["max"] = "Max";
        slider.Bindings["step"] = "Step";
        slider.Bindings["value"] = "SliderValue";

        Element& name = doc.Add(panel, ElementKind::Text);
        name.Bindings["text"] = "Name";
    }

    // The popup List of the currently-open dropdown popup, or nullptr when none is open.
    Element* OpenOptionList(Document& doc)
    {
        const PopupId top = doc.GetTopPopup();
        Element* const root = doc.GetPopupRoot(top);
        return root != nullptr && !root->Children.empty() ? root->Children.front() : nullptr;
    }
}

VE_REFLECT(::NestOption, 0x51E0000000000001ULL)
VE_FIELD(Label)
VE_REFLECT_END();

VE_REFLECT(::NestRow, 0x51E0000000000002ULL)
VE_ARRAY_FIELD(Options)
VE_FIELD(Value)
VE_FIELD(Min)
VE_FIELD(Max)
VE_FIELD(Step)
VE_FIELD(SliderValue)
VE_FIELD(Name)
VE_REFLECT_END();

VE_REFLECT(::NestModel, 0x51E0000000000003ULL)
VE_ARRAY_FIELD(Rows)
VE_ARRAY_FIELD(TopOptions)
VE_FIELD(TopSelected)
VE_REFLECT_END();

TEST_CASE("gui nested repeater: per-row dropdowns and sliders resolve against their own row")
{
    Document doc;
    Element& list = doc.Add(doc.Root(), ElementKind::List);
    list.Bindings["items"] = "Rows";
    BuildRowTemplate(doc, list);
    doc.InitWidget(list);

    // A top-level dropdown that must keep reading the root model, not any row.
    Element& topDropdown = doc.Add(doc.Root(), ElementKind::Dropdown);
    topDropdown.Bindings["items"] = "TopOptions";
    topDropdown.Bindings["value"] = "TopSelected";
    Element& topOption = doc.Add(topDropdown, ElementKind::Text);
    topOption.Bindings["text"] = "Label";
    doc.InitWidget(topDropdown);

    TypeRegistry registry;
    registry.Register<NestModel>();
    NestModel model;
    model.Rows = {
        NestRow{.Options = {NestOption{.Label = "Alpha"}, NestOption{.Label = "Beta"},
                            NestOption{.Label = "Gamma"}},
                .Value = 1,
                .Min = 0.0f,
                .Max = 100.0f,
                .Step = 0.0f,
                .SliderValue = 42.0f,
                .Name = "First"},
        NestRow{.Options = {NestOption{.Label = "One"}, NestOption{.Label = "Two"}},
                .Value = 0,
                .Min = 10.0f,
                .Max = 20.0f,
                .Step = 5.0f,
                .SliderValue = 99.0f,
                .Name = "Second"},
    };
    model.TopOptions = {NestOption{.Label = "X"}, NestOption{.Label = "Y"},
                        NestOption{.Label = "Z"}, NestOption{.Label = "W"}};
    model.TopSelected = 2;

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    REQUIRE(doc.GetItemCount(list) == 2);

    // Each row's Panel holds its dropdown, slider, and name Text in authored order.
    Element* const row0 = doc.GetItemElement(list, 0);
    Element* const row1 = doc.GetItemElement(list, 1);
    REQUIRE(row0 != nullptr);
    REQUIRE(row1 != nullptr);
    REQUIRE(row0->Children.size() == 3);
    REQUIRE(row1->Children.size() == 3);

    const Element& dropdown0 = *row0->Children[0];
    const Element& slider0 = *row0->Children[1];
    const Element& name0 = *row0->Children[2];
    const Element& dropdown1 = *row1->Children[0];
    const Element& slider1 = *row1->Children[1];
    const Element& name1 = *row1->Children[2];

    // Each dropdown reports its own row's option count (Max is count - 1) and selected label —
    // the two rows differ, so this is per-row resolution and row independence, not a shared array.
    CHECK(dropdown0.Widget.Max == doctest::Approx(2.0f));
    CHECK(dropdown0.Widget.Value == doctest::Approx(1.0f));
    CHECK(dropdown0.Text == "Beta");
    CHECK(dropdown1.Widget.Max == doctest::Approx(1.0f));
    CHECK(dropdown1.Widget.Value == doctest::Approx(0.0f));
    CHECK(dropdown1.Text == "One");

    // Each nested slider reads its own row's bounds and value: row 0 unclamped, row 1 clamped to
    // its max and snapped to its step — proving min/max/step/value all came from the row.
    CHECK(slider0.Widget.Min == doctest::Approx(0.0f));
    CHECK(slider0.Widget.Max == doctest::Approx(100.0f));
    CHECK(slider0.Widget.Step == doctest::Approx(0.0f));
    CHECK(slider0.Widget.Value == doctest::Approx(42.0f));
    CHECK(slider1.Widget.Min == doctest::Approx(10.0f));
    CHECK(slider1.Widget.Max == doctest::Approx(20.0f));
    CHECK(slider1.Widget.Step == doctest::Approx(5.0f));
    CHECK(slider1.Widget.Value == doctest::Approx(20.0f));

    // A plain `{field}` inside a row still reads that row (regression).
    CHECK(name0.Text == "First");
    CHECK(name1.Text == "Second");

    // A top-level dropdown still resolves against the root context (regression).
    CHECK(topDropdown.Widget.Max == doctest::Approx(3.0f));
    CHECK(topDropdown.Widget.Value == doctest::Approx(2.0f));
    CHECK(topDropdown.Text == "Z");

    // A model move re-reads each row's own binding: swap row 0's selection and re-tune row 1's
    // slider, and only the touched values move.
    model.Rows[0].Value = 2;
    model.Rows[1].SliderValue = 12.0f;
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(dropdown0.Text == "Gamma");
    CHECK(dropdown0.Widget.Value == doctest::Approx(2.0f));
    CHECK(slider1.Widget.Value == doctest::Approx(10.0f)); // clamp(12,10,20) snapped to step 5
    CHECK(dropdown1.Text == "One");                        // untouched row keeps its label
}

TEST_CASE("gui nested repeater: opening a nested dropdown's popup fills from that row's array")
{
    Document doc;
    doc.SetInteractive(true);
    Element& list = doc.Add(doc.Root(), ElementKind::List);
    list.Bindings["items"] = "Rows";
    BuildRowTemplate(doc, list);
    doc.InitWidget(list);

    TypeRegistry registry;
    registry.Register<NestModel>();
    NestModel model;
    model.Rows = {
        NestRow{.Options = {NestOption{.Label = "Alpha"}, NestOption{.Label = "Beta"},
                            NestOption{.Label = "Gamma"}},
                .Value = 0,
                .Name = "First"},
        NestRow{.Options = {NestOption{.Label = "One"}, NestOption{.Label = "Two"}},
                .Value = 0,
                .Name = "Second"},
    };

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();

    // Open the second row's dropdown: its popup List fills from row 1's own two options, not row 0's
    // three — the popup is parentless, so this proves NodeBase crossed the popup to the anchor's row.
    Element* const row1 = doc.GetItemElement(list, 1);
    REQUIRE(row1 != nullptr);
    Element& dropdown1 = *row1->Children[0];
    doc.SetFocus(&dropdown1);
    CHECK(doc.Navigate(NavAction::Confirm));
    REQUIRE(doc.GetPopupCount() == 1);
    Element* const optionList = OpenOptionList(doc);
    REQUIRE(optionList != nullptr);
    REQUIRE(doc.GetItemCount(*optionList) == 2);
    CHECK(doc.GetItemElement(*optionList, 0)->Text == "One");
    CHECK(doc.GetItemElement(*optionList, 1)->Text == "Two");
}
