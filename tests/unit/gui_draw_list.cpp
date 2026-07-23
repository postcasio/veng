// Gui draw-list run partitioning: N primitives resolve to the expected run count and
// clip nesting, keyed by {pipeline, clip, texture}. Device-free — the draw list is pure
// CPU data, so this needs no GPU context. Text is exercised by the GPU golden (it needs a
// resident font atlas); this pins the geometry/run bookkeeping the pass replays.

#include <cmath>

#include <doctest/doctest.h>

#include <Veng/Gui/DrawList.h>

using namespace Veng;
using namespace Veng::Gui;

namespace
{
    Renderer::TextureHandle Texture(u32 index)
    {
        return Renderer::TextureHandle{.Index = index};
    }
    Renderer::SamplerHandle Sampler(u32 index)
    {
        return Renderer::SamplerHandle{.Index = index};
    }

    constexpr Rect UnitRect{.Min = {10.0f, 10.0f}, .Size = {40.0f, 40.0f}};

    // Rotates a point about a pivot, clockwise-positive in the y-down space (the transform-stack
    // convention). The reference the vertex-position cases compare the draw list's output against.
    vec2 RotateAbout(vec2 point, vec2 pivot, f32 radians)
    {
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        const vec2 d = point - pivot;
        return pivot + vec2(c * d.x - s * d.y, s * d.x + c * d.y);
    }

    void CheckVec2(vec2 actual, vec2 expected)
    {
        CHECK(actual.x == doctest::Approx(expected.x));
        CHECK(actual.y == doctest::Approx(expected.y));
    }
}

TEST_CASE("gui draw list: consecutive shapes with matching key merge into one run")
{
    DrawList list;
    list.Quad(UnitRect, vec4(1.0f));
    list.Quad({.Min = {60.0f, 10.0f}, .Size = {40.0f, 40.0f}}, vec4(0.5f));
    list.Quad({.Min = {110.0f, 10.0f}, .Size = {40.0f, 40.0f}}, vec4(0.25f), CornerRadii::All(6.0f),
              Border{.Width = 2.0f, .Color = vec4(1.0f)});

    // Three untextured shapes, same pipeline and clip → one run of three quads.
    REQUIRE(list.GetRuns().size() == 1);
    CHECK(list.GetRuns()[0].Pipeline == GuiPipeline::Shape);
    CHECK(list.GetRuns()[0].IndexCount == 18);
    CHECK(list.GetVertices().size() == 12);
    CHECK(list.GetIndices().size() == 18);
    CHECK_FALSE(list.GetRuns()[0].HasClip);
}

TEST_CASE("gui draw list: a gradient appends a record and selects it by index from the vertex")
{
    DrawList list;
    const GradientFill fill{.Kind = GradientKind::Radial,
                            .P0 = vec2(0.25f, -0.5f),
                            .P1 = vec2(0.8f, 0.6f),
                            .AngleOffset = 0.0f,
                            .Ramp = Texture(5),
                            .Sampler = Sampler(2)};
    list.Gradient(UnitRect, fill, CornerRadii::All(6.0f));

    REQUIRE(list.GetRuns().size() == 1);
    CHECK(list.GetRuns()[0].Pipeline == GuiPipeline::Shape);
    REQUIRE(list.GetVertices().size() == 4);
    // The corner radius rides Params.x; the fill is untextured (the ramp lives in the record), and
    // the vertex selects the record by index-plus-one.
    CHECK(list.GetVertices()[0].Params.x == doctest::Approx(6.0f));
    CHECK(list.GetVertices()[0].GradientSelector == 1);

    // The record carries the shape, geometry, and ramp/sampler slots.
    REQUIRE(list.GetGradients().size() == 1);
    const GpuGradient& record = list.GetGradients()[0];
    CHECK(record.Kind == static_cast<u32>(GradientKind::Radial));
    CHECK(record.RampTexture == 5);
    CHECK(record.RampSampler == 2);
    CHECK(record.P0.x == doctest::Approx(0.25f));
    CHECK(record.P1.y == doctest::Approx(0.6f));

    // A second gradient — even with a different ramp — merges into one run (the ramp rides the
    // record, so no texture keys the run); the second vertex selects record two.
    GradientFill other = fill;
    other.Ramp = Texture(9);
    list.Gradient({.Min = {60.0f, 10.0f}, .Size = {40.0f, 40.0f}}, other);
    CHECK(list.GetRuns().size() == 1);
    CHECK(list.GetGradients().size() == 2);
    CHECK(list.GetVertices()[4].GradientSelector == 2);
    CHECK(list.GetGradients()[1].RampTexture == 9);

    // A plain solid quad leaves the selector zero, so the fragment takes the non-gradient path.
    DrawList solid;
    solid.Quad(UnitRect, vec4(1.0f));
    CHECK(solid.GetVertices()[0].GradientSelector == 0);
    CHECK(solid.GetGradients().empty());
}

