// Translucent segmenting unit cases. A frame's translucent draws composite back to front, and a
// run of half-resolution draws composites as one layer, so SegmentTranslucentDraws cuts the sorted
// draws into the order they composite in: a run lands at its place in the order wherever a
// full-resolution draw overlaps it on screen. Pure data → data; the material and mesh pointers are
// never dereferenced, so the cases leave them null.

#include <doctest/doctest.h>

#include <algorithm>
#include <span>

#include "Renderer/DrawGather.h"
#include "Renderer/HalfResTranslucency.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A draw at a view depth (more negative is farther) covering a UV rectangle.
    TranslucentDraw Draw(const u32 id, const f32 viewDepth, const bool half,
                         const vec4 rect = {0.0f, 0.0f, 1.0f, 1.0f}, const i32 priority = 0)
    {
        return TranslucentDraw{
            .Material = nullptr,
            .SourceMesh = nullptr,
            .IndexCount = 3,
            .FirstIndex = 0,
            .CandidateId = id,
            .ViewDepth = viewDepth,
            .SortPriority = priority,
            .ScreenRect = rect,
            .HalfResolution = half,
        };
    }

    // The candidate ids in the order the segments composite them.
    vector<u32> CompositeOrder(const TranslucentSegments& segments)
    {
        vector<u32> order;
        for (u32 s = 0; s <= MaxHalfResSegments; ++s)
        {
            for (const TranslucentDraw& draw : segments.Full[s].Draws)
            {
                order.push_back(draw.CandidateId);
            }
            if (s < MaxHalfResSegments)
            {
                for (const TranslucentDraw& draw : segments.Half[s].Draws)
                {
                    order.push_back(draw.CandidateId);
                }
            }
        }
        return order;
    }

    bool Overlap(const vec4& a, const vec4& b)
    {
        return a.x < b.z && b.x < a.z && a.y < b.w && b.y < a.w;
    }

    struct Sorted
    {
        vector<TranslucentDraw> Full;
        vector<TranslucentDraw> Half;
    };

    Sorted Split(vector<TranslucentDraw> draws)
    {
        Sorted sorted;
        for (const TranslucentDraw& draw : draws)
        {
            (draw.HalfResolution ? sorted.Half : sorted.Full).push_back(draw);
        }
        std::ranges::sort(sorted.Full, TranslucentDrawsBefore);
        std::ranges::sort(sorted.Half, TranslucentDrawsBefore);
        return sorted;
    }

    TranslucentSegments Segment(const vector<TranslucentDraw>& draws)
    {
        const Sorted sorted = Split(draws);
        TranslucentSegments segments;
        SegmentTranslucentDraws(sorted.Full, sorted.Half, vec2(0.0f), segments);
        return segments;
    }

    // Counts the overlapping pairs whose composite order disagrees with the back-to-front order.
    u32 MisorderedOverlappingPairs(const vector<TranslucentDraw>& draws,
                                   const TranslucentSegments& segments)
    {
        vector<TranslucentDraw> sorted = draws;
        std::ranges::sort(sorted, TranslucentDrawsBefore);
        const vector<u32> order = CompositeOrder(segments);
        const auto position = [&](const u32 id)
        { return std::ranges::find(order, id) - order.begin(); };
        u32 misordered = 0;
        for (usize i = 0; i < sorted.size(); ++i)
        {
            for (usize j = i + 1; j < sorted.size(); ++j)
            {
                if (Overlap(sorted[i].ScreenRect, sorted[j].ScreenRect) &&
                    position(sorted[i].CandidateId) > position(sorted[j].CandidateId))
                {
                    ++misordered;
                }
            }
        }
        return misordered;
    }
}

TEST_CASE("translucent segments: the order is priority, then depth, then half before full")
{
    CHECK(TranslucentDrawsBefore(Draw(0, -5.0f, false, {}, 0), Draw(1, -9.0f, false, {}, 1)));
    CHECK(TranslucentDrawsBefore(Draw(0, -9.0f, false), Draw(1, -5.0f, false)));
    // At an equal depth the half-resolution draw goes first, both ways round, so a ring sorted by
    // its planet's centre always draws over that planet's own shell.
    CHECK(TranslucentDrawsBefore(Draw(0, -5.0f, true), Draw(1, -5.0f, false)));
    CHECK_FALSE(TranslucentDrawsBefore(Draw(1, -5.0f, false), Draw(0, -5.0f, true)));
}

TEST_CASE("translucent segments: a near half-res draw composites over a far full-res one")
{
    const TranslucentSegments under = Segment({Draw(0, -2.0f, true), Draw(1, -9.0f, false)});
    CHECK(CompositeOrder(under) == vector<u32>{1, 0});

    const TranslucentSegments over = Segment({Draw(0, -9.0f, true), Draw(1, -2.0f, false)});
    CHECK(CompositeOrder(over) == vector<u32>{0, 1});
    CHECK(over.Half[0].Draws.size() == 1);
    CHECK(over.Full[1].Draws.size() == 1);
}

