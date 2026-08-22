// Font cook: the `variations` block, which is what makes a variable font usable.
//
// A variable font carries a design space rather than one weight, and the cook bakes one instance
// into an atlas — so the block is the only way to ask for anything but the face's default, and
// asking wrongly has to fail loudly rather than quietly produce the default. The fixture is the
// engine's own default UI font, which is variable (Weight 100–900, Width 75–100), so the test needs
// no font of its own; the static kerning fixture beside it supplies the not-a-variable-font case.

#include <cstring>
#include <fstream>
#include <string>

#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    void WriteFile(const path& file, const std::string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    // A one-asset pack whose font declares `variations` verbatim from the caller. The .font.json
    // names the TTF by absolute path, so the fixture lives entirely in the temp dir.
    path WritePack(const path& dir, const path& ttf, const std::string& variations)
    {
        std::string font = "{\n  \"font\": \"" + ttf.generic_string() +
                           "\",\n  \"charset\": \"ascii\",\n  \"glyphSize\": 24,\n  "
                           "\"pixelRange\": 4";
        if (!variations.empty())
        {
            font += ",\n  \"variations\": " + variations;
        }
        font += "\n}\n";
        WriteFile(dir / "instance.font.json", font);

        const path packJson = dir / "font_variations_pack.json";
        WriteFile(packJson,
                  R"({"version": 1, "assets": [
                       {"id": "0xF0A7", "type": "Font", "source": "instance.font.json"}]})");
        return packJson;
    }

    // The advance of one cooked glyph, in em units — the metric a weight change has to move.
    optional<f32> AdvanceOf(const path& archive, const u32 codepoint)
    {
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        if (!reader.has_value())
        {
            return std::nullopt;
        }
        const optional<ArchiveEntry> entry = reader->Find(AssetId{0xF0A7});
        if (!entry.has_value() || entry->Blob.size() < sizeof(CookedFontHeader))
        {
            return std::nullopt;
        }

        CookedFontHeader header{};
        std::memcpy(&header, entry->Blob.data(), sizeof(header));
        const u8* glyphs = entry->Blob.data() + sizeof(header);
        for (u32 i = 0; i < header.GlyphCount; ++i)
        {
            CookedGlyph glyph{};
            std::memcpy(&glyph, glyphs + i * sizeof(CookedGlyph), sizeof(glyph));
            if (glyph.Codepoint == codepoint)
            {
                return glyph.Advance;
            }
        }
        return std::nullopt;
    }

    // A fresh temp directory per case, so two cooks in one test never share an output path.
    path CaseDir(const char* name)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_font_var_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }
}

TEST_CASE("Cooker: a variations block cooks a different instance of the same variable font")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    REQUIRE(std::filesystem::exists(ttf));

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const path lightDir = CaseDir("light");
    const path lightPack = WritePack(lightDir, ttf, R"({"Weight": 100})");
    const path lightOut = lightDir / "light.vengpack";
    REQUIRE(cooker.CookPack(lightPack, lightOut).has_value());

    const path heavyDir = CaseDir("heavy");
    const path heavyPack = WritePack(heavyDir, ttf, R"({"Weight": 900})");
    const path heavyOut = heavyDir / "heavy.vengpack";
    REQUIRE(cooker.CookPack(heavyPack, heavyOut).has_value());

    // 'M' is the widest letter in the charset and the one a weight axis moves most: a heavier
    // instance sets more ink and a wider advance. That the two differ at all is the whole claim —
    // without the block both cooks would be the face's default and identical.
    const optional<f32> light = AdvanceOf(lightOut, 'M');
    const optional<f32> heavy = AdvanceOf(heavyOut, 'M');
    REQUIRE(light.has_value());
    REQUIRE(heavy.has_value());
    CHECK(*heavy > *light);
}

TEST_CASE("Cooker: an axis the font does not carry fails the cook and names the ones it does")
{
    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const path dir = CaseDir("unknown_axis");
    // The four-character tag rather than the name, which is the mistake an author actually makes.
    const path packJson = WritePack(dir, path(VENG_DEFAULT_FONT_TTF), R"({"wght": 700})");

    const VoidResult cooked = cooker.CookPack(packJson, dir / "out.vengpack");
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("wght") != string::npos);
    // The error is only actionable if it says what to write instead.
    CHECK(cooked.error().find("'Weight'") != string::npos);
}

TEST_CASE("Cooker: variations on a font with no design space is an error, not a silent no-op")
{
    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const path dir = CaseDir("static_font");
    const path ttf = path(VENG_COOKER_TEST_FIXTURE_DIR) / "fonts" / "VengTestKern.ttf";
    const path packJson = WritePack(dir, ttf, R"({"Weight": 700})");

    const VoidResult cooked = cooker.CookPack(packJson, dir / "out.vengpack");
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("not a variable font") != string::npos);
}
