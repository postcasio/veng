// Dynamic glyph-atlas GPU cases. Loads the synthetic kerning fixture face through GlyphSource,
// ensures glyphs into a GlyphAtlas, records the staged uploads into a graphics-queue command
// buffer, and reads a page back to prove the atlas end to end: a miss rasterizes, packs and uploads
// texels that match the rasterizer's own bytes at the slot's rectangle; a re-ensure is a hit that
// re-packs nothing; a tiny page cap forces LRU eviction so a prior frame's glyph gives up its space
// and a re-request re-rasterizes into a fresh slot; and a same-frame request past the cap reports
// not-resident while the pinned glyph stays put. The mid-frame retire safety (an evicted rectangle
// never overwritten while a prior frame's draw could sample it) is a graphics-queue submission-order
// property, invisible to a headless single-queue tier, so it is not asserted here.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Text/GlyphAtlas.h>
#include <Veng/Text/GlyphSource.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Text;

namespace
{
    constexpr u32 MsdfPixelSize = 32;
    constexpr u32 DistanceRange = 4;

    std::vector<std::byte> ReadFixtureFont()
    {
        const std::string path = std::string(GPU_COOKER_FIXTURE_DIR) + "/fonts/VengTestKern.ttf";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        REQUIRE_MESSAGE(file.good(), "failed to open fixture font: " << path);
        const std::streamsize size = file.tellg();
        file.seekg(0);
        std::vector<std::byte> bytes(static_cast<usize>(size));
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        REQUIRE(file.good());
        return bytes;
    }

    // The number of texels of `glyph` that differ from the RGBA8 page `page` at `slot`'s rectangle.
    usize CountRectMismatches(const vector<u8>& page, u32 pageSize, const GlyphSlot& slot,
                              const RasterizedGlyph& glyph)
    {
        const auto ox = static_cast<u32>(std::lround(slot.UvMin.x * static_cast<f32>(pageSize)));
        const auto oy = static_cast<u32>(std::lround(slot.UvMin.y * static_cast<f32>(pageSize)));
        usize mismatches = 0;
        for (u32 y = 0; y < glyph.Height; y++)
        {
            for (u32 x = 0; x < glyph.Width; x++)
            {
                for (u32 c = 0; c < 4; c++)
                {
                    const usize pageByte = (static_cast<usize>(oy + y) * pageSize + ox + x) * 4 + c;
                    const usize glyphByte = (static_cast<usize>(y) * glyph.Width + x) * 4 + c;
                    if (page[pageByte] != glyph.Pixels[glyphByte])
                    {
                        mismatches++;
                    }
                }
            }
        }
        return mismatches;
    }

