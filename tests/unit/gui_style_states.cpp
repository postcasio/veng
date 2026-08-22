// Gui style states + transitions: variant selection driven by interaction state, USS-order
// priority among simultaneously-active states, time-based property transitions, and the
// layout-vs-paint re-dirty split. Device-free — the whole style pipeline is pure CPU (no ICD,
// no font resource). Variants and transitions are authored directly on the elements (the same
// data the cook resolves), so the test exercises Document::Update / SetState with no cooked blob.

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    // A hover variant setting a background, plus its base — the smallest variant-selection fixture.
    StyleVariant HoverBackground(vec4 color)
    {
        StyleDeclaration decl;
        decl.Property = StyleProperty::Background;
        decl.Values = color;
        return StyleVariant{.State = ElementState::Hovered, .Declarations = {decl}};
    }

    // A variant scoped to one state, setting the text color — the property a label inside a control
    // has to move for the control to invert as one unit.
    StyleVariant TextColorIn(ElementState state, vec4 color)
    {
        StyleDeclaration decl;
        decl.Property = StyleProperty::TextColor;
        decl.Values = color;
        return StyleVariant{.State = state, .Declarations = {decl}};
    }
}

TEST_CASE("gui style: a :hover variant is selected on state and reverts when cleared")
{
    Document doc;
    Element& button = doc.Add(doc.Root(), ElementKind::Button);

    Style base;
    base.Background = vec4(0.1f, 0.1f, 0.1f, 1.0f);
    doc.SetStyle(button, base);

    button.Variants = {HoverBackground(vec4(0.8f, 0.8f, 0.8f, 1.0f))};

    // Base state: the live style is the base background.
    doc.Update(0.0f);
    CHECK(button.ComputedStyle.Background.r == doctest::Approx(0.1f));

    // Hovered: the variant's background wins.
    doc.SetState(button, ElementState::Hovered);
    CHECK(button.ComputedStyle.Background.r == doctest::Approx(0.8f));

    // Cleared: it reverts to base.
    doc.SetState(button, ElementState::None);
    CHECK(button.ComputedStyle.Background.r == doctest::Approx(0.1f));
}

TEST_CASE("gui style: simultaneously-active states compose in USS (source) order")
{
    Document doc;
    Element& button = doc.Add(doc.Root(), ElementKind::Button);

    Style base;
    base.Opacity = 1.0f;
    doc.SetStyle(button, base);

    StyleDeclaration hoverOpacity;
    hoverOpacity.Property = StyleProperty::Opacity;
    hoverOpacity.Values = vec4(0.7f, 0.0f, 0.0f, 0.0f);

    StyleDeclaration disabledOpacity;
    disabledOpacity.Property = StyleProperty::Opacity;
    disabledOpacity.Values = vec4(0.4f, 0.0f, 0.0f, 0.0f);

    // Source order: :hover then :disabled — the later-listed :disabled wins when both are active.
    button.Variants = {
        StyleVariant{.State = ElementState::Hovered, .Declarations = {hoverOpacity}},
        StyleVariant{.State = ElementState::Disabled, .Declarations = {disabledOpacity}},
    };

    doc.SetState(button, ElementState::Hovered);
    CHECK(button.ComputedStyle.Opacity == doctest::Approx(0.7f));

    doc.SetState(button, ElementState::Hovered | ElementState::Disabled);
    CHECK(button.ComputedStyle.Opacity == doctest::Approx(0.4f));

    // Only disabled: the hover entry is inactive, disabled still applies.
    doc.SetState(button, ElementState::Disabled);
    CHECK(button.ComputedStyle.Opacity == doctest::Approx(0.4f));
}

TEST_CASE("gui style: a transition eases a paint property and reaches the target at the duration")
{
    Document doc;
    Element& button = doc.Add(doc.Root(), ElementKind::Button);

    Style base;
    base.Background = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    doc.SetStyle(button, base);
    button.Variants = {HoverBackground(vec4(1.0f, 1.0f, 1.0f, 1.0f))};
    doc.SetTransitions(button,
                       {StyleTransition{.Property = StyleProperty::Background, .Duration = 1.0f}});

    // Entering hover starts the tween from black toward white; the value has not jumped.
    doc.SetState(button, ElementState::Hovered);
    CHECK(button.ComputedStyle.Background.r == doctest::Approx(0.0f));
    CHECK(doc.IsAnimating());

    // Halfway through the duration the value is the linear midpoint.
    doc.Update(0.5f);
    CHECK(button.ComputedStyle.Background.r == doctest::Approx(0.5f));
    CHECK(doc.IsAnimating());

    // Reaching the duration lands exactly on the target and the tween completes.
    doc.Update(0.5f);
    CHECK(button.ComputedStyle.Background.r == doctest::Approx(1.0f));
    CHECK_FALSE(doc.IsAnimating());
}