TEST_CASE("gui draw list: a distinct texture opens a distinct run")
{
    DrawList list;
    list.Quad(UnitRect, vec4(1.0f));                // untextured shape
    list.Texture(UnitRect, Texture(3), Sampler(0)); // texture 3
    list.Texture(UnitRect, Texture(3), Sampler(0)); // texture 3 again — merges
    list.Texture(UnitRect, Texture(7), Sampler(0)); // texture 7 — new run

    // untextured | tex3 (two quads) | tex7 → three runs.
    REQUIRE(list.GetRuns().size() == 3);
    CHECK(list.GetRuns()[0].IndexCount == 6);
    CHECK(list.GetRuns()[1].IndexCount == 12);
    CHECK(list.GetRuns()[2].IndexCount == 6);
    for (const DrawRun& run : list.GetRuns())
    {
        CHECK(run.Pipeline == GuiPipeline::Shape);
    }
}

TEST_CASE("gui draw list: push/pop clip nests and intersects, opening runs at boundaries")
{
    DrawList list;
    list.Quad(UnitRect, vec4(1.0f)); // unclipped

    list.PushClip({.Min = {0.0f, 0.0f}, .Size = {100.0f, 100.0f}});
    list.Quad(UnitRect, vec4(1.0f)); // clipped to (0,0,100,100)

    list.PushClip({.Min = {50.0f, 50.0f}, .Size = {200.0f, 200.0f}});
    list.Quad(UnitRect, vec4(1.0f)); // clipped to the intersection (50,50,50,50)
    list.PopClip();

    list.Quad(UnitRect, vec4(1.0f)); // back to (0,0,100,100) — a new run at the clip change
    list.PopClip();

    list.Quad(UnitRect, vec4(1.0f)); // unclipped again — a new run

    // unclipped | clip A | clip A∩B | clip A | unclipped → five runs (each clip change is a boundary).
    REQUIRE(list.GetRuns().size() == 5);

    CHECK_FALSE(list.GetRuns()[0].HasClip);

    CHECK(list.GetRuns()[1].HasClip);
    CHECK(list.GetRuns()[1].Clip.Min == vec2(0.0f, 0.0f));
    CHECK(list.GetRuns()[1].Clip.Size == vec2(100.0f, 100.0f));

    // The nested clip is the intersection of (0,0,100,100) and (50,50,200,200).
    CHECK(list.GetRuns()[2].HasClip);
    CHECK(list.GetRuns()[2].Clip.Min == vec2(50.0f, 50.0f));
    CHECK(list.GetRuns()[2].Clip.Size == vec2(50.0f, 50.0f));

    CHECK(list.GetRuns()[3].HasClip);
    CHECK(list.GetRuns()[3].Clip.Min == vec2(0.0f, 0.0f));

    CHECK_FALSE(list.GetRuns()[4].HasClip);
}

