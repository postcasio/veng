// Gui draw-list run partitioning: N primitives resolve to the expected run count and
// clip nesting, keyed by {pipeline, clip, texture}. Device-free — the draw list is pure
// CPU data, so this needs no GPU context. Text is exercised by the GPU golden (it needs a
// resident font atlas); this pins the geometry/run bookkeeping the pass replays.

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
