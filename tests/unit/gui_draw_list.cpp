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
