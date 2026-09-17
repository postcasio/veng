// Font load test: cooks the font fixture pack in-process, mounts it, wires a shared GlyphSource +
// GlyphAtlas into the AssetManager, LoadSync<Font>s it, and asserts the face-backed contract the
// text draw + layout-measure paths depend on — the face loaded, the em line metrics, a known glyph's
// advance/bounds read from the face, a kerning pair, and that ShapeRun applies advances + kerning
// consistently. There is no baked atlas; every glyph is rasterized on demand into the shared atlas.

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Font.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Text/GlyphAtlas.h>
#include <Veng/Text/GlyphSource.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // The fixture font's units-per-em is 1000; a synthetic AV kern of -80 units is -0.08 em, and
    // the 'A' glyph's advance is 600 units = 0.6 em.
    constexpr f32 EmTolerance = 0.001f;

    // The fixture pack's Font AssetId (tests/cooker/fixtures/font_pack.json).
    constexpr AssetId FontId{0xFB6782CABF076640ULL};

    // The AssetId of the in-process latin-extended pack the .notdef cases cook.
    constexpr AssetId ExtendedFontId{0xF0A9ULL};

    // Cooks a one-font pack from the default font at the given charset into a fresh temp dir, and
    // returns the archive path. The .notdef cases need the default font because it carries a real
    // tofu box at glyph 0 and full Latin Extended-A coverage.
    path CookDefaultFontPack(const char* caseName, const char* charset)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_gpu_font_") + caseName);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);

        const path ttf = path(VENG_DEFAULT_FONT_TTF);
        std::ofstream(dir / "ext.font.json", std::ios::binary | std::ios::trunc)
            << "{\n  \"font\": \"" << ttf.generic_string() << "\",\n  \"hotset\": \"" << charset
            << "\"\n}\n";
        std::ofstream(dir / "pack.json", std::ios::binary | std::ios::trunc)
            << R"({"version": 1, "assets": [
                    {"id": "0xF0A9", "type": "Font", "source": "ext.font.json"}]})";

        const path out = dir / "ext.vengpack";
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(dir / "pack.json", out).has_value());
        return out;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font loader: cook, mount, LoadSync, and read the face-backed metrics contract")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "font_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_font.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(FontId);
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const Font& font = *handle->Get();

    // The face loaded into the shared rasterizer and the em line metrics came off it.
    CHECK(font.GetFaceId() != Text::FaceId::Invalid);
    CHECK(font.GetLineHeight() > 0.0f);
    CHECK(font.GetAscender() > 0.0f);
    CHECK(font.GetDescender() < 0.0f);

    // 'A' has a 600/1000 = 0.6 em advance and a non-degenerate quad, read from the face device-free.
    CHECK(font.HasGlyph('A'));
    const FontGlyph glyphA = font.GetGlyphMetrics('A');
    CHECK(glyphA.Advance == doctest::Approx(0.6f).epsilon(0.01f));
    CHECK(glyphA.PlaneMax.x > glyphA.PlaneMin.x);
    CHECK(glyphA.PlaneMax.y > glyphA.PlaneMin.y);

    // Ensuring 'A' resident packs it into the shared atlas and returns a valid page + matching advance.
    const FontGlyph resident = font.GetGlyph('A', 32.0f);
    CHECK(resident.Page.IsValid());
    CHECK(resident.UvMax.x > resident.UvMin.x);
    CHECK(resident.UvMax.y > resident.UvMin.y);
    CHECK(resident.Advance == doctest::Approx(glyphA.Advance));

    // Space is whitespace: it advances the pen but has no atlas geometry.
    CHECK(font.HasGlyph(' '));
    CHECK(font.GetGlyphMetrics(' ').Advance > 0.0f);

    // The synthetic AV kern (-80 units at 1000 upem = -0.08 em) came off the face's kern table.
    CHECK(font.GetKerning('A', 'V') == doctest::Approx(-0.08f).epsilon(0.05f));
    // An un-kerned pair reports zero.
    CHECK(font.GetKerning('A', 'A') == doctest::Approx(0.0f).epsilon(EmTolerance));

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "font loader: ShapeRun applies advances and kerning")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "font_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_font_shape.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(FontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    constexpr f32 pixelSize = 64.0f;
    const f32 advanceA = font.GetGlyphMetrics('A').Advance;
    const f32 advanceV = font.GetGlyphMetrics('V').Advance;

    // "AV" is one line of two visible quads; the kerning pulls V left of its unkerned position.
    const std::array<u32, 2> av = {'A', 'V'};
    const ShapeResult shaped = font.ShapeRun(av, pixelSize, std::nullopt);
    REQUIRE(shaped.Lines.size() == 1);
    REQUIRE(shaped.Glyphs.size() == 2);
    CHECK(shaped.Glyphs[1].Min.x < shaped.Glyphs[0].Max.x + advanceA * pixelSize);

    // The kerned line is narrower than the same pair laid out without kerning.
    const f32 kernedWidth = shaped.Lines[0].Width;
    const f32 unkernedWidth = (advanceA + advanceV) * pixelSize;
    CHECK(kernedWidth < unkernedWidth);
    CHECK(kernedWidth ==
          doctest::Approx(unkernedWidth + font.GetKerning('A', 'V') * pixelSize).epsilon(0.01f));

    // An explicit newline splits into two lines, the second baseline a line-height below the first.
    const std::array<u32, 3> twoLines = {'A', '\n', 'V'};
    const ShapeResult multi = font.ShapeRun(twoLines, pixelSize, std::nullopt);
    REQUIRE(multi.Lines.size() == 2);
    CHECK(multi.Lines[1].Baseline > multi.Lines[0].Baseline);
    CHECK(multi.Size.y > multi.Lines[0].Baseline);

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font loader: a codepoint no face covers shapes as .notdef")
{
    const path outArchive = CookDefaultFontPack("notdef", "latin-extended");

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(ExtendedFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    // A CJK codepoint is outside the default face's coverage, so no face in the chain covers it.
    constexpr u32 absent = 0x4E00; // 一 CJK UNIFIED IDEOGRAPH
    CHECK_FALSE(font.HasGlyph(absent));
    // A present codepoint still resolves.
    CHECK(font.HasGlyph('A'));

    // The .notdef box (the face's glyph 0) carries a real advance, so a coverage gap is visible.
    const FontGlyph notdef = font.GetGlyphMetrics(absent);
    CHECK(notdef.Advance > 0.0f);

    constexpr f32 pixelSize = 64.0f;

    // ShapeRun advances the absent codepoint by the .notdef advance rather than dropping it.
    const std::array<u32, 1> missing = {absent};
    const ShapeResult shaped = font.ShapeRun(missing, pixelSize, std::nullopt);
    REQUIRE(shaped.Lines.size() == 1);
    CHECK(shaped.Lines[0].Width == doctest::Approx(notdef.Advance * pixelSize).epsilon(0.001f));

    // A present codepoint shapes to the same width whether or not a missing one preceded it: the
    // notdef contributes its own advance and nothing spurious to the run.
    const std::array<u32, 1> present = {'A'};
    const ShapeResult a = font.ShapeRun(present, pixelSize, std::nullopt);
    const std::array<u32, 2> pair = {absent, 'A'};
    const ShapeResult both = font.ShapeRun(pair, pixelSize, std::nullopt);
    CHECK(both.Lines[0].Width ==
          doctest::Approx(notdef.Advance * pixelSize + a.Lines[0].Width).epsilon(0.001f));

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font loader: the latin-extended hot set resolves Extended-A glyphs")
{
    const path outArchive = CookDefaultFontPack("extended", "latin-extended");

    Text::GlyphSource source;
    Text::GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(ExtendedFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    // A Latin Extended-A letter and a curly quote resolve to real glyphs the face covers.
    CHECK(font.HasGlyph(0x142)); // ł
    CHECK(font.GetGlyphMetrics(0x142).Advance > 0.0f);
    CHECK(font.HasGlyph(0x2019)); // ’

    std::filesystem::remove(outArchive);
}
