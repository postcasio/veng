// Font load test: cooks the font fixture pack in-process, mounts it,
// LoadSync<Font>s it through AssetManager, and asserts the atlas dimensions,
// glyph count, a known glyph's advance/bounds, a kerning pair, and that
// ShapeRun applies advances + kerning consistently. The metrics contract the
// text draw + layout-measure paths depend on.

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
#include <Veng/Renderer/Types.h>

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
            << "{\n  \"font\": \"" << ttf.generic_string() << "\",\n  \"charset\": \"" << charset
            << "\",\n  \"glyphSize\": 24,\n  \"pixelRange\": 4\n}\n";
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
                  "font loader: cook, mount, LoadSync, and read the atlas + metrics contract")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "font_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_font.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(FontId);
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const Font& font = *handle->Get();

    // The atlas is a non-empty, power-of-two-square RGBA8 image the loader uploaded and registered.
    const uvec2 extent = font.GetAtlasExtent();
    CHECK(extent.x > 0);
    CHECK(extent.y > 0);
    CHECK(extent.x == extent.y);
    CHECK(font.GetAtlasHandle().IsValid());
    CHECK(font.GetAtlasSamplerHandle().IsValid());
    CHECK(font.GetDistanceRange() == doctest::Approx(4.0f));

    // The fixture cooks the printable-ASCII charset; the four glyphs with outlines are present.
    CHECK(font.GetGlyphCount() >= 4);

    // 'A' has a 600/1000 = 0.6 em advance and a non-degenerate quad.
    const FontGlyph* glyphA = font.GetGlyph('A');
    REQUIRE(glyphA != nullptr);
    CHECK(glyphA->Advance == doctest::Approx(0.6f).epsilon(0.01f));
    CHECK(glyphA->PlaneMax.x > glyphA->PlaneMin.x);
    CHECK(glyphA->PlaneMax.y > glyphA->PlaneMin.y);
    CHECK(glyphA->UvMax.x > glyphA->UvMin.x);
    CHECK(glyphA->UvMax.y > glyphA->UvMin.y);

    // Space is whitespace: it advances the pen but has no atlas geometry.
    const FontGlyph* glyphSpace = font.GetGlyph(' ');
    REQUIRE(glyphSpace != nullptr);
    CHECK(glyphSpace->Advance > 0.0f);

    // The synthetic AV kern (-80 units at 1000 upem = -0.08 em) round-tripped through the cook.
    CHECK(font.GetKerning('A', 'V') == doctest::Approx(-0.08f).epsilon(0.02f));
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

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(FontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    constexpr f32 pixelSize = 64.0f;

    // "AV" is one line of two visible quads; the kerning pulls V left of its unkerned position.
    const std::array<u32, 2> av = {'A', 'V'};
    const ShapeResult shaped = font.ShapeRun(av, pixelSize, std::nullopt);
    REQUIRE(shaped.Lines.size() == 1);
    REQUIRE(shaped.Glyphs.size() == 2);
    CHECK(shaped.Glyphs[1].Min.x <
          shaped.Glyphs[0].Max.x + font.GetGlyph('A')->Advance * pixelSize);

    // The kerned line is narrower than the same pair laid out without kerning.
    const f32 kernedWidth = shaped.Lines[0].Width;
    const f32 unkernedWidth =
        (font.GetGlyph('A')->Advance + font.GetGlyph('V')->Advance) * pixelSize;
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
                  "font loader: a missing codepoint returns null but shapes as .notdef")
{
    const path outArchive = CookDefaultFontPack("notdef", "latin-extended");

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(ExtendedFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    // The .notdef is a real box with a real advance — the whole point is that a gap is visible.
    const FontGlyph& notdef = font.GetNotdefGlyph();
    CHECK(notdef.Advance > 0.0f);
    CHECK(notdef.PlaneMax.x > notdef.PlaneMin.x);
    CHECK(notdef.PlaneMax.y > notdef.PlaneMin.y);

    // A CJK codepoint is outside every Latin preset, so GetGlyph keeps its null-on-absent contract.
    constexpr u32 absent = 0x4E00; // 一 CJK UNIFIED IDEOGRAPH
    CHECK(font.GetGlyph(absent) == nullptr);
    // A present codepoint still resolves.
    CHECK(font.GetGlyph('A') != nullptr);

    constexpr f32 pixelSize = 64.0f;

    // ShapeRun draws the absent codepoint as one visible box advanced by the .notdef advance,
    // rather than dropping it — a single glyph laid across the notdef's advance width.
    const std::array<u32, 1> missing = {absent};
    const ShapeResult shaped = font.ShapeRun(missing, pixelSize, std::nullopt);
    REQUIRE(shaped.Lines.size() == 1);
    REQUIRE(shaped.Glyphs.size() == 1);
    CHECK(shaped.Lines[0].Width == doctest::Approx(notdef.Advance * pixelSize).epsilon(0.001f));

    // A present codepoint shapes to the same width whether or not a missing one preceded it: the
    // notdef contributes its own advance and nothing spurious to the run.
    const std::array<u32, 1> present = {'A'};
    const ShapeResult a = font.ShapeRun(present, pixelSize, std::nullopt);
    REQUIRE(a.Glyphs.size() == 1);
    const std::array<u32, 2> pair = {absent, 'A'};
    const ShapeResult both = font.ShapeRun(pair, pixelSize, std::nullopt);
    REQUIRE(both.Glyphs.size() == 2);
    CHECK(both.Lines[0].Width ==
          doctest::Approx(notdef.Advance * pixelSize + a.Lines[0].Width).epsilon(0.001f));

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font loader: the latin-extended charset resolves Extended-A glyphs")
{
    const path outArchive = CookDefaultFontPack("extended", "latin-extended");

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(ExtendedFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    // A Latin Extended-A letter and a curly quote resolve to real quads, where an ascii-only atlas
    // would have shaped them as .notdef.
    const FontGlyph* lStroke = font.GetGlyph(0x142); // ł
    REQUIRE(lStroke != nullptr);
    CHECK(lStroke->Advance > 0.0f);
    CHECK(font.GetGlyph(0x2019) != nullptr); // ’

    std::filesystem::remove(outArchive);
}
