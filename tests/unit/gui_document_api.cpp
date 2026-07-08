// The Document convenience API: FindById lookup, the paint-only setters and SetPlacement (the
// layout-dirty discrimination), pointer-events hit-test transparency, anchored absolute
// positioning through unset inset edges, style animations, and style-property bindings.
// Device-free — panels only, no font resource.

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;
using namespace Veng::Gui;

// A reflected model whose fields the style-binding cases bind paint properties to. Global scope —
// the reflect macros require the fully-qualified `::` spelling.
struct PaintModel
{
    Veng::vec4 Tint{1.0f, 0.0f, 0.0f, 1.0f};
    Veng::f32 Fade = 0.5f;
};

namespace
{
    void CheckRect(const Rect& rect, f32 x, f32 y, f32 w, f32 h)
    {
        CHECK(rect.Min.x == doctest::Approx(x));
        CHECK(rect.Min.y == doctest::Approx(y));
        CHECK(rect.Size.x == doctest::Approx(w));
        CHECK(rect.Size.y == doctest::Approx(h));
    }

    // A one-property keyframe: `property` = `value` at `offset`.
    StyleKeyframe KeyframeAt(f32 offset, StyleProperty property, vec4 value)
    {
        StyleDeclaration declaration;
        declaration.Property = property;
        declaration.Values = value;
        return StyleKeyframe{.Offset = offset, .Declarations = {declaration}};
    }
}

VE_REFLECT(::PaintModel, 0x51D0000000000001ULL)
VE_FIELD(Tint)
VE_FIELD(Fade)
VE_REFLECT_END();

TEST_CASE("gui document: FindById finds a nested element by authored id")
{
    Document doc;
    Element& outer = doc.Add(doc.Root(), ElementKind::Panel);
    Element& inner = doc.Add(outer, ElementKind::Text);
    inner.Id = "status-line";

    CHECK(doc.FindById("status-line") == &inner);
    CHECK(doc.FindById("missing") == nullptr);
    // An empty id matches nothing, not the untagged root.
    CHECK(doc.FindById("") == nullptr);
}

TEST_CASE("gui document: SetPlacement pins a rect and re-dirties layout only on a real change")
{
    Document doc;
    Element& panel = doc.Add(doc.Root(), ElementKind::Panel);

    doc.SetPlacement(panel, vec2(20.0f, 30.0f), vec2(50.0f, 40.0f));
    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(panel.Layout, 20.0f, 30.0f, 50.0f, 40.0f);
    CHECK(!doc.IsDirty());

    // Re-pinning the identical rect is free — no re-solve.
    doc.SetPlacement(panel, vec2(20.0f, 30.0f), vec2(50.0f, 40.0f));
    CHECK(!doc.IsDirty());

    // Moving it re-dirties and re-solves to the new rect.
    doc.SetPlacement(panel, vec2(60.0f, 30.0f), vec2(50.0f, 40.0f));
    CHECK(doc.IsDirty());
    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(panel.Layout, 60.0f, 30.0f, 50.0f, 40.0f);
}

