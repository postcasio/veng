// Font cook: the charset presets and the always-present `.notdef` glyph.
//
// A translated string renders only the glyphs the atlas carries, so the "latin-extended" preset is
// a correctness gate on Latin-script European languages — its accented letters and typographic
// punctuation live outside Latin-1. And a codepoint the atlas lacks must be *seen*, not dropped, so
// every cooked atlas carries a `.notdef` tofu box under a reserved sentinel. The fixture is the
// engine's own default UI font (Roboto), which covers Latin Extended-A and the curated punctuation.

#include <array>
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
    constexpr AssetId CharsetFontId{0xF0A8};

    void WriteFile(const path& file, const std::string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    // A one-asset pack whose font names the given charset preset and the Roboto TTF by absolute
    // path, so the fixture lives entirely in the temp dir.
    path WritePack(const path& dir, const path& ttf, const std::string& charset)
    {
        const std::string font = "{\n  \"font\": \"" + ttf.generic_string() +
                                 "\",\n  \"charset\": \"" + charset +
                                 "\",\n  \"glyphSize\": 24,\n  \"pixelRange\": 4\n}\n";
        WriteFile(dir / "instance.font.json", font);

        const path packJson = dir / "font_charset_pack.json";
        WriteFile(packJson,
                  R"({"version": 1, "assets": [
                       {"id": "0xF0A8", "type": "Font", "source": "instance.font.json"}]})");
        return packJson;
    }

    // One cooked glyph, decoded from the archive, or nullopt if the codepoint is not cooked.
    optional<CookedGlyph> GlyphOf(const path& archive, const u32 codepoint)
    {
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        if (!reader.has_value())
        {
            return std::nullopt;
        }
        const optional<ArchiveEntry> entry = reader->Find(CharsetFontId);
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
                return glyph;
            }
        }
        return std::nullopt;
    }

    path CaseDir(const char* name)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_font_charset_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }
}

TEST_CASE("Cooker: the latin-extended preset carries Extended-A letters and curly punctuation")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    REQUIRE(std::filesystem::exists(ttf));

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const path dir = CaseDir("extended");
    const path pack = WritePack(dir, ttf, "latin-extended");
    const path out = dir / "extended.vengpack";
    REQUIRE(cooker.CookPack(pack, out).has_value());

    // A stratified handful across Latin Extended-A (Polish/Czech/Hungarian/Turkish) and the curated
    // General-Punctuation set — each must be a cooked glyph with real atlas geometry, not tofu.
    const std::array<u32, 8> covered = {
        0x142,  // ł  LATIN SMALL LETTER L WITH STROKE
        0x159,  // ř  LATIN SMALL LETTER R WITH CARON
        0x151,  // ő  LATIN SMALL LETTER O WITH DOUBLE ACUTE
        0x15F,  // ş  LATIN SMALL LETTER S WITH CEDILLA
        0x2019, // ’  RIGHT SINGLE QUOTATION MARK
        0x201C, // “  LEFT DOUBLE QUOTATION MARK
        0x2014, // —  EM DASH
        0x2026, // …  HORIZONTAL ELLIPSIS
    };
    for (const u32 cp : covered)
    {
        const optional<CookedGlyph> glyph = GlyphOf(out, cp);
        REQUIRE_MESSAGE(glyph.has_value(), "missing cooked glyph U+" << cp);
        CHECK(glyph->AtlasWidth > 0.0f);
        CHECK(glyph->AtlasHeight > 0.0f);
    }

    // Latin-1 accented letters the base languages need are still there under the wider preset.
    CHECK(GlyphOf(out, 0xE9).has_value()); // é
    // A plain-ASCII letter is unaffected.
    CHECK(GlyphOf(out, 'A').has_value());
}

TEST_CASE("Cooker: every cooked font carries a .notdef under the reserved sentinel")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // Both the narrow (ascii) and wide (latin-extended) presets emit the tofu box.
    for (const char* preset : {"ascii", "latin-extended"})
    {
        const path dir = CaseDir(preset);
        const path pack = WritePack(dir, ttf, preset);
        const path out = dir / "out.vengpack";
        REQUIRE(cooker.CookPack(pack, out).has_value());

        const optional<CookedGlyph> notdef = GlyphOf(out, CookedFontNotdefCodepoint);
        REQUIRE(notdef.has_value());
        // The tofu is a visible box with a real advance — the whole point is that a gap draws.
        CHECK(notdef->Advance > 0.0f);
        CHECK(notdef->AtlasWidth > 0.0f);
        CHECK(notdef->AtlasHeight > 0.0f);
    }
}

TEST_CASE("Cooker: an unknown charset preset is an error naming the valid ones")
{
    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const path dir = CaseDir("unknown");
    const path pack = WritePack(dir, path(VENG_DEFAULT_FONT_TTF), "cyrillic");

    const VoidResult cooked = cooker.CookPack(pack, dir / "out.vengpack");
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("cyrillic") != string::npos);
    CHECK(cooked.error().find("latin-extended") != string::npos);
}