TEST_CASE("translucent segments: a full-res draw clear of the run before it starts no segment")
{
    const vec4 left{0.0f, 0.0f, 0.4f, 1.0f};
    const vec4 right{0.6f, 0.0f, 1.0f, 1.0f};
    const TranslucentSegments segments =
        Segment({Draw(0, -9.0f, true, left), Draw(1, -2.0f, false, right)});
    CHECK(segments.Full[0].Draws.size() == 1);
    CHECK(segments.Full[1].Draws.empty());
    CHECK(segments.Half[1].Draws.empty());

    // The overlap margin brings the same pair together once it spans the gap between them.
    const Sorted sorted = Split({Draw(0, -9.0f, true, left), Draw(1, -2.0f, false, right)});
    TranslucentSegments widened;
    SegmentTranslucentDraws(sorted.Full, sorted.Half, vec2(0.15f, 0.0f), widened);
    CHECK(widened.Full[1].Draws.size() == 1);
}

TEST_CASE("translucent segments: draws are conserved and overlapping pairs keep their order")
{
    // A stratified field: draws spread across depth and screen, some overlapping and some not, at
    // two priorities, with four half-res draws among them so no run needs folding.
    vector<TranslucentDraw> draws;
    for (u32 i = 0; i < 20; ++i)
    {
        const f32 x = static_cast<f32>((i * 7) % 10) * 0.08f;
        const f32 y = static_cast<f32>((i * 3) % 10) * 0.08f;
        draws.push_back(Draw(i, -static_cast<f32>((i * 11) % 20) - 1.0f, (i % 5) == 0,
                             vec4{x, y, x + 0.3f, y + 0.3f}, i < 16 ? 0 : 1));
    }
    const TranslucentSegments segments = Segment(draws);

    vector<u32> order = CompositeOrder(segments);
    CHECK(order.size() == draws.size());
    std::ranges::sort(order);
    CHECK(std::ranges::adjacent_find(order) == order.end());

    REQUIRE(segments.MergedRuns == 0);
    CHECK(MisorderedOverlappingPairs(draws, segments) == 0);

    // Each run's rectangle bounds every draw in it.
    for (u32 s = 0; s < MaxHalfResSegments; ++s)
    {
        for (const TranslucentDraw& draw : segments.Half[s].Draws)
        {
            const vec4& rect = segments.HalfRect[s];
            CHECK(rect.x <= draw.ScreenRect.x);
            CHECK(rect.y <= draw.ScreenRect.y);
            CHECK(rect.z >= draw.ScreenRect.z);
            CHECK(rect.w >= draw.ScreenRect.w);
        }
    }
}

TEST_CASE("translucent segments: past the cap the runs fold into the last segment")
{
    // Seven alternating runs, all over the whole screen: four segments, and the rest folded.
    vector<TranslucentDraw> draws;
    for (u32 i = 0; i < 14; ++i)
    {
        draws.push_back(Draw(i, -static_cast<f32>(20 - i), (i % 2) == 0));
    }
    const TranslucentSegments segments = Segment(draws);
    for (u32 s = 0; s < MaxHalfResSegments; ++s)
    {
        CHECK_FALSE(segments.Half[s].Draws.empty());
    }
    CHECK(segments.MergedRuns == 3);
    CHECK(CompositeOrder(segments).size() == draws.size());
}

TEST_CASE("translucent segments: the composite's scissor covers its rectangle and the margin")
{
    const uvec2 extent{200, 100};
    const HalfResScissor whole = HalfResCompositeScissor({0.0f, 0.0f, 1.0f, 1.0f}, extent);
    CHECK(whole.Offset == ivec2(0));
    CHECK(whole.Extent == extent);

    const HalfResScissor part = HalfResCompositeScissor({0.25f, 0.5f, 0.5f, 0.75f}, extent);
    const auto margin = static_cast<i32>(HalfResCompositeMarginPixels);
    CHECK(part.Offset == ivec2(50 - margin, 50 - margin));
    CHECK(part.Extent == uvec2(50 + 2 * margin, 25 + 2 * margin));

    // Clamped at the edge, never reaching outside the extent.
    const HalfResScissor edge = HalfResCompositeScissor({0.95f, 0.0f, 1.0f, 0.02f}, extent);
    CHECK(edge.Offset.x + static_cast<i32>(edge.Extent.x) <= static_cast<i32>(extent.x));
    CHECK(edge.Offset.y == 0);
}
