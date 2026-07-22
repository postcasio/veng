// The arithmetic every texture fill resolves to before it reaches the draw list: the object-fit
// sub-rect, the pixel→UV slice conversion, and the tiled UV scale. Device-free — the fill geometry
// is pure math over a box and a texture size, shared by a container's background-image and the
// Image widget's content fill, so it is pinned here rather than only through a rendered capture.
//
// The fill-source precedence rule's fall-through half lives here too: which source *wins* needs
// resident assets and so is proven by the image goldens, but that an unresolved higher-ranked
// source falls through to the next rather than blanking the fill is device-free.

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>

#include "Gui/FillGeometry.h"

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    void CheckVec2(vec2 actual, vec2 expected)
    {
        CHECK(actual.x == doctest::Approx(expected.x));
        CHECK(actual.y == doctest::Approx(expected.y));
    }

    void CheckRect(const Rect& actual, const Rect& expected)
    {
        CheckVec2(actual.Min, expected.Min);
        CheckVec2(actual.Size, expected.Size);
    }

    constexpr Rect WholeTexture{.Min = vec2(0.0f), .Size = vec2(1.0f)};
}

TEST_CASE("gui fill geometry: object-fit maps a square source into a wide box")
{
    const Rect box{.Min = {10.0f, 20.0f}, .Size = {100.0f, 50.0f}};
    const vec2 source{20.0f, 20.0f};

    // Fill ignores the aspect entirely: the whole texture stretches over the whole box.
    const FittedFill fill = FitTexture(box, source, ImageFit::Fill);
    CheckRect(fill.Dest, box);
    CheckRect(fill.Uv, WholeTexture);

    // Contain fits the limiting axis (here the height) and letterboxes the destination.
    const FittedFill contain = FitTexture(box, source, ImageFit::Contain);
    CheckRect(contain.Dest, Rect{.Min = {35.0f, 20.0f}, .Size = {50.0f, 50.0f}});
    CheckRect(contain.Uv, WholeTexture);

    // Cover keeps the box and crops the UV on the overflowing axis instead.
    const FittedFill cover = FitTexture(box, source, ImageFit::Cover);
    CheckRect(cover.Dest, box);
    CheckRect(cover.Uv, Rect{.Min = {0.0f, 0.25f}, .Size = {1.0f, 0.5f}});

    // None draws the texture's own pixels, centered — smaller than the box here, so nothing crops.
    const FittedFill none = FitTexture(box, source, ImageFit::None);
    CheckRect(none.Dest, Rect{.Min = {50.0f, 35.0f}, .Size = {20.0f, 20.0f}});
    CheckRect(none.Uv, WholeTexture);
}

TEST_CASE("gui fill geometry: object-fit maps a wide source into a tall box")
{
    // The mirrored aspect relation: the limiting axis swaps, so contain and cover swap which axis
    // they letterbox and which they crop.
    const Rect box{.Min = {0.0f, 0.0f}, .Size = {20.0f, 40.0f}};
    const vec2 source{80.0f, 20.0f};

    const FittedFill contain = FitTexture(box, source, ImageFit::Contain);
    CheckRect(contain.Dest, Rect{.Min = {0.0f, 17.5f}, .Size = {20.0f, 5.0f}});

    const FittedFill cover = FitTexture(box, source, ImageFit::Cover);
    CheckRect(cover.Dest, box);
    CheckRect(cover.Uv, Rect{.Min = {0.4375f, 0.0f}, .Size = {0.125f, 1.0f}});

    // None is the one mode that crops *and* centers: the source is wider than the box, so the
    // destination is the box's width at the source's height and the UV takes the middle quarter.
    const FittedFill none = FitTexture(box, source, ImageFit::None);
    CheckRect(none.Dest, Rect{.Min = {0.0f, 10.0f}, .Size = {20.0f, 20.0f}});
    CheckRect(none.Uv, Rect{.Min = {0.375f, 0.0f}, .Size = {0.25f, 1.0f}});
}

TEST_CASE("gui fill geometry: a matching aspect collapses contain and cover onto the box")
{
    // The boundary between letterboxing and cropping: at an exactly matching aspect ratio both
    // modes degenerate to the plain stretch, which is what makes the two ramps continuous.
    const Rect box{.Min = {4.0f, 6.0f}, .Size = {40.0f, 20.0f}};
    const vec2 source{80.0f, 40.0f};

    const FittedFill contain = FitTexture(box, source, ImageFit::Contain);
    CheckRect(contain.Dest, box);
    CheckRect(contain.Uv, WholeTexture);

    const FittedFill cover = FitTexture(box, source, ImageFit::Cover);
    CheckRect(cover.Dest, box);
    CheckRect(cover.Uv, WholeTexture);
}

TEST_CASE("gui fill geometry: fit is computed against the sampled sub-rect, not the whole sheet")
{
    // An atlas frame fits its own cell: the crop a Cover applies is a fraction *of the cell*, so
    // the sampled rect never leaves the cell and a flipbook advance cannot bleed into its
    // neighbour. The right half of a sheet, 32×64 texels of it, into a square box.
    const Rect cell{.Min = {0.5f, 0.0f}, .Size = {0.5f, 1.0f}};
    const Rect box{.Min = {0.0f, 0.0f}, .Size = {32.0f, 32.0f}};

    const FittedFill cover = FitTexture(box, vec2(32.0f, 64.0f), ImageFit::Cover, cell);
    CheckRect(cover.Dest, box);
    CheckRect(cover.Uv, Rect{.Min = {0.5f, 0.25f}, .Size = {0.5f, 0.5f}});
    CHECK(cover.Uv.Min.x >= cell.Min.x);
    CHECK(cover.Uv.Max().x <= cell.Max().x);

    // A degenerate source or box maps the cell onto the box unchanged rather than dividing by zero.
    CheckRect(FitTexture(box, vec2(0.0f), ImageFit::Cover, cell).Uv, cell);
    CheckRect(FitTexture(Rect{.Min = vec2(0.0f), .Size = vec2(0.0f)}, vec2(32.0f),
                         ImageFit::Contain, cell)
                  .Uv,
              cell);
}