    // A page just large enough to hold one glyph of the given dimensions, so a second glyph of a
    // similar size cannot fit and the field type's single-page cap forces eviction.
    u32 OneGlyphPageSize(const RasterizedGlyph& a, const RasterizedGlyph& b)
    {
        const u32 span = std::max({a.Width, a.Height, b.Width, b.Height});
        return span + 1; // + the gutter
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "glyph atlas: a miss rasterizes, packs and uploads its texels")
{
    const std::vector<std::byte> font = ReadFixtureFont();
    GlyphSource source;
    const Result<FaceId> face = source.LoadFace(font);
    REQUIRE(face.has_value());
    const u32 glyphA = source.GlyphIndex(*face, 'A');
    REQUIRE(glyphA != 0);

    const Result<RasterizedGlyph> expected = source.Rasterize(
        *face, glyphA, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf, DistanceRange);
    REQUIRE(expected.has_value());
    REQUIRE(expected->Channels == 4);

    const u32 pageSize = std::max(expected->Width, expected->Height) + 8;
    GlyphAtlas atlas(Context, source,
                     GlyphAtlasInfo{.PageSize = pageSize,
                                    .MaxPagesPerFieldType = 1,
                                    .Gutter = 1,
                                    .DistanceRangePx = DistanceRange,
                                    .MsdfPixelSize = MsdfPixelSize});

    atlas.BeginFrame();
    const GlyphKey key =
        atlas.KeyFor(*face, glyphA, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf);
    const GlyphSlot slot = atlas.Ensure(key);
    REQUIRE(slot.Resident);
    REQUIRE_FALSE(slot.Empty);
    REQUIRE(slot.Page.IsValid());
    CHECK(slot.FieldType == GlyphFieldType::Msdf);
    CHECK(atlas.GetPageCount() == 1);

    Context.ImmediateCommands([&](Renderer::CommandBuffer& cmd) { atlas.RecordUploads(cmd); });

    const vector<u8> page = atlas.GetPageView(0)->GetImage()->Download();
    REQUIRE(page.size() == static_cast<usize>(pageSize) * pageSize * 4);
    CHECK(CountRectMismatches(page, pageSize, slot, *expected) == 0);

    // A second ensure of the same glyph is a hit: the same rectangle, nothing new to upload.
    const GlyphSlot hit = atlas.Ensure(key);
    CHECK(hit.Resident);
    CHECK(hit.PageIndex == slot.PageIndex);
    CHECK(hit.UvMin.x == doctest::Approx(slot.UvMin.x));
    CHECK(hit.UvMin.y == doctest::Approx(slot.UvMin.y));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "glyph atlas: a full page evicts a prior frame's glyph and re-packs")
{
    const std::vector<std::byte> font = ReadFixtureFont();
    GlyphSource source;
    const Result<FaceId> face = source.LoadFace(font);
    REQUIRE(face.has_value());
    const u32 glyphA = source.GlyphIndex(*face, 'A');
    const u32 glyphV = source.GlyphIndex(*face, 'V');
    REQUIRE(glyphA != 0);
    REQUIRE(glyphV != 0);

    const Result<RasterizedGlyph> rA = source.Rasterize(
        *face, glyphA, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf, DistanceRange);
    const Result<RasterizedGlyph> rV = source.Rasterize(
        *face, glyphV, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf, DistanceRange);
    REQUIRE(rA.has_value());
    REQUIRE(rV.has_value());

    // One page of the field type, sized so a single glyph fills it — a second must evict.
    const u32 pageSize = OneGlyphPageSize(*rA, *rV);
    GlyphAtlas atlas(Context, source,
                     GlyphAtlasInfo{.PageSize = pageSize,
                                    .MaxPagesPerFieldType = 1,
                                    .Gutter = 1,
                                    .DistanceRangePx = DistanceRange,
                                    .MsdfPixelSize = MsdfPixelSize});

    const GlyphKey keyA =
        atlas.KeyFor(*face, glyphA, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf);
    const GlyphKey keyV =
        atlas.KeyFor(*face, glyphV, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf);

    // Frame 1: A alone.
    atlas.BeginFrame();
    const GlyphSlot slotA = atlas.Ensure(keyA);
    REQUIRE(slotA.Resident);
    Context.ImmediateCommands([&](Renderer::CommandBuffer& cmd) { atlas.RecordUploads(cmd); });
    CHECK(atlas.GetPageCount() == 1);

    // Frame 2: V evicts A (a prior frame's glyph) and reuses the one page — no rollover.
    atlas.BeginFrame();
    const GlyphSlot slotV = atlas.Ensure(keyV);
    REQUIRE(slotV.Resident);
    CHECK(atlas.GetPageCount() == 1);
    Context.ImmediateCommands([&](Renderer::CommandBuffer& cmd) { atlas.RecordUploads(cmd); });
    const vector<u8> pageWithV = atlas.GetPageView(0)->GetImage()->Download();
    CHECK(CountRectMismatches(pageWithV, pageSize, slotV, *rV) == 0);

    // Frame 3: A is re-requested; it re-rasterizes into a fresh slot, evicting V.
    atlas.BeginFrame();
    const GlyphSlot slotA2 = atlas.Ensure(keyA);
    REQUIRE(slotA2.Resident);
    CHECK(atlas.GetPageCount() == 1);
    Context.ImmediateCommands([&](Renderer::CommandBuffer& cmd) { atlas.RecordUploads(cmd); });
    const vector<u8> pageWithA = atlas.GetPageView(0)->GetImage()->Download();
    CHECK(CountRectMismatches(pageWithA, pageSize, slotA2, *rA) == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "glyph atlas: a same-frame request past the cap is not resident")
{
    const std::vector<std::byte> font = ReadFixtureFont();
    GlyphSource source;
    const Result<FaceId> face = source.LoadFace(font);
    REQUIRE(face.has_value());
    const u32 glyphA = source.GlyphIndex(*face, 'A');
    const u32 glyphV = source.GlyphIndex(*face, 'V');
    REQUIRE(glyphA != 0);
    REQUIRE(glyphV != 0);

    const Result<RasterizedGlyph> rA = source.Rasterize(
        *face, glyphA, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf, DistanceRange);
    const Result<RasterizedGlyph> rV = source.Rasterize(
        *face, glyphV, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf, DistanceRange);
    REQUIRE(rA.has_value());
    REQUIRE(rV.has_value());

    const u32 pageSize = OneGlyphPageSize(*rA, *rV);
    GlyphAtlas atlas(Context, source,
                     GlyphAtlasInfo{.PageSize = pageSize,
                                    .MaxPagesPerFieldType = 1,
                                    .Gutter = 1,
                                    .DistanceRangePx = DistanceRange,
                                    .MsdfPixelSize = MsdfPixelSize});

    const GlyphKey keyA =
        atlas.KeyFor(*face, glyphA, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf);
    const GlyphKey keyV =
        atlas.KeyFor(*face, glyphV, static_cast<f32>(MsdfPixelSize), GlyphFieldType::Msdf);

    // Both requested the same frame: A pins the one page, so V cannot evict it and is not resident.
    atlas.BeginFrame();
    const GlyphSlot slotA = atlas.Ensure(keyA);
    const GlyphSlot slotV = atlas.Ensure(keyV);
    CHECK(slotA.Resident);
    CHECK_FALSE(slotV.Resident);
    CHECK(atlas.GetPageCount() == 1);

    // A, the resident one, still uploads correctly.
    Context.ImmediateCommands([&](Renderer::CommandBuffer& cmd) { atlas.RecordUploads(cmd); });
    const vector<u8> page = atlas.GetPageView(0)->GetImage()->Download();
    CHECK(CountRectMismatches(page, pageSize, slotA, *rA) == 0);

    // Next frame A unpins, so V now packs by evicting it.
    atlas.BeginFrame();
    const GlyphSlot slotV2 = atlas.Ensure(keyV);
    CHECK(slotV2.Resident);
}
