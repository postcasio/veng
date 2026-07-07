// The Gui widget library — headless per-widget behavior cases. Device-free: each control's behavior
// is exercised over the plan-07 dispatch paths (pointer, navigation, text) with trees built directly
// and geometry pinned, plus a reflected in-test view-model for value and List bindings. No ICD and no
// font resource is needed — a widget's logic is pure CPU. Button activation fires onClick; Checkbox
// toggles Checked and its bound value; Slider drag/nudge changes the value and clamps; ProgressBar
// reflects a bound value; TextInput inserts/backspaces codepoints; ScrollView clips and offsets its
// children on scroll; a List instantiates N item elements from a bound array and re-syncs on change.

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
    // One inventory row the List repeater binds a child template against.
    struct Slot
    {
        string Name;
        i32 Count = 0;
    };

    // The binding data object: a bound scalar/bool a Slider/Checkbox/ProgressBar/TextInput read, plus
    // an array the List repeats over.
    struct WidgetModel
    {
        f32 Volume = 0.0f;
        bool Muted = false;
        f32 Health = 0.0f;
        string PlayerName;
        vector<Slot> Slots;
    };

    // Places an element at a fixed document-space rect, bypassing the flex solve — the pointer/scroll
    // cases pin geometry rather than exercise layout.
    void PlaceAt(Element& element, vec2 min, vec2 size)
    {
        element.Layout = Rect{.Min = min, .Size = size};
    }
}

VE_REFLECT(::Slot, 0x5108000000000001ULL)
VE_FIELD(Name)
VE_FIELD(Count)
VE_REFLECT_END();

VE_REFLECT(::WidgetModel, 0x5108000000000002ULL)
VE_FIELD(Volume)
VE_FIELD(Muted)
VE_FIELD(Health)
VE_FIELD(PlayerName)
VE_ARRAY_FIELD(Slots)
VE_REFLECT_END();

TEST_CASE("gui widget: a Button fires onClick on click and on gamepad confirm")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& button = doc.Add(root, ElementKind::Button);
    button.Focusable = true;
    PlaceAt(button, {10, 10}, {100, 40});
    button.Bindings["onClick"] = "activate";

    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);
    int clicks = 0;
    context.SetHandler("activate", [&](Element&) { ++clicks; });

    // A pointer press-then-release on the button is a click.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(50, 20)};
    doc.DispatchPointer(down);
    PointerEvent up{.Kind = PointerEventKind::Up, .Position = vec2(50, 20)};
    doc.DispatchPointer(up);
    CHECK(clicks == 1);

    // Gamepad confirm on the focused button fires the same handler.
    doc.SetFocus(&button);
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(clicks == 2);
}

TEST_CASE("gui widget: a Checkbox toggles its Checked bit and bound value on activation")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& box = doc.Add(root, ElementKind::Checkbox);
    box.Focusable = true;
    PlaceAt(box, {10, 10}, {24, 24});

    int changes = 0;
    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);
    box.Bindings["onChange"] = "changed";
    context.SetHandler("changed", [&](Element&) { ++changes; });

    CHECK((box.State & ElementState::Checked) == ElementState::None);
    CHECK(doc.GetWidgetValue(box) == 0.0f);

    // A click toggles it on: the Checked bit sets (driving the :checked variant) and onChange fires.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(20, 20)};
    doc.DispatchPointer(down);
    PointerEvent up{.Kind = PointerEventKind::Up, .Position = vec2(20, 20)};
    doc.DispatchPointer(up);
    CHECK((box.State & ElementState::Checked) == ElementState::Checked);
    CHECK(doc.GetWidgetValue(box) == 1.0f);
    CHECK(changes == 1);

    // A gamepad confirm toggles it back off.
    doc.SetFocus(&box);
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK((box.State & ElementState::Checked) == ElementState::None);
    CHECK(doc.GetWidgetValue(box) == 0.0f);
    CHECK(changes == 2);
}

TEST_CASE("gui widget: a Slider drag and directional nudge change the value, clamped to range")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& slider = doc.Add(root, ElementKind::Slider);
    slider.Focusable = true;
    PlaceAt(slider, {0, 0}, {100, 20});
    // Range 0..10, step 1 — the widget config the instantiate path parses off these bindings.
    slider.Bindings["min"] = "0";
    slider.Bindings["max"] = "10";
    slider.Bindings["step"] = "1";
    doc.InitWidget(slider);

    int changes = 0;
    f32 lastValue = -1.0f;
    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);
    slider.Bindings["onChange"] = "changed";
    context.SetHandler("changed",
                       [&](Element& e)
                       {
                           ++changes;
                           lastValue = doc.GetWidgetValue(e);
                       });

    CHECK(doc.GetWidgetValue(slider) == 0.0f);

    // A press at 70% of the width lands the value at 7 (0..10, stepped to 1).
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(70, 10)};
    doc.DispatchPointer(down);
    CHECK(doc.GetWidgetValue(slider) == doctest::Approx(7.0f));
    CHECK(changes == 1);
    CHECK(lastValue == doctest::Approx(7.0f));

    // A drag past the right edge clamps to the max.
    PointerEvent drag{.Kind = PointerEventKind::Move, .Position = vec2(500, 10)};
    doc.DispatchPointer(drag);
    CHECK(doc.GetWidgetValue(slider) == doctest::Approx(10.0f));
    PointerEvent up{.Kind = PointerEventKind::Up, .Position = vec2(500, 10)};
    doc.DispatchPointer(up);

    // A directional nudge left steps down by one; nudging past the min clamps.
    doc.SetFocus(&slider);
    CHECK(doc.Navigate(NavAction::MoveLeft));
    CHECK(doc.GetWidgetValue(slider) == doctest::Approx(9.0f));
}

