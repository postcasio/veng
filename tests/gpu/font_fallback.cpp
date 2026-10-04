// Font fallback + face-backed lookup GPU cases. A base font whose face lacks a codepoint resolves
// it through a fallback face and ensures the glyph resident in the shared dynamic atlas; a codepoint
// no face in the chain covers lands on .notdef; the declared hot set is resident immediately after
// load with no draw; and an MSDF-declared and an SDF-declared font land their glyphs on pages of the
// expected field kind. A stale (old-version) blob is rejected at load. The fonts are cooked in
// process and loaded through an AssetManager wired to a shared GlyphSource + GlyphAtlas.

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TempPath.h"
#include "support/TestCook.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
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
    constexpr AssetId BaseFontId{0x000000000000F0C0ULL};
    constexpr AssetId FallbackFontId{0x000000000000F0C1ULL};

    void WriteFile(const path& file, const std::string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    path CaseDir(const char* name)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_gpu_font_fb_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A single-font pack from the given TTF, field type, and optional fallback id.
    path CookFontPack(const char* name, AssetId id, const path& ttf, const char* field,
                      const optional<AssetId>& fallback)
    {
        const path dir = CaseDir(name);
        std::string src = "{\n  \"font\": \"" + ttf.generic_string() + "\",\n  \"field\": \"" +
                          field +
                          "\",\n  \"hotset\": \"ascii\",\n  \"glyphSize\": 24,\n  "
                          "\"pixelRange\": 4";
        if (fallback)
        {
            src += ",\n  \"fallback\": [\"" + FormatAssetId(*fallback) + "\"]";
        }
        src += "\n}\n";
        WriteFile(dir / "font.json", src);
        WriteFile(dir / "pack.json", "{\"version\": 1, \"assets\": [{\"id\": \"" +
                                         FormatAssetId(id) +
                                         "\", \"type\": \"Font\", \"source\": \"font.json\"}]}");
        const path out = dir / (std::string(name) + ".vengpack");
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(cooker, dir / "pack.json", out).has_value());
        return out;
    }

    // The base + fallback pair in one pack: base (VengTestKern, a minimal synthetic face) falls back
    // to the fallback (Roboto, broad Latin coverage).
    path CookFallbackPair(const char* name, const path& baseTtf, const path& fallbackTtf)
    {
        const path dir = CaseDir(name);
        WriteFile(dir / "base.font.json",
                  "{\n  \"font\": \"" + baseTtf.generic_string() +
                      "\",\n  \"field\": \"msdf\",\n  \"hotset\": \"ascii\",\n  \"fallback\": [\"" +
                      FormatAssetId(FallbackFontId) + "\"],\n  \"glyphSize\": 24\n}\n");
        WriteFile(dir / "fallback.font.json", "{\n  \"font\": \"" + fallbackTtf.generic_string() +
                                                  "\",\n  \"hotset\": \"ascii\",\n  \"glyphSize\": "
                                                  "24\n}\n");
        WriteFile(dir / "pack.json", "{\"version\": 1, \"assets\": [{\"id\": \"" +
                                         FormatAssetId(BaseFontId) +
                                         "\", \"type\": \"Font\", \"source\": \"base.font.json\"},"
                                         "{\"id\": \"" +
                                         FormatAssetId(FallbackFontId) +
                                         "\", \"type\": \"Font\", \"source\": "
                                         "\"fallback.font.json\"}]}");
        const path out = dir / (std::string(name) + ".vengpack");
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(cooker, dir / "pack.json", out).has_value());
        return out;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font fallback: a missing codepoint resolves through the fallback face")
{
    const path baseTtf = path(GPU_COOKER_FIXTURE_DIR) / "fonts" / "VengTestKern.ttf";
    const path robotoTtf = path(VENG_DEFAULT_FONT_TTF);
    const path archive = CookFallbackPair("resolve", baseTtf, robotoTtf);

    GlyphSource source;
    GlyphAtlas atlas(Context, source);

    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> baseHandle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(baseHandle.has_value());
    const Font& base = *baseHandle->Get();
    const AssetResult<AssetHandle<Font>> fbHandle = assets.LoadSync<Font>(FallbackFontId);
    REQUIRE(fbHandle.has_value());
    const Font& fallback = *fbHandle->Get();

    REQUIRE(base.GetFaceId() != FaceId::Invalid);
    REQUIRE(fallback.GetFaceId() != FaceId::Invalid);

    // A codepoint the minimal base face lacks but the Roboto fallback covers.
    u32 crossFace = 0;
    for (u32 cp = 0x21; cp <= 0x7E && crossFace == 0; cp++)
    {
        if (!source.HasGlyph(base.GetFaceId(), cp) && source.HasGlyph(fallback.GetFaceId(), cp))
        {
            crossFace = cp;
        }
    }
    REQUIRE_MESSAGE(crossFace != 0, "no codepoint the base lacks and the fallback covers");

    // The base resolves it through the chain and ensures it resident in the shared atlas.
    CHECK(base.HasGlyph(crossFace));
    const FontGlyph glyph = base.GetGlyph(crossFace, 32.0f);
    CHECK(glyph.Page.IsValid());
    CHECK(glyph.Advance > 0.0f);

    // Metrics and ensure-resident agree on the advance, so a measured run and a drawn run match.
    const FontGlyph metrics = base.GetGlyphMetrics(crossFace);
    CHECK(metrics.Advance == doctest::Approx(glyph.Advance));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "font fallback: a codepoint no face covers is .notdef")
{
    const path baseTtf = path(GPU_COOKER_FIXTURE_DIR) / "fonts" / "VengTestKern.ttf";
    const path robotoTtf = path(VENG_DEFAULT_FONT_TTF);
    const path archive = CookFallbackPair("notdef", baseTtf, robotoTtf);

    GlyphSource source;
    GlyphAtlas atlas(Context, source);

    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Font>> baseHandle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(baseHandle.has_value());
    const Font& base = *baseHandle->Get();

    // No face in the chain (a minimal synthetic base, Roboto fallback) covers a CJK ideograph.
    constexpr u32 cjk = 0x4E00; // 一
    CHECK_FALSE(base.HasGlyph(cjk));

    // GetGlyph still returns — the primary face's .notdef box, at this font's field type.
    const FontGlyph glyph = base.GetGlyph(cjk, 32.0f);
    CHECK(glyph.FieldType == GlyphFieldType::Msdf);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font fallback: the hot set is resident immediately after load, with no draw")
{
    const path archive =
        CookFontPack("hotset", BaseFontId, path(VENG_DEFAULT_FONT_TTF), "msdf", std::nullopt);

    GlyphSource source;
    GlyphAtlas atlas(Context, source);

    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(archive).has_value());

    // No page exists before the load; the warm at load packs the hot set into the shared atlas.
    CHECK(atlas.GetPageCount() == 0);
    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE(handle.has_value());
    CHECK(atlas.GetPageCount() > 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font fallback: MSDF- and SDF-declared fonts land on the expected page kind")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    const path msdfArchive = CookFontPack("msdf", BaseFontId, ttf, "msdf", std::nullopt);
    const path sdfArchive = CookFontPack("sdf", FallbackFontId, ttf, "sdf", std::nullopt);

    GlyphSource source;
    GlyphAtlas atlas(Context, source);

    AssetManager assets(Context, Tasks, Types);
    assets.SetGlyphSystems(&source, &atlas);
    REQUIRE(assets.Mount(msdfArchive).has_value());
    REQUIRE(assets.Mount(sdfArchive).has_value());

    const AssetResult<AssetHandle<Font>> msdfHandle = assets.LoadSync<Font>(BaseFontId);
    const AssetResult<AssetHandle<Font>> sdfHandle = assets.LoadSync<Font>(FallbackFontId);
    REQUIRE(msdfHandle.has_value());
    REQUIRE(sdfHandle.has_value());

    const FontGlyph msdfGlyph = msdfHandle->Get()->GetGlyph('A', 32.0f);
    const FontGlyph sdfGlyph = sdfHandle->Get()->GetGlyph('A', 32.0f);

    CHECK(msdfGlyph.FieldType == GlyphFieldType::Msdf);
    CHECK(sdfGlyph.FieldType == GlyphFieldType::Sdf);
    REQUIRE(msdfGlyph.Page.IsValid());
    REQUIRE(sdfGlyph.Page.IsValid());
    // MSDF and SDF renditions never share a page — their formats and sampling differ.
    CHECK(msdfGlyph.Page.Index != sdfGlyph.Page.Index);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "font fallback: a stale-version font blob is rejected at load")
{
    // A header-only blob carrying the previous format version: the loader rejects it before it
    // touches the atlas or the face, so a pack cooked against the old layout fails loudly.
    CookedFontHeader header{};
    header.Version = CookedFontVersion - 1;
    const auto* bytes = reinterpret_cast<const u8*>(&header);
    const std::span<const u8> blob(bytes, sizeof(header));

    ArchiveWriter writer;
    writer.Add(BaseFontId, AssetTypes::Font, blob);

    AssetManager assets(Context, Tasks, Types);
    const MountHandle mount = assets.MountMemory(writer.Build(), "stale font");

    const AssetResult<AssetHandle<Font>> handle = assets.LoadSync<Font>(BaseFontId);
    REQUIRE_FALSE(handle.has_value());
    CHECK(handle.error().Kind == AssetError::Corrupt);
}
