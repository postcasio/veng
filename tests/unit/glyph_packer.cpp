// Device-free glyph-packer cases. Drives GlyphPacker directly — no graphics device — over the
// residency policy GlyphAtlas is the GPU wrapper for: rectangle packing places non-overlapping
// rects and rolls to a new page when one fills; eviction frees a rect a later pack reuses; the
// this-frame pin keeps a glyph ensured this frame off the eviction list; and a frame whose working
// set exceeds the page cap reports not-resident rather than dropping a pinned glyph.

#include <doctest/doctest.h>

#include "Text/GlyphPacker.h"

#include <vector>

using namespace Veng;
using namespace Veng::Text;

namespace
{
    GlyphKey MakeKey(u32 glyphIndex, GlyphFieldType field = GlyphFieldType::Msdf, u32 size = 48)
    {
        return GlyphKey{
            .Face = static_cast<FaceId>(0),
            .GlyphIndex = glyphIndex,
            .QuantizedPixelSize = size,
            .FieldType = field,
        };
    }

    GlyphPacker::Metrics Metrics(u32 w, u32 h)
    {
        return GlyphPacker::Metrics{
            .Size = {w, h},
            .PlaneMin = {0.0f, 0.0f},
            .PlaneMax = {1.0f, 1.0f},
            .Advance = 0.5f,
        };
    }

    // True if two placed glyphs' texel rectangles overlap.
    bool Overlaps(const GlyphPacker::Slot& a, const GlyphPacker::Slot& b)
    {
        const bool disjointX =
            a.Offset.x + a.Size.x <= b.Offset.x || b.Offset.x + b.Size.x <= a.Offset.x;
        const bool disjointY =
            a.Offset.y + a.Size.y <= b.Offset.y || b.Offset.y + b.Size.y <= a.Offset.y;
        return !(disjointX || disjointY);
    }
}

TEST_CASE("glyph packer: places non-overlapping rects within the page")
{
    GlyphPacker packer(GlyphPacker::Config{.PageSize = 64, .Gutter = 1, .MaxPagesPerFieldType = 1});
    packer.BeginFrame(1);

    std::vector<GlyphPacker::Slot> slots;
    // A spread of sizes so the guillotine split is exercised, all fitting one 64x64 page.
    const std::pair<u32, u32> sizes[] = {{20, 18}, {12, 24}, {18, 12}, {10, 10}, {22, 8}, {8, 20}};
    for (u32 i = 0; i < 6; i++)
    {
        const GlyphPacker::Result result =
            packer.Insert(MakeKey(i), Metrics(sizes[i].first, sizes[i].second));
        REQUIRE(result.Outcome == GlyphPacker::Outcome::Packed);
        // Each glyph lands within the page and at the size asked for.
        CHECK(result.Slot.Offset.x + result.Slot.Size.x <= 64);
        CHECK(result.Slot.Offset.y + result.Slot.Size.y <= 64);
        CHECK(result.Slot.Size.x == sizes[i].first);
        CHECK(result.Slot.Size.y == sizes[i].second);
        slots.push_back(result.Slot);
    }

    // No two glyphs share a texel.
    for (usize i = 0; i < slots.size(); i++)
    {
        for (usize j = i + 1; j < slots.size(); j++)
        {
            CAPTURE(i);
            CAPTURE(j);
            CHECK_FALSE(Overlaps(slots[i], slots[j]));
        }
    }
    CHECK(packer.PageCount() == 1);
}

TEST_CASE("glyph packer: rolls onto a new page when the first fills")
{
    // A 32x32 page holds exactly four 15x15 (16x16 with gutter) glyphs.
    GlyphPacker packer(GlyphPacker::Config{.PageSize = 32, .Gutter = 1, .MaxPagesPerFieldType = 2});
    packer.BeginFrame(1);

    for (u32 i = 0; i < 4; i++)
    {
        const GlyphPacker::Result result = packer.Insert(MakeKey(i), Metrics(15, 15));
        REQUIRE(result.Outcome == GlyphPacker::Outcome::Packed);
        // Only the first glyph allocates page 0; the rest fit alongside it.
        CHECK(result.NewPage == (i == 0));
    }
    CHECK(packer.PageCount() == 1);

    // The fifth glyph does not fit page 0, so a second page is allocated.
    const GlyphPacker::Result fifth = packer.Insert(MakeKey(4), Metrics(15, 15));
    REQUIRE(fifth.Outcome == GlyphPacker::Outcome::Packed);
    CHECK(fifth.NewPage);
    CHECK(fifth.NewPageIndex == 1);
    CHECK(fifth.Slot.Page == 1);
    CHECK(packer.PageCount() == 2);
}