TEST_CASE("gui draw list: a nine-slice emits nine textured tiles in one run")
{
    DrawList list;
    list.NineSlice({.Min = {0.0f, 0.0f}, .Size = {128.0f, 128.0f}}, Texture(2), Sampler(0),
                   Insets::All(0.25f), Insets::All(16.0f));

    // Nine tiles, one shared texture and clip → one run of nine quads.
    REQUIRE(list.GetRuns().size() == 1);
    CHECK(list.GetRuns()[0].Pipeline == GuiPipeline::Shape);
    CHECK(list.GetRuns()[0].IndexCount == 9 * 6);
    CHECK(list.GetVertices().size() == 9 * 4);

    // Stretching leaves the wrap lane inactive on every cell, which is what makes the fragment
    // take the path it took before the lane existed.
    for (const GuiVertex& vertex : list.GetVertices())
    {
        CHECK(vertex.UvWrap == vec4(0.0f));
    }
}

TEST_CASE("gui draw list: a tiled nine-slice wraps each cell within its own source sub-rect")
{
    // A 64×64 source sliced at 16px: each cell is 16 source pixels on its fixed axis and 32 on its
    // growing one. The destination is 128×128, so the centre grows 96 against 32 source pixels and
    // each edge grows the same on its one axis — 3 repeats, and 1 on the axis it does not grow on.
    DrawList list;
    list.NineSlice({.Min = {0.0f, 0.0f}, .Size = {128.0f, 128.0f}}, Texture(2), Sampler(0),
                   Insets::All(0.25f), Insets::All(16.0f), vec4(1.0f),
                   {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}}, ImageRepeat::Tile,
                   vec2(64.0f, 64.0f));

    REQUIRE(list.GetVertices().size() == 9 * 4);

    // Cells are emitted row-major, so quad index (row * 3 + col) is that cell of the 3×3 grid.
    const auto cell = [&](usize row, usize col) -> const GuiVertex&
    { return list.GetVertices()[(row * 3 + col) * 4]; };

    // The four corners are fixed-size by definition and must take the untouched path.
    for (const usize row : {usize{0}, usize{2}})
    {
        for (const usize col : {usize{0}, usize{2}})
        {
            CHECK(cell(row, col).UvWrap == vec4(0.0f));
        }
    }

    // The contract the fragment reads: a tiled cell's wrap lane *is* that cell's source sub-rect.
    CHECK(cell(1, 1).UvWrap == vec4(0.25f, 0.25f, 0.5f, 0.5f));  // centre
    CHECK(cell(0, 1).UvWrap == vec4(0.25f, 0.0f, 0.5f, 0.25f));  // top edge
    CHECK(cell(2, 1).UvWrap == vec4(0.25f, 0.75f, 0.5f, 0.25f)); // bottom edge
    CHECK(cell(1, 0).UvWrap == vec4(0.0f, 0.25f, 0.25f, 0.5f));  // left edge
    CHECK(cell(1, 2).UvWrap == vec4(0.75f, 0.25f, 0.25f, 0.5f)); // right edge

    // An edge spans repeats of its sub-rect along its growing axis only: the top edge's quad UV
    // covers three copies horizontally and exactly one vertically.
    const GuiVertex& topLeftOfTopEdge = cell(0, 1);
    const GuiVertex& bottomRightOfTopEdge = list.GetVertices()[(0 * 3 + 1) * 4 + 2];
    CHECK(topLeftOfTopEdge.Uv == vec2(0.25f, 0.0f));
    CHECK(bottomRightOfTopEdge.Uv == vec2(0.25f + 3.0f * 0.5f, 0.25f));

    // And the centre repeats on both axes.
    const GuiVertex& bottomRightOfCentre = list.GetVertices()[(1 * 3 + 1) * 4 + 2];
    CHECK(bottomRightOfCentre.Uv == vec2(0.25f + 3.0f * 0.5f, 0.25f + 3.0f * 0.5f));
}