TEST_CASE("gui widget: a ProgressBar reflects a one-way bound value")
{
    Document doc;

    Element& root = doc.Root();
    Element& bar = doc.Add(root, ElementKind::ProgressBar);
    bar.Bindings["value"] = "Health"; // one-way binding to the model's [0,1] health
    doc.InitWidget(bar);

    TypeRegistry registry;
    registry.Register<WidgetModel>();
    WidgetModel model;
    model.Health = 0.25f;

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);

    doc.UpdateBindings();
    CHECK(doc.GetWidgetValue(bar) == doctest::Approx(0.25f));

    // A signalled change updates the fill; the value clamps into [0,1].
    model.Health = 1.5f;
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(doc.GetWidgetValue(bar) == doctest::Approx(1.0f));
}

TEST_CASE("gui widget: a TextInput inserts and backspaces codepoints into its text")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& input = doc.Add(root, ElementKind::TextInput);
    PlaceAt(input, {0, 0}, {120, 24});
    doc.InitWidget(input);

    int changes = 0;
    BindingContext context;
    const TypeRegistry registry;
    doc.BindContext(&context, &registry);
    input.Bindings["onChange"] = "edited";
    context.SetHandler("edited", [&](Element&) { ++changes; });

    doc.SetFocus(&input);

    // Typing "Hi" inserts two codepoints at the caret.
    CHECK(doc.DispatchText('H'));
    CHECK(doc.DispatchText('i'));
    CHECK(input.Text == "Hi");
    CHECK(changes == 2);

    // Backspace (U+0008) deletes the codepoint before the caret.
    CHECK(doc.DispatchText(0x08));
    CHECK(input.Text == "H");
    CHECK(changes == 3);

    // A backspace on an empty edit position past the start still deletes down to empty, then no-ops.
    CHECK(doc.DispatchText(0x08));
    CHECK(input.Text.empty());
    CHECK_FALSE(doc.DispatchText(0x08));
}

TEST_CASE("gui widget: a ScrollView clips and offsets its children on scroll")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& view = doc.Add(root, ElementKind::ScrollView);
    view.Focusable = true;
    PlaceAt(view, {0, 0}, {100, 100});
    // A child taller than the viewport — the content the view scrolls through.
    Element& content = doc.Add(view, ElementKind::Panel);
    PlaceAt(content, {0, 0}, {100, 300});

    // Scrolling down moves the offset; a subsequent Solve is what applies it to children, so drive
    // the offset directly and check the clamp against the 200px overflow (300 content − 100 view).
    doc.ScrollBy(view, vec2(0, 50));
    CHECK(view.Widget.ScrollOffset.y == doctest::Approx(50.0f));

    // Scrolling past the content clamps to the overflow.
    doc.ScrollBy(view, vec2(0, 1000));
    CHECK(view.Widget.ScrollOffset.y == doctest::Approx(200.0f));

    // A gamepad up nudge scrolls back toward the top.
    doc.SetFocus(&view);
    CHECK(doc.Navigate(NavAction::MoveUp));
    CHECK(view.Widget.ScrollOffset.y < 200.0f);
}

TEST_CASE("gui widget: a ScrollView claims a drag started over a child")
{
    Document doc;
    doc.SetInteractive(true);

    Element& root = doc.Root();
    PlaceAt(root, {0, 0}, {200, 200});
    Element& view = doc.Add(root, ElementKind::ScrollView);
    view.Focusable = true;
    PlaceAt(view, {0, 0}, {100, 100});
    Element& content = doc.Add(view, ElementKind::Panel);
    PlaceAt(content, {0, 0}, {100, 300});

    // A press lands on the child, but the ScrollView captures it — the drag pans the view.
    PointerEvent down{.Kind = PointerEventKind::Down, .Position = vec2(50, 50)};
    doc.DispatchPointer(down);
    PointerEvent drag{.Kind = PointerEventKind::Move, .Position = vec2(50, 20)};
    doc.DispatchPointer(drag);
    // The pointer moved up 30px, so the content scrolls down 30px.
    CHECK(view.Widget.ScrollOffset.y == doctest::Approx(30.0f));
}

TEST_CASE(
    "gui widget: a List instantiates a child template per array element and re-syncs on change")
{
    Document doc;

    Element& root = doc.Root();
    Element& list = doc.Add(root, ElementKind::List);
    list.Bindings["items"] = "Slots"; // the bound reflected array
    doc.InitWidget(list);
    // The List's one authored child is the item template: a Text bound to the array element's Name.
    Element& itemTemplate = doc.Add(list, ElementKind::Text);
    itemTemplate.Bindings["text"] = "Name";

    TypeRegistry registry;
    registry.Register<WidgetModel>();
    WidgetModel model;
    model.Slots.push_back(Slot{.Name = "Sword", .Count = 1});
    model.Slots.push_back(Slot{.Name = "Shield", .Count = 1});

    BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);

    // The first sync instantiates one item element per array element, each bound to its element.
    doc.UpdateBindings();
    CHECK(list.Children.size() == 2);
    CHECK(list.Children[0]->Text == "Sword");
    CHECK(list.Children[1]->Text == "Shield");

    // Growing the array adds an item on the next signalled sync.
    model.Slots.push_back(Slot{.Name = "Potion", .Count = 3});
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(list.Children.size() == 3);
    CHECK(list.Children[2]->Text == "Potion");

    // Shrinking the array removes the trailing items.
    model.Slots.resize(1);
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(list.Children.size() == 1);
    CHECK(list.Children[0]->Text == "Sword");
}
