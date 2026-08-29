// Half-res translucent layer unit cases: the extent halving every consumer of the layer
// derives through, and the plan merge that folds the layer's draws back into the
// full-resolution plan on a frame the wired passes cannot take them. Pure data → data; the
// material and mesh pointers are never dereferenced, only carried.

#include <doctest/doctest.h>

#include "Renderer/DrawGather.h"
#include "Renderer/HalfResTranslucency.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    TranslucentDraw Draw(const u32 candidateId, const f32 viewDepth, const i32 sortPriority)
    {
        return TranslucentDraw{
            .Material = nullptr,
            .SourceMesh = nullptr,
            .IndexCount = 3,
            .FirstIndex = 0,
            .CandidateId = candidateId,
            .ViewDepth = viewDepth,
            .SortPriority = sortPriority,
        };
    }
}

TEST_CASE("HalfResExtent rounds up and never reaches zero")
{
    CHECK(HalfResExtent({1920, 1080}) == uvec2{960, 540});
    // Odd extents keep a texel covering the last row/column.
    CHECK(HalfResExtent({1921, 1081}) == uvec2{961, 541});
    CHECK(HalfResExtent({1, 1}) == uvec2{1, 1});
    CHECK(HalfResExtent({0, 0}) == uvec2{1, 1});
}

TEST_CASE("MergeTranslucentPlans restores one back-to-front order and empties the source")
{
    TranslucentDrawPlan into;
    into.Draws = {Draw(0, -10.0f, 0), Draw(1, -2.0f, 0), Draw(2, -5.0f, 1)};
    TranslucentDrawPlan from;
    from.Draws = {Draw(3, -7.0f, 0), Draw(4, -1.0f, 1)};

    MergeTranslucentPlans(into, from);

    CHECK(from.Draws.empty());
    REQUIRE(into.Draws.size() == 5);
    // Ascending priority groups, most negative view depth first within each — the same order
    // the gather itself produces, so a merged frame draws exactly as an unrouted one would.
    for (usize i = 1; i < into.Draws.size(); i++)
    {
        const TranslucentDraw& a = into.Draws[i - 1];
        const TranslucentDraw& b = into.Draws[i];
        const bool ordered = a.SortPriority < b.SortPriority ||
                             (a.SortPriority == b.SortPriority && a.ViewDepth <= b.ViewDepth);
        CHECK(ordered);
    }
    // Every draw from both plans survives the merge.
    u32 seen = 0;
    for (const TranslucentDraw& draw : into.Draws)
    {
        seen |= 1u << draw.CandidateId;
    }
    CHECK(seen == 0b11111u);
}

TEST_CASE("MergeTranslucentPlans with an empty source leaves the target untouched")
{
    TranslucentDrawPlan into;
    into.Draws = {Draw(0, -2.0f, 0), Draw(1, -1.0f, 0)};
    TranslucentDrawPlan from;

    MergeTranslucentPlans(into, from);

    REQUIRE(into.Draws.size() == 2);
    CHECK(into.Draws[0].CandidateId == 0);
    CHECK(into.Draws[1].CandidateId == 1);
}