TEST_CASE("gui fill geometry: slice insets convert from source pixels to UV fractions")
{
    // The authored value is source-texture pixels — the same number for a 24px frame whatever box
    // it is stretched over — so the primitive's UV split is that pixel count over the sampled size.
    const Insets slice{.Left = 4.0f, .Top = 8.0f, .Right = 4.0f, .Bottom = 8.0f};
    const Insets uv = SliceToUv(slice, vec2(32.0f, 64.0f));
    CHECK(uv.Left == doctest::Approx(0.125f));
    CHECK(uv.Top == doctest::Approx(0.125f));
    CHECK(uv.Right == doctest::Approx(0.125f));
    CHECK(uv.Bottom == doctest::Approx(0.125f));

    // Asymmetric edges stay independent, and each axis divides by its own source extent.
    const Insets uneven = SliceToUv(
        Insets{.Left = 2.0f, .Top = 16.0f, .Right = 6.0f, .Bottom = 0.0f}, vec2(8.0f, 32.0f));
    CHECK(uneven.Left == doctest::Approx(0.25f));
    CHECK(uneven.Top == doctest::Approx(0.5f));
    CHECK(uneven.Right == doctest::Approx(0.75f));
    CHECK(uneven.Bottom == doctest::Approx(0.0f));

    // A source with no extent yields zero fractions rather than a division by zero.
    const Insets degenerate = SliceToUv(slice, vec2(0.0f));
    CHECK(degenerate.Left == doctest::Approx(0.0f));
    CHECK(degenerate.Bottom == doctest::Approx(0.0f));

    // Any positive edge makes a fill sliced; an all-zero inset leaves it a plain quad.
    CHECK(IsSliced(slice));
    CHECK(IsSliced(Insets{.Left = 0.0f, .Top = 0.0f, .Right = 0.0f, .Bottom = 1.0f}));
    CHECK_FALSE(IsSliced(Insets{}));
}

TEST_CASE("gui fill geometry: the tiled UV scale is the box measured in whole textures")
{
    // Tiling is one quad with a scaled UV rect against a wrapping sampler, so the repeat count is
    // arithmetic and a non-integral ratio simply ends mid-tile — 12.5 tiles across, 6.25 down.
    CheckVec2(TileUvSize(vec2(100.0f, 50.0f), vec2(8.0f), vec2(1.0f)), vec2(12.5f, 6.25f));

    // A box smaller than one tile samples a fraction of the texture: the sub-unit case a
    // quad-per-tile emitter could not express at all.
    CheckVec2(TileUvSize(vec2(3.0f, 2.0f), vec2(8.0f), vec2(1.0f)), vec2(0.375f, 0.25f));

    // The two axes scale independently against their own texel extents.
    CheckVec2(TileUvSize(vec2(96.0f, 96.0f), vec2(32.0f, 8.0f), vec2(1.0f)), vec2(3.0f, 12.0f));

    // An exactly-one-tile box is the identity, which is what makes an unsized tile read as the
    // plain stretched fill.
    CheckVec2(TileUvSize(vec2(8.0f), vec2(8.0f), vec2(1.0f)), vec2(1.0f));

    // A texture with no extent falls back to the caller's UV rather than dividing by zero — the
    // whole texture for a background, the element's own sub-rect for an Image.
    CheckVec2(TileUvSize(vec2(100.0f), vec2(0.0f, 8.0f), vec2(0.25f, 0.5f)), vec2(0.25f, 0.5f));
}

TEST_CASE("gui fill geometry: an unresolved fill source falls through to the next one")
{
    // Fill sources are exclusive and ranked material > gradient > image > color. Which source wins
    // when several resolve is a rendered fact (the image goldens pin it); what is device-free is
    // the fall-through: an *unresolved* higher-ranked source leaves the next one painting, so a
    // texture that failed to load shows the flat color rather than a hole.
    Document document;

    Style style;
    style.Width = Length::Points(40.0f);
    style.Height = Length::Points(20.0f);
    style.Background = vec4(0.2f, 0.4f, 0.8f, 1.0f);
    // Every higher-ranked source named but none resolved: an empty material handle, a gradient
    // whose ramp never loaded, and an empty texture handle.
    style.BackgroundGradient = ResolvedGradient{
        .Kind = GradientKind::Linear, .P0 = vec2(0.0f, -1.0f), .P1 = vec2(0.0f, 1.0f)};
    Element& panel = document.Add(document.Root(), ElementKind::Panel);
    document.SetStyle(panel, style);
    document.Solve(vec2(100.0f, 100.0f));

    DrawList list;
    document.Build(list);

    // One flat quad, carrying the color and selecting no gradient record.
    REQUIRE(list.GetVertices().size() == 4);
    CHECK(list.GetGradients().empty());
    CHECK(list.GetVertices()[0].GradientSelector == 0);
    CHECK(list.GetVertices()[0].Color.b > list.GetVertices()[0].Color.r);

    // A transparent color under the same unresolved sources paints nothing at all — the fall-through
    // ends at the color rather than substituting a default fill.
    style.Background = vec4(0.0f);
    document.SetStyle(panel, style);
    DrawList empty;
    document.Build(empty);
    CHECK(empty.IsEmpty());
}
