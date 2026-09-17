// ShapeRun layout cases over the face-backed Font. A run measured device-free (metrics only) and a
// run drawn ensure-resident break lines and size identically — the property that lets a Yoga measure
// and the paint agree; a space-less CJK ideograph run wraps at an ideograph boundary within a max
// width (the minimal inter-ideograph break); a Latin run breaks only at its spaces, keeping words
// whole; and kerning applies within a face but a cross-face pair (a base glyph and a fallback glyph)
// carries none. The fonts are cooked in process and loaded through an AssetManager wired to a shared
// GlyphSource + GlyphAtlas, since a face-backed Font and the ensure-resident path both need them.

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Font.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Text/GlyphAtlas.h>
#include <Veng/Text/GlyphSource.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Text;

namespace
{
    constexpr AssetId BaseFontId{0x000000000000F0D0ULL};
    constexpr AssetId FallbackFontId{0x000000000000F0D1ULL};

    void WriteFile(const path& file, const std::string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    path CaseDir(const char* name)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_gpu_shape_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A single-font pack from the default (broad Latin) face.
    path CookSingleFont(const char* name, AssetId id)
    {
        const path dir = CaseDir(name);
        const path ttf = path(VENG_DEFAULT_FONT_TTF);
        WriteFile(dir / "font.json",
                  "{\n  \"font\": \"" + ttf.generic_string() + "\",\n  \"hotset\": \"ascii\"\n}\n");
        WriteFile(dir / "pack.json", "{\"version\": 1, \"assets\": [{\"id\": \"" +
                                         FormatAssetId(id) +
                                         "\", \"type\": \"Font\", \"source\": \"font.json\"}]}");
        const path out = dir / (std::string(name) + ".vengpack");
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(dir / "pack.json", out).has_value());
        return out;
    }