TEST_CASE("gui draw list: Clear resets geometry, runs, and the clip stack")
{
    DrawList list;
    list.PushClip({.Min = {0.0f, 0.0f}, .Size = {10.0f, 10.0f}});
    list.Quad(UnitRect, vec4(1.0f));
    REQUIRE_FALSE(list.IsEmpty());

    list.Clear();
    CHECK(list.IsEmpty());
    CHECK(list.GetVertices().empty());
    CHECK(list.GetIndices().empty());
    CHECK(list.GetRuns().empty());

    // The clip stack cleared too: a fresh quad is unclipped.
    list.Quad(UnitRect, vec4(1.0f));
    REQUIRE(list.GetRuns().size() == 1);
    CHECK_FALSE(list.GetRuns()[0].HasClip);
}

TEST_CASE("gui draw list: an empty rect emits nothing")
{
    DrawList list;
    list.Quad({.Min = {0.0f, 0.0f}, .Size = {0.0f, 20.0f}}, vec4(1.0f));
    list.Texture({.Min = {0.0f, 0.0f}, .Size = {20.0f, 0.0f}}, Texture(1), Sampler(0));
    CHECK(list.IsEmpty());
}

TEST_CASE("gui draw list: a transform rotates vertex positions but leaves the shape SDF space")
{
    const Rect rect{.Min = {20.0f, 30.0f}, .Size = {60.0f, 40.0f}};
    const vec2 pivot = rect.Center();
    const f32 angle = glm::radians(90.0f);

    // The same bordered rounded quad, once unrotated and once under a 90° transform about its center.
    DrawList plain;
    plain.Quad(rect, vec4(0.3f, 0.4f, 0.5f, 1.0f), CornerRadii::All(8.0f),
               Border{.Width = 2.0f, .Color = vec4(1.0f)});

    DrawList rotated;
    rotated.PushTransform(pivot, angle);
    rotated.Quad(rect, vec4(0.3f, 0.4f, 0.5f, 1.0f), CornerRadii::All(8.0f),
                 Border{.Width = 2.0f, .Color = vec4(1.0f)});
    rotated.PopTransform();

    REQUIRE(plain.GetVertices().size() == 4);
    REQUIRE(rotated.GetVertices().size() == 4);

    for (usize i = 0; i < 4; ++i)
    {
        const GuiVertex& before = plain.GetVertices()[i];
        const GuiVertex& after = rotated.GetVertices()[i];

        // The position is the rotation of the unrotated corner about the pivot.
        CheckVec2(after.Position, RotateAbout(before.Position, pivot, angle));

        // The SDF's local box space, the UV, the color, and the packed params are untouched — that
        // is what makes the rounded corners, border, and any texture rotate rigidly.
        CheckVec2(after.RectHalf, before.RectHalf);
        CheckVec2(after.RectCoord, before.RectCoord);
        CheckVec2(after.Uv, before.Uv);
        CHECK(after.Params.x == doctest::Approx(before.Params.x)); // corner radius
        CHECK(after.Params.y == doctest::Approx(before.Params.y)); // border width
    }
}

TEST_CASE("gui draw list: nested transforms compose inner-within-outer")
{
    const Rect rect{.Min = {0.0f, 0.0f}, .Size = {20.0f, 20.0f}};
    const vec2 outerPivot{100.0f, 100.0f};
    const vec2 innerPivot{10.0f, 10.0f};
    const f32 outer = glm::radians(30.0f);
    const f32 inner = glm::radians(45.0f);

    DrawList list;
    list.PushTransform(outerPivot, outer);
    list.PushTransform(innerPivot, inner);
    list.Quad(rect, vec4(1.0f));
    list.PopTransform();
    list.PopTransform();

    DrawList reference;
    reference.Quad(rect, vec4(1.0f));

    REQUIRE(list.GetVertices().size() == 4);
    for (usize i = 0; i < 4; ++i)
    {
        // The inner push turns within the outer frame: a corner maps through the inner rotation
        // first, then the outer one.
        const vec2 corner = reference.GetVertices()[i].Position;
        const vec2 expected =
            RotateAbout(RotateAbout(corner, innerPivot, inner), outerPivot, outer);
        CheckVec2(list.GetVertices()[i].Position, expected);
    }
}