TEST_CASE("gui document: paint-only setters change the live style without a layout re-solve")
{
    Document doc;
    Element& panel = doc.Add(doc.Root(), ElementKind::Panel);
    doc.Solve(vec2(100.0f, 100.0f));
    CHECK(!doc.IsDirty());

    doc.SetOpacity(panel, 0.25f);
    doc.SetBackground(panel, vec4(0.0f, 1.0f, 0.0f, 1.0f));
    doc.SetTextColor(panel, vec4(0.0f, 0.0f, 1.0f, 1.0f));
    doc.SetBackgroundGradient(
        panel, ResolvedGradient{.Kind = GradientKind::Linear, .P0 = vec2(0.0f, -1.0f)});

    CHECK(!doc.IsDirty());
    CHECK(panel.ComputedStyle.Opacity == doctest::Approx(0.25f));
    CHECK(panel.ComputedStyle.Background.g == doctest::Approx(1.0f));
    CHECK(panel.ComputedStyle.TextColor.b == doctest::Approx(1.0f));
    REQUIRE(panel.ComputedStyle.BackgroundGradient.has_value());
    CHECK(panel.ComputedStyle.BackgroundGradient->P0.y == doctest::Approx(-1.0f));

    // The write landed on the base too, so a variant re-resolve keeps it — the seam a game animates
    // through, mutating a gradient's endpoints and re-setting it each frame.
    doc.Update(0.0f);
    CHECK(panel.ComputedStyle.Opacity == doctest::Approx(0.25f));
    REQUIRE(panel.ComputedStyle.BackgroundGradient.has_value());
    CHECK(panel.ComputedStyle.BackgroundGradient->P0.y == doctest::Approx(-1.0f));

    // Clearing falls back to the flat background.
    doc.SetBackgroundGradient(panel, std::nullopt);
    CHECK(!panel.ComputedStyle.BackgroundGradient.has_value());
}

TEST_CASE("gui document: pointer-events none makes an element and its subtree hit-transparent")
{
    Document doc;
    Element& below = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetPlacement(below, vec2(0.0f, 0.0f), vec2(100.0f, 100.0f));

    // The overlay paints over `below` and carries a child — both must pass the point through.
    Element& overlay = doc.Add(doc.Root(), ElementKind::Panel);
    doc.SetPlacement(overlay, vec2(0.0f, 0.0f), vec2(100.0f, 100.0f));
    Element& label = doc.Add(overlay, ElementKind::Text);
    doc.SetPlacement(label, vec2(10.0f, 10.0f), vec2(50.0f, 20.0f));

    doc.Solve(vec2(100.0f, 100.0f));
    CHECK(doc.HitTest(vec2(20.0f, 20.0f)) == &label);

    Style transparent = overlay.BaseStyle;
    transparent.Pointer = PointerEvents::None;
    doc.SetStyle(overlay, transparent);
    doc.Solve(vec2(100.0f, 100.0f));

    CHECK(doc.HitTest(vec2(20.0f, 20.0f)) == &below);
}

TEST_CASE("gui layout: an absolute element anchors from right/bottom insets at its own size")
{
    Document doc;
    Element& badge = doc.Add(doc.Root(), ElementKind::Panel);

    Style style;
    style.Position = PositionType::Absolute;
    style.Inset = {.Right = 10.0f, .Bottom = 20.0f};
    style.Width = Length::Points(50.0f);
    style.Height = Length::Points(30.0f);
    doc.SetStyle(badge, style);

    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(badge.Layout, 140.0f, 150.0f, 50.0f, 30.0f);
}

TEST_CASE("gui layout: opposing set insets with an auto size stretch the element between them")
{
    Document doc;
    Element& bar = doc.Add(doc.Root(), ElementKind::Panel);

    Style style;
    style.Position = PositionType::Absolute;
    style.Inset = {.Left = 10.0f, .Top = 50.0f, .Right = 10.0f};
    style.Height = Length::Points(30.0f);
    doc.SetStyle(bar, style);

    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(bar.Layout, 10.0f, 50.0f, 180.0f, 30.0f);
}