    // A minimal base face (VengTestKern: space/A/V/T with an AV kern pair) that falls back to the
    // broad-coverage default face, in one pack.
    path CookFallbackPair(const char* name)
    {
        const path dir = CaseDir(name);
        const path baseTtf = path(GPU_COOKER_FIXTURE_DIR) / "fonts" / "VengTestKern.ttf";
        const path fallbackTtf = path(VENG_DEFAULT_FONT_TTF);
        WriteFile(dir / "base.font.json", "{\n  \"font\": \"" + baseTtf.generic_string() +
                                              "\",\n  \"hotset\": \"ascii\",\n  \"fallback\": [\"" +
                                              FormatAssetId(FallbackFontId) + "\"]\n}\n");
        WriteFile(dir / "fallback.font.json", "{\n  \"font\": \"" + fallbackTtf.generic_string() +
                                                  "\",\n  \"hotset\": \"ascii\"\n}\n");
        WriteFile(dir / "pack.json",
                  "{\"version\": 1, \"assets\": [{\"id\": \"" + FormatAssetId(BaseFontId) +
                      "\", \"type\": \"Font\", \"source\": \"base.font.json\"},{\"id\": \"" +
                      FormatAssetId(FallbackFontId) +
                      "\", \"type\": \"Font\", \"source\": \"fallback.font.json\"}]}");
        const path out = dir / (std::string(name) + ".vengpack");
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(dir / "pack.json", out).has_value());
        return out;
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "shape run: a metrics-measured run and an ensure-resident run lay out identically")
{
    const path archive = CookSingleFont("layout", BaseFontId);

    GlyphSource source;
    GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    constexpr f32 pixelSize = 32.0f;
    const std::array<u32, 7> text = {'W', 'a', 'x', ' ', 'f', 'l', 'y'};

    // The advance is identical from GetGlyphMetrics (measure) and the ensured slot (draw), so the two
    // runs must break into the same lines with the same widths and the same overall size — the box a
    // measure sizes is the box the draw fills, whether or not a glyph was resident when it drew.
    const ShapeResult measured = font.ShapeRun(text, pixelSize, 60.0f, TextShapeMode::Measure);
    const ShapeResult drawn = font.ShapeRun(text, pixelSize, 60.0f, TextShapeMode::Draw);

    REQUIRE(measured.Lines.size() == drawn.Lines.size());
    CHECK(measured.Size.x == doctest::Approx(drawn.Size.x));
    CHECK(measured.Size.y == doctest::Approx(drawn.Size.y));
    for (usize i = 0; i < measured.Lines.size(); i++)
    {
        CHECK(measured.Lines[i].Count == drawn.Lines[i].Count);
        CHECK(measured.Lines[i].Width == doctest::Approx(drawn.Lines[i].Width));
        CHECK(measured.Lines[i].Baseline == doctest::Approx(drawn.Lines[i].Baseline));
    }

    // The drawn run's visible glyphs carry a resident page and the font's field type, so the shader
    // has a page to sample and a branch to pick.
    REQUIRE_FALSE(drawn.Glyphs.empty());
    for (const ShapedGlyph& glyph : drawn.Glyphs)
    {
        CHECK(glyph.Page.IsValid());
        CHECK(glyph.FieldType == GlyphFieldType::Msdf);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "shape run: a space-less CJK run wraps at an ideograph boundary")
{
    const path archive = CookSingleFont("cjk", BaseFontId);

    GlyphSource source;
    GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    constexpr f32 pixelSize = 32.0f;
    // Six CJK ideographs with no spaces. The face lacks them, so each resolves to its .notdef box —
    // but the break is a codepoint-class decision, not a coverage one, so the run still wraps.
    const std::array<u32, 6> cjk = {0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D};

    // The width of a single ideograph, to set a max width that must force a wrap.
    const std::array<u32, 1> one = {0x4E00};
    const f32 glyphWidth = font.ShapeRun(one, pixelSize, std::nullopt).Size.x;
    REQUIRE_MESSAGE(glyphWidth > 0.0f, "the face's .notdef box has no advance to wrap");

    // A width holding roughly three ideographs: a space-less six-glyph run must break into more than
    // one line, which it can only do at an inter-ideograph boundary.
    const ShapeResult wrapped =
        font.ShapeRun(cjk, pixelSize, glyphWidth * 3.0f, TextShapeMode::Measure);
    CHECK(wrapped.Lines.size() >= 2);
    // Every ideograph landed on some line, none dropped by the wrap.
    u32 total = 0;
    for (const ShapedLine& line : wrapped.Lines)
    {
        total += line.Count;
    }
    CHECK(total == cjk.size());

    // With no width constraint the same run is a single line — the break is the wrap's, not the run's.
    const ShapeResult unwrapped =
        font.ShapeRun(cjk, pixelSize, std::nullopt, TextShapeMode::Measure);
    CHECK(unwrapped.Lines.size() == 1);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "shape run: a Latin run breaks only at its spaces")
{
    const path archive = CookSingleFont("latin", BaseFontId);

    GlyphSource source;
    GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    constexpr f32 pixelSize = 32.0f;

    // One word's width, to size a box that holds a word but not two.
    const std::array<u32, 3> word = {'a', 'b', 'c'};
    const f32 wordWidth = font.ShapeRun(word, pixelSize, std::nullopt).Size.x;
    REQUIRE(wordWidth > 0.0f);

    // "abc abc" in a box a touch wider than one word breaks at the space, so each line is a whole
    // word — a Latin run does not break between two adjacent letters the way a CJK run does.
    const std::array<u32, 7> latin = {'a', 'b', 'c', ' ', 'a', 'b', 'c'};
    const ShapeResult wrapped =
        font.ShapeRun(latin, pixelSize, wordWidth * 1.5f, TextShapeMode::Measure);
    REQUIRE(wrapped.Lines.size() == 2);
    // The first line is exactly the first word (three visible glyphs, the trailing space carrying no
    // quad), so the break kept the word whole rather than splitting it between letters.
    CHECK(wrapped.Lines[0].Count == 3);
    CHECK(wrapped.Lines[1].Count == 3);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "shape run: kerning applies within a face but not across a fallback pair")
{
    const path archive = CookFallbackPair("kern");

    GlyphSource source;
    GlyphAtlas atlas(Context, source);
    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(handle.has_value());
    const Font& font = *handle->Get();

    // The synthetic base face kerns its A/V pair (-80 units at 1000 upem = -0.08 em); both glyphs
    // resolve to the base face, so the pair kerns.
    REQUIRE(font.HasGlyph('A'));
    REQUIRE(font.HasGlyph('V'));
    CHECK(font.GetKerning('A', 'V') == doctest::Approx(-0.08f).epsilon(0.05f));

    // A codepoint the minimal base lacks but the fallback covers: the pair (A on the base, this on
    // the fallback) crosses faces, so it carries no kerning however the fallback kerns its own pairs.
    u32 crossFace = 0;
    for (u32 cp = 'a'; cp <= 'z' && crossFace == 0; cp++)
    {
        if (!source.HasGlyph(font.GetFaceId(), cp) && font.HasGlyph(cp))
        {
            crossFace = cp;
        }
    }
    REQUIRE_MESSAGE(crossFace != 0, "no codepoint the base lacks and the fallback covers");
    CHECK(font.GetKerning('A', crossFace) == doctest::Approx(0.0f));
    CHECK(font.GetKerning(crossFace, 'A') == doctest::Approx(0.0f));

    std::filesystem::remove(archive);
}