TEST_CASE("gui style: a transition eases the scalar rotation property to its target")
{
    Document doc;
    Element& needle = doc.Add(doc.Root(), ElementKind::Panel);

    Style base;
    base.Rotation = 0.0f;
    doc.SetStyle(needle, base);

    StyleDeclaration hoverRotation;
    hoverRotation.Property = StyleProperty::Rotation;
    hoverRotation.Values = vec4(90.0f, 0.0f, 0.0f, 0.0f);
    needle.Variants = {
        StyleVariant{.State = ElementState::Hovered, .Declarations = {hoverRotation}}};
    doc.SetTransitions(needle,
                       {StyleTransition{.Property = StyleProperty::Rotation, .Duration = 1.0f}});

    // Entering hover starts the tween from 0° toward 90°; the value has not jumped.
    doc.SetState(needle, ElementState::Hovered);
    CHECK(needle.ComputedStyle.Rotation == doctest::Approx(0.0f));
    CHECK(doc.IsAnimating());

    // Halfway through the duration the angle is the linear midpoint.
    doc.Update(0.5f);
    CHECK(needle.ComputedStyle.Rotation == doctest::Approx(45.0f));

    // A pure rotation tween is paint-only — it never dirties layout.
    doc.Solve(vec2(100.0f, 100.0f));
    CHECK_FALSE(doc.IsDirty());
    doc.Update(0.25f);
    CHECK_FALSE(doc.IsDirty());

    // Reaching the duration lands exactly on the target and the tween completes.
    doc.Update(0.25f);
    CHECK(needle.ComputedStyle.Rotation == doctest::Approx(90.0f));
    CHECK_FALSE(doc.IsAnimating());
}

TEST_CASE("gui style: a non-transitioned property snaps on a state change")
{
    Document doc;
    Element& button = doc.Add(doc.Root(), ElementKind::Button);

    Style base;
    base.Background = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    doc.SetStyle(button, base);
    button.Variants = {HoverBackground(vec4(1.0f, 1.0f, 1.0f, 1.0f))};
    // No transition configured: the property snaps.

    doc.SetState(button, ElementState::Hovered);
    CHECK(button.ComputedStyle.Background.r == doctest::Approx(1.0f));
    CHECK_FALSE(doc.IsAnimating());
}

TEST_CASE("gui style: a paint-only variant does not re-dirty layout, a layout variant does")
{
    Document doc;
    Element& panel = doc.Add(doc.Root(), ElementKind::Panel);

    Style base;
    base.Background = vec4(0.0f);
    base.Width = Length::Points(40.0f);
    doc.SetStyle(panel, base);

    // A hover variant changing only the background (paint-only) and a focus variant changing the
    // width (a layout input).
    StyleDeclaration hoverBg;
    hoverBg.Property = StyleProperty::Background;
    hoverBg.Values = vec4(1.0f, 0.0f, 0.0f, 1.0f);

    StyleDeclaration focusWidth;
    focusWidth.Property = StyleProperty::Width;
    focusWidth.Unit = static_cast<u32>(LengthKind::Points);
    focusWidth.Values = vec4(80.0f, 0.0f, 0.0f, 0.0f);

    panel.Variants = {
        StyleVariant{.State = ElementState::Hovered, .Declarations = {hoverBg}},
        StyleVariant{.State = ElementState::Focused, .Declarations = {focusWidth}},
    };

    doc.Solve(vec2(200.0f, 200.0f));
    REQUIRE_FALSE(doc.IsDirty());

    // A paint-only change (background) does not force a re-solve.
    doc.SetState(panel, ElementState::Hovered);
    CHECK(panel.ComputedStyle.Background.r == doctest::Approx(1.0f));
    CHECK_FALSE(doc.IsDirty());

    // A layout-input change (width) re-dirties so the following Solve re-runs.
    doc.SetState(panel, ElementState::Focused);
    CHECK(doc.IsDirty());
    doc.Solve(vec2(200.0f, 200.0f));
    CHECK(panel.Layout.Size.x == doctest::Approx(80.0f));
}

TEST_CASE("gui style: a font declaration resolves to nothing with no asset manager")
{
    // An imperatively-built document borrows no AssetManager (m_Assets is null), the device-free
    // path. A TextFont declaration is guarded on the manager, so resolving it leaves the style's
    // font unchanged rather than dereferencing null — the tree still builds and updates.
    Document doc;
    Element& label = doc.Add(doc.Root(), ElementKind::Text);
    CHECK(doc.Root().Children.size() == 1);

    StyleDeclaration font;
    font.Property = StyleProperty::TextFont;
    font.Handle = AssetId{0x0123456789ABCDEFULL};
    label.Variants = {StyleVariant{.State = ElementState::Hovered, .Declarations = {font}}};

    // Activating the variant runs the font declaration through the guarded, manager-less path.
    doc.SetState(label, ElementState::Hovered);
    CHECK_FALSE(label.ComputedStyle.TextFont.Id().IsValid());
}