TEST_CASE("gui draw list: popping the transform stack restores the enclosing frame")
{
    const Rect rect{.Min = {0.0f, 0.0f}, .Size = {20.0f, 20.0f}};

    DrawList list;
    list.PushTransform(vec2(50.0f, 50.0f), glm::radians(90.0f));
    list.Quad(rect, vec4(1.0f)); // rotated
    list.PopTransform();
    list.Quad({.Min = {40.0f, 40.0f}, .Size = {20.0f, 20.0f}}, vec4(1.0f)); // identity again

    DrawList reference;
    reference.Quad({.Min = {40.0f, 40.0f}, .Size = {20.0f, 20.0f}}, vec4(1.0f));

    REQUIRE(list.GetVertices().size() == 8);
    for (usize i = 0; i < 4; ++i)
    {
        // The four vertices emitted after the pop match the unrotated reference exactly.
        CheckVec2(list.GetVertices()[4 + i].Position, reference.GetVertices()[i].Position);
    }
}

TEST_CASE("gui draw list: Clear resets the transform stack")
{
    DrawList list;
    list.PushTransform(vec2(50.0f, 50.0f), glm::radians(90.0f));
    list.Clear();

    // A fresh quad after Clear is unrotated — the transform stack cleared with everything else.
    list.Quad(UnitRect, vec4(1.0f));
    DrawList reference;
    reference.Quad(UnitRect, vec4(1.0f));
    REQUIRE(list.GetVertices().size() == 4);
    for (usize i = 0; i < 4; ++i)
    {
        CheckVec2(list.GetVertices()[i].Position, reference.GetVertices()[i].Position);
    }
}

TEST_CASE("gui draw list: an outer shadow expands its quad and keeps the element's SDF box")
{
    DrawList list;
    const BoxShadow shadow{.Offset = vec2(4.0f, 6.0f),
                           .Blur = 8.0f,
                           .Spread = 2.0f,
                           .Color = vec4(0.0f, 0.0f, 0.0f, 0.5f)};
    list.Shadow(UnitRect, shadow, CornerRadii::All(6.0f));

    REQUIRE(list.GetVertices().size() == 4);
    // The quad grows by spread + blur on every side and slides by the offset, so the softened
    // silhouette has fragments outside the element box to shade.
    CheckVec2(list.GetVertices()[0].Position, UnitRect.Min + shadow.Offset - vec2(10.0f));
    CheckVec2(list.GetVertices()[2].Position, UnitRect.Max() + shadow.Offset + vec2(10.0f));

    // The SDF box stays the *element's*: the fragment displaces and grows it from the shadow field.
    for (const GuiVertex& vertex : list.GetVertices())
    {
        CheckVec2(vertex.RectHalf, UnitRect.Size * 0.5f);
        CHECK(vertex.Shadow.x == doctest::Approx(8.0f));
        CHECK(vertex.Shadow.y == doctest::Approx(2.0f));
        CHECK(vertex.Shadow.z == doctest::Approx(4.0f));
        CHECK(vertex.Shadow.w == doctest::Approx(6.0f));
    }

    // Untextured and gradient-free, so it merges with the quads around it into one run.
    list.Quad(UnitRect, vec4(1.0f));
    CHECK(list.GetRuns().size() == 1);
}

TEST_CASE("gui draw list: an inset shadow keeps the element box as its quad and signs its blur")
{
    DrawList list;
    const BoxShadow shadow{
        .Offset = vec2(3.0f), .Blur = 5.0f, .Spread = 1.0f, .Color = vec4(1.0f), .Inset = true};
    list.Shadow(UnitRect, shadow);

    REQUIRE(list.GetVertices().size() == 4);
    // An inset shadow is bounded by the box it recesses, so the quad is exactly that box.
    CheckVec2(list.GetVertices()[0].Position, UnitRect.Min);
    CheckVec2(list.GetVertices()[2].Position, UnitRect.Max());
    // The sign of the blur lane is the inset flag's only transport.
    CHECK(list.GetVertices()[0].Shadow.x == doctest::Approx(-5.0f));
}