TEST_CASE("gui animation: a looping opacity clip interpolates and wraps")
{
    Document doc;
    Element& pulse = doc.Add(doc.Root(), ElementKind::Panel);

    doc.SetAnimations(
        pulse,
        {StyleAnimation{
            .Keyframes = {KeyframeAt(0.0f, StyleProperty::Opacity, vec4(0.0f, 0.0f, 0.0f, 0.0f)),
                          KeyframeAt(1.0f, StyleProperty::Opacity, vec4(1.0f, 0.0f, 0.0f, 0.0f))},
            .Duration = 2.0f,
            .Mode = AnimationLoopMode::Loop}});

    // Half way through the clip: half opacity.
    doc.Update(1.0f);
    CHECK(pulse.ComputedStyle.Opacity == doctest::Approx(0.5f));

    // A paint-only animation never dirties layout.
    doc.Solve(vec2(100.0f, 100.0f));
    CHECK(!doc.IsDirty());
    doc.Update(0.5f);
    CHECK(!doc.IsDirty());
    CHECK(pulse.ComputedStyle.Opacity == doctest::Approx(0.75f));

    // The loop wraps: 2.25s into a 2s clip reads as phase 0.125.
    doc.Update(0.75f);
    CHECK(pulse.ComputedStyle.Opacity == doctest::Approx(0.125f));

    CHECK(doc.IsAnimating());
}

TEST_CASE("gui animation: a ping-pong clip mirrors alternate cycles")
{
    Document doc;
    Element& sweep = doc.Add(doc.Root(), ElementKind::Panel);

    doc.SetAnimations(
        sweep,
        {StyleAnimation{
            .Keyframes = {KeyframeAt(0.0f, StyleProperty::Opacity, vec4(0.0f, 0.0f, 0.0f, 0.0f)),
                          KeyframeAt(1.0f, StyleProperty::Opacity, vec4(1.0f, 0.0f, 0.0f, 0.0f))},
            .Duration = 1.0f,
            .Mode = AnimationLoopMode::PingPong}});

    // 1.25s into a 1s ping-pong: the second cycle runs backward, phase 0.75.
    doc.Update(1.25f);
    CHECK(sweep.ComputedStyle.Opacity == doctest::Approx(0.75f));
}

TEST_CASE("gui animation: a keyframed layout input re-dirties layout as it animates")
{
    Document doc;
    Element& grower = doc.Add(doc.Root(), ElementKind::Panel);
    doc.Solve(vec2(100.0f, 100.0f));
    CHECK(!doc.IsDirty());

    StyleDeclaration narrow;
    narrow.Property = StyleProperty::Width;
    narrow.Unit = static_cast<u32>(LengthKind::Points);
    narrow.Values = vec4(10.0f, 0.0f, 0.0f, 0.0f);
    StyleDeclaration wide = narrow;
    wide.Values = vec4(90.0f, 0.0f, 0.0f, 0.0f);

    doc.SetAnimations(
        grower,
        {StyleAnimation{.Keyframes = {StyleKeyframe{.Offset = 0.0f, .Declarations = {narrow}},
                                      StyleKeyframe{.Offset = 1.0f, .Declarations = {wide}}},
                        .Duration = 1.0f,
                        .Mode = AnimationLoopMode::Once}});

    doc.Update(0.5f);
    CHECK(doc.IsDirty());
    doc.Solve(vec2(100.0f, 100.0f));
    CHECK(grower.Layout.Size.x == doctest::Approx(50.0f));
}

TEST_CASE("gui binding: a paint style property binds to a reflected model field")
{
    Document doc;
    Element& swatch = doc.Add(doc.Root(), ElementKind::Panel);
    swatch.Bindings["background"] = "Tint";
    swatch.Bindings["opacity"] = "Fade";

    PaintModel model;
    BindingContext context;
    context.SetData(model);
    TypeRegistry registry;
    registry.Register<PaintModel>();
    doc.BindContext(&context, &registry);

    doc.UpdateBindings();
    CHECK(swatch.ComputedStyle.Background.r == doctest::Approx(1.0f));
    CHECK(swatch.ComputedStyle.Background.g == doctest::Approx(0.0f));
    CHECK(swatch.ComputedStyle.Opacity == doctest::Approx(0.5f));

    // A model edit re-resolves on the next version bump.
    model.Tint = vec4(0.0f, 1.0f, 0.0f, 1.0f);
    model.Fade = 1.0f;
    context.Invalidate();
    doc.UpdateBindings();
    CHECK(swatch.ComputedStyle.Background.g == doctest::Approx(1.0f));
    CHECK(swatch.ComputedStyle.Opacity == doctest::Approx(1.0f));
}