TEST_CASE("gui style: an animating width transition re-dirties layout each step")
{
    Document doc;
    Element& panel = doc.Add(doc.Root(), ElementKind::Panel);

    Style base;
    base.Width = Length::Points(40.0f);
    base.FlexShrink = 0.0f;
    doc.SetStyle(panel, base);

    StyleDeclaration hoverWidth;
    hoverWidth.Property = StyleProperty::Width;
    hoverWidth.Unit = static_cast<u32>(LengthKind::Points);
    hoverWidth.Values = vec4(140.0f, 0.0f, 0.0f, 0.0f);
    panel.Variants = {StyleVariant{.State = ElementState::Hovered, .Declarations = {hoverWidth}}};
    doc.SetTransitions(panel,
                       {StyleTransition{.Property = StyleProperty::Width, .Duration = 1.0f}});

    doc.Solve(vec2(400.0f, 200.0f));

    // Entering hover starts a width tween; the layout must re-solve to the interpolated width.
    doc.SetState(panel, ElementState::Hovered);
    doc.Update(0.5f);
    CHECK(doc.IsDirty());
    doc.Solve(vec2(400.0f, 200.0f));
    CHECK(panel.Layout.Size.x == doctest::Approx(90.0f)); // midpoint of 40..140

    doc.Update(0.5f);
    doc.Solve(vec2(400.0f, 200.0f));
    CHECK(panel.Layout.Size.x == doctest::Approx(140.0f));
    CHECK_FALSE(doc.IsAnimating());
}

TEST_CASE("gui style: an interaction state a control carries reaches the labels inside it")
{
    // A control's label is a child element with a color of its own, and text color does not inherit
    // — so a `:selected` rule on the label is the only thing that can invert it, and it has to
    // resolve off the *control's* bit. Nothing ever sets a state on a Text leaf directly.
    Document doc;
    Element& row = doc.Add(doc.Root(), ElementKind::Button);
    Element& label = doc.Add(row, ElementKind::Text);
    Element& group = doc.Add(row, ElementKind::Panel);
    Element& deeper = doc.Add(group, ElementKind::Text);

    Style base;
    base.TextColor = vec4(0.0f, 1.0f, 1.0f, 1.0f);
    doc.SetStyle(label, base);
    doc.SetStyle(deeper, base);

    const vec4 knockedOut{0.0f, 0.0f, 0.0f, 1.0f};
    label.Variants = {TextColorIn(ElementState::Selected, knockedOut)};
    deeper.Variants = {TextColorIn(ElementState::Selected, knockedOut)};

    doc.Update(0.0f);
    CHECK(label.ComputedStyle.TextColor.g == doctest::Approx(1.0f));

    // Lit on the row alone, and resolved on the same call rather than a frame later: a ground that
    // inverts before the text on it is a visible tear.
    doc.SetState(row, ElementState::Selected);
    CHECK(label.ComputedStyle.TextColor.g == doctest::Approx(0.0f));
    CHECK(deeper.ComputedStyle.TextColor.g == doctest::Approx(0.0f));
    CHECK(label.State == ElementState::None);

    doc.SetState(row, ElementState::None);
    CHECK(label.ComputedStyle.TextColor.g == doctest::Approx(1.0f));
    CHECK(deeper.ComputedStyle.TextColor.g == doctest::Approx(1.0f));
}

TEST_CASE("gui style: focus and checkedness stay on the element that carries them")
{
    // The line the inheritance stops at. A focus ring is a fact about the control, and a checkbox's
    // value is its own two-state reading — a label painting itself checked because the box beside it
    // is would be saying something no author wrote.
    Document doc;
    Element& box = doc.Add(doc.Root(), ElementKind::Button);
    Element& label = doc.Add(box, ElementKind::Text);

    Style base;
    base.TextColor = vec4(0.0f, 1.0f, 1.0f, 1.0f);
    doc.SetStyle(label, base);

    const vec4 moved{1.0f, 0.0f, 0.0f, 1.0f};
    label.Variants = {TextColorIn(ElementState::Focused, moved),
                      TextColorIn(ElementState::Checked, moved)};

    doc.SetState(box, ElementState::Focused | ElementState::Checked);
    doc.Update(0.0f);
    CHECK(label.ComputedStyle.TextColor.g == doctest::Approx(1.0f));

    // And the same two bits on the label itself do reach it, so the case above is about the chain
    // rather than about the properties being unreachable.
    doc.SetState(label, ElementState::Checked);
    CHECK(label.ComputedStyle.TextColor.g == doctest::Approx(0.0f));
}

TEST_CASE("gui style: a disabled container greys the controls inside it")
{
    // Disabled inherits for the same reason the other three do: a greyed panel whose labels stayed
    // lit would read as a live control that merely refuses the pointer.
    Document doc;
    Element& panel = doc.Add(doc.Root(), ElementKind::Panel);
    Element& inner = doc.Add(panel, ElementKind::Button);

    Style base;
    base.Opacity = 1.0f;
    doc.SetStyle(inner, base);

    StyleDeclaration dim;
    dim.Property = StyleProperty::Opacity;
    dim.Values = vec4(0.35f);
    inner.Variants = {StyleVariant{.State = ElementState::Disabled, .Declarations = {dim}}};

    doc.Update(0.0f);
    CHECK(inner.ComputedStyle.Opacity == doctest::Approx(1.0f));

    doc.SetState(panel, ElementState::Disabled);
    CHECK(inner.ComputedStyle.Opacity == doctest::Approx(0.35f));
}