TEST_CASE("gui draw list: a zero-spread shadow grows by the blur alone")
{
    // Spread and blur both widen the quad, so a spread of zero isolates the blur's contribution:
    // an off-by-Blur here is invisible except as a shadow ramp clipped at the quad's edge.
    DrawList list;
    const BoxShadow shadow{
        .Offset = vec2(0.0f), .Blur = 7.0f, .Spread = 0.0f, .Color = vec4(0.0f, 0.0f, 0.0f, 0.6f)};
    list.Shadow(UnitRect, shadow);

    REQUIRE(list.GetVertices().size() == 4);
    CheckVec2(list.GetVertices()[0].Position, UnitRect.Min - vec2(7.0f));
    CheckVec2(list.GetVertices()[2].Position, UnitRect.Max() + vec2(7.0f));
    CHECK(list.GetVertices()[0].Shadow.y == doctest::Approx(0.0f));

    // A negative spread shrinks the silhouette, so the quad grows by less than the blur — but never
    // below the element's own box, which the fragment still needs to reconstruct the SDF.
    DrawList tightened;
    tightened.Shadow(UnitRect, BoxShadow{.Blur = 7.0f, .Spread = -3.0f, .Color = vec4(1.0f)});
    REQUIRE(tightened.GetVertices().size() == 4);
    CheckVec2(tightened.GetVertices()[0].Position, UnitRect.Min - vec2(4.0f));
    CHECK(tightened.GetVertices()[0].Shadow.y == doctest::Approx(-3.0f));
}

TEST_CASE("gui draw list: an inset shadow's quad ignores blur, spread, and offset alike")
{
    // An inset shadow is bounded by the box it recesses, so none of the three widening terms may
    // reach the geometry — they are all evaluated in the fragment against the element's own SDF.
    DrawList list;
    list.Shadow(UnitRect, BoxShadow{.Offset = vec2(9.0f, -4.0f),
                                    .Blur = 12.0f,
                                    .Spread = 5.0f,
                                    .Color = vec4(1.0f),
                                    .Inset = true});

    REQUIRE(list.GetVertices().size() == 4);
    CheckVec2(list.GetVertices()[0].Position, UnitRect.Min);
    CheckVec2(list.GetVertices()[2].Position, UnitRect.Max());
    for (const GuiVertex& vertex : list.GetVertices())
    {
        CheckVec2(vertex.RectHalf, UnitRect.Size * 0.5f);
        // The blur's magnitude is the ramp and its sign the inset flag; spread and offset ride
        // unsigned, so an inset spread is read as inward growth by the fragment, not here.
        CHECK(vertex.Shadow.x == doctest::Approx(-12.0f));
        CHECK(vertex.Shadow.y == doctest::Approx(5.0f));
        CheckVec2(vec2(vertex.Shadow.z, vertex.Shadow.w), vec2(9.0f, -4.0f));
    }
}

TEST_CASE("gui draw list: a hard shadow still carries a non-zero blur lane")
{
    DrawList list;
    list.Shadow(UnitRect, BoxShadow{.Blur = 0.0f, .Color = vec4(1.0f)});
    REQUIRE(list.GetVertices().size() == 4);
    // A zero lane means "not a shadow" to the fragment, so a hard edge transports a tiny positive
    // blur the fragment widens to the anti-aliasing width.
    CHECK(list.GetVertices()[0].Shadow.x > 0.0f);

    // A fully transparent shadow emits nothing at all.
    DrawList empty;
    empty.Shadow(UnitRect, BoxShadow{.Blur = 4.0f, .Color = vec4(0.0f)});
    CHECK(empty.GetVertices().empty());
}