TEST_CASE("gui layout: origin anchors an element at its anchor point, growing around it")
{
    Document doc;
    Element& ring = doc.Add(doc.Root(), ElementKind::Panel);

    // A centered anchor: the placement names where the center sits, not the top-left.
    Style style;
    style.Origin = vec2(0.5f);
    doc.SetStyle(ring, style);
    doc.SetPlacement(ring, vec2(100.0f, 80.0f), vec2(40.0f, 40.0f));

    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(ring.Layout, 80.0f, 60.0f, 40.0f, 40.0f);

    // Growing the element keeps the anchor fixed — the pulse case.
    doc.SetPlacement(ring, vec2(100.0f, 80.0f), vec2(60.0f, 60.0f));
    doc.Solve(vec2(200.0f, 200.0f));
    CheckRect(ring.Layout, 70.0f, 50.0f, 60.0f, 60.0f);
}

TEST_CASE("gui draw: an oversized corner radius clamps to half the box (a circle)")
{
    DrawList list;
    list.Quad(Rect{.Min = vec2(0.0f), .Size = vec2(40.0f, 40.0f)}, vec4(1.0f, 1.0f, 1.0f, 1.0f),
              CornerRadii::All(999.0f));
    REQUIRE(list.GetVertices().size() == 4);
    CHECK(list.GetVertices()[0].Params.x == doctest::Approx(20.0f));
}

TEST_CASE("gui image: Build emits a textured quad for a resident Image, nothing when unresolved")
{
    Document doc;
    Element& image = doc.Add(doc.Root(), ElementKind::Image);

    Style style;
    style.Width = Length::Points(64.0f);
    style.Height = Length::Points(48.0f);
    doc.SetStyle(image, style);
    doc.Solve(vec2(200.0f, 200.0f));

    // Unresolved: no texture slot set, and the box has no background/border, so the Image paints
    // nothing at all — no textured run.
    {
        DrawList list;
        doc.Build(list);
        CHECK(list.IsEmpty());
    }

    // Resident (stub): set the bindless slots, a UV sub-rect, and a tint directly — the state the
    // instantiate-time resolve fills from a resident texture, without a device.
    image.ImageTexture = Renderer::TextureHandle{.Index = 7};
    image.ImageSampler = Renderer::SamplerHandle{.Index = 3};
    image.ImageUv = Rect{.Min = {0.25f, 0.5f}, .Size = {0.5f, 0.25f}};
    image.ImageTint = vec4(1.0f, 0.5f, 0.25f, 0.8f);

    DrawList list;
    doc.Build(list);

    // Exactly one Shape run keyed by the texture; its quad carries the element's rect corner, the
    // UV sub-rect, and the tint (whose alpha folds the style opacity, 1.0 here).
    REQUIRE(list.GetRuns().size() == 1);
    CHECK(list.GetRuns()[0].Pipeline == GuiPipeline::Shape);
    REQUIRE(list.GetVertices().size() == 4);
    const GuiVertex& topLeft = list.GetVertices()[0];
    CHECK(topLeft.Params.z == doctest::Approx(7.0f));
    CHECK(topLeft.Params.w == doctest::Approx(3.0f));
    CHECK(topLeft.Position.x == doctest::Approx(image.Layout.Min.x));
    CHECK(topLeft.Position.y == doctest::Approx(image.Layout.Min.y));
    CHECK(topLeft.Uv.x == doctest::Approx(0.25f));
    CHECK(topLeft.Uv.y == doctest::Approx(0.5f));
    CHECK(topLeft.Color.r == doctest::Approx(1.0f));
    CHECK(topLeft.Color.g == doctest::Approx(0.5f));
    CHECK(topLeft.Color.a == doctest::Approx(0.8f));
}