TEST_CASE("glyph packer: eviction frees a rect a later pack reuses")
{
    // One page, so a full page must evict rather than roll to a second.
    GlyphPacker packer(GlyphPacker::Config{.PageSize = 32, .Gutter = 1, .MaxPagesPerFieldType = 1});

    // Fill the page one glyph per frame, so each carries a distinct LRU stamp.
    for (u32 i = 0; i < 4; i++)
    {
        packer.BeginFrame(i + 1);
        REQUIRE(packer.Insert(MakeKey(i), Metrics(15, 15)).Outcome == GlyphPacker::Outcome::Packed);
    }
    CHECK(packer.ResidentCount() == 4);

    // A later frame: the fifth glyph evicts the least-recently-used (glyph 0, frame 1) and reuses
    // its freed rectangle on the same page.
    packer.BeginFrame(5);
    const GlyphPacker::Result fifth = packer.Insert(MakeKey(4), Metrics(15, 15));
    REQUIRE(fifth.Outcome == GlyphPacker::Outcome::Packed);
    CHECK_FALSE(fifth.NewPage);
    CHECK(packer.PageCount() == 1);
    REQUIRE(fifth.Evicted.size() == 1);
    CHECK(fifth.Evicted[0] == MakeKey(0));

    // The evicted glyph is gone; the newcomer is resident; the population is unchanged.
    CHECK_FALSE(packer.Contains(MakeKey(0)));
    CHECK(packer.Contains(MakeKey(4)));
    CHECK(packer.ResidentCount() == 4);
}

TEST_CASE("glyph packer: a this-frame glyph is pinned against eviction")
{
    GlyphPacker packer(GlyphPacker::Config{.PageSize = 32, .Gutter = 1, .MaxPagesPerFieldType = 1});

    // Fill the page across four frames, so glyph 0 is the coldest.
    for (u32 i = 0; i < 4; i++)
    {
        packer.BeginFrame(i + 1);
        REQUIRE(packer.Insert(MakeKey(i), Metrics(15, 15)).Outcome == GlyphPacker::Outcome::Packed);
    }

    // In a new frame, touch the coldest glyph so it is pinned this frame; the eviction must then
    // pass it over and drop one of the others.
    packer.BeginFrame(5);
    REQUIRE(packer.Touch(MakeKey(0)).has_value());
    const GlyphPacker::Result fifth = packer.Insert(MakeKey(4), Metrics(15, 15));
    REQUIRE(fifth.Outcome == GlyphPacker::Outcome::Packed);
    REQUIRE(fifth.Evicted.size() == 1);
    CHECK(fifth.Evicted[0] != MakeKey(0));
    CHECK(packer.Contains(MakeKey(0)));
    CHECK(packer.Contains(MakeKey(4)));
}

TEST_CASE("glyph packer: an over-cap frame reports not-resident, keeping pinned glyphs")
{
    GlyphPacker packer(GlyphPacker::Config{.PageSize = 32, .Gutter = 1, .MaxPagesPerFieldType = 1});

    // Fill the whole cap within one frame, so every resident glyph is pinned this frame.
    packer.BeginFrame(1);
    for (u32 i = 0; i < 4; i++)
    {
        REQUIRE(packer.Insert(MakeKey(i), Metrics(15, 15)).Outcome == GlyphPacker::Outcome::Packed);
    }

    // A fifth distinct glyph the same frame cannot evict a pinned slot: not resident, nothing dropped.
    const GlyphPacker::Result overflow = packer.Insert(MakeKey(4), Metrics(15, 15));
    CHECK(overflow.Outcome == GlyphPacker::Outcome::NotResident);
    CHECK(overflow.Evicted.empty());
    for (u32 i = 0; i < 4; i++)
    {
        CHECK(packer.Contains(MakeKey(i)));
    }
    CHECK_FALSE(packer.Contains(MakeKey(4)));

    // Next frame the earlier glyphs unpin, so the same glyph now packs by evicting one.
    packer.BeginFrame(2);
    const GlyphPacker::Result retried = packer.Insert(MakeKey(4), Metrics(15, 15));
    CHECK(retried.Outcome == GlyphPacker::Outcome::Packed);
    CHECK(packer.Contains(MakeKey(4)));
}

TEST_CASE("glyph packer: field types never share a page")
{
    GlyphPacker packer(GlyphPacker::Config{.PageSize = 64, .Gutter = 1, .MaxPagesPerFieldType = 4});
    packer.BeginFrame(1);

    const GlyphPacker::Result msdf =
        packer.Insert(MakeKey(1, GlyphFieldType::Msdf), Metrics(20, 20));
    const GlyphPacker::Result sdf = packer.Insert(MakeKey(2, GlyphFieldType::Sdf), Metrics(20, 20));
    REQUIRE(msdf.Outcome == GlyphPacker::Outcome::Packed);
    REQUIRE(sdf.Outcome == GlyphPacker::Outcome::Packed);

    // The SDF glyph could not go on the MSDF glyph's page, so a second page was allocated for it.
    CHECK(msdf.Slot.Page != sdf.Slot.Page);
    CHECK(packer.PageFieldType(msdf.Slot.Page) == GlyphFieldType::Msdf);
    CHECK(packer.PageFieldType(sdf.Slot.Page) == GlyphFieldType::Sdf);
    CHECK(packer.PageCount() == 2);
}

TEST_CASE("glyph packer: a whitespace glyph is resident without a page rectangle")
{
    GlyphPacker packer(GlyphPacker::Config{.PageSize = 64, .Gutter = 1, .MaxPagesPerFieldType = 1});
    packer.BeginFrame(1);

    const GlyphPacker::Result space = packer.Insert(MakeKey(3), Metrics(0, 0));
    REQUIRE(space.Outcome == GlyphPacker::Outcome::Packed);
    CHECK(space.Slot.Empty);
    CHECK_FALSE(space.NewPage);
    // A whitespace glyph consumes no page, so none is allocated for it alone.
    CHECK(packer.PageCount() == 0);
    CHECK(packer.Contains(MakeKey(3)));
}
