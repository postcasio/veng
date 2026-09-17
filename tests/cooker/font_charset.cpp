// Font cook: the charset presets, which name the hot set the runtime pre-rasterizes at load.
//
// A preset selects the codepoints warmed into the shared atlas at load, so the "latin-extended"
// preset is a correctness gate on Latin-script European languages — its accented letters and
// typographic punctuation live outside Latin-1. The fixture is the engine's own default UI font
// (Roboto), which covers Latin Extended-A and the curated punctuation. There is no baked atlas or
// glyph table any more; a preset's reach is checked through the cooked hot-set codepoints.

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
                                 "\",\n  \"charset\": \"" + charset + "\"\n}\n";
        WriteFile(dir / "instance.font.json", font);

        const path packJson = dir / "font_charset_pack.json";
        WriteFile(packJson,
                  R"({"version": 1, "assets": [
                       {"id": "0xF0A8", "type": "Font", "source": "instance.font.json"}]})");
        return packJson;
    }

    // Whether a codepoint is in the cooked font's hot set — the u32 array following the face bytes.
    bool HotsetContains(const path& archive, const u32 codepoint)
    {
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        if (!reader.has_value())
        {
            return false;
        }
        const optional<ArchiveEntry> entry = reader->Find(CharsetFontId);
        if (!entry.has_value() || entry->Blob.size() < sizeof(CookedFontHeader))
        {
            return false;
        }

        CookedFontHeader header{};
        std::memcpy(&header, entry->Blob.data(), sizeof(header));
        const usize hotsetOffset = sizeof(header) + static_cast<usize>(header.FaceBytes);
        if (entry->Blob.size() <
            hotsetOffset + static_cast<usize>(header.HotsetCount) * sizeof(u32))
        {
            return false;
        }
        for (u32 i = 0; i < header.HotsetCount; ++i)
        {
            u32 cp = 0;
            std::memcpy(&cp, entry->Blob.data() + hotsetOffset + i * sizeof(u32), sizeof(u32));
            if (cp == codepoint)
            {
                return true;
            }
        }
        return false;
    }

    path CaseDir(const char* name)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_font_charset_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }
}

TEST_CASE("Cooker: the latin-extended preset warms Extended-A letters and curly punctuation")
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
    // General-Punctuation set — each must be in the cooked hot set the runtime warms at load.
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
        CHECK_MESSAGE(HotsetContains(out, cp), "missing hot-set codepoint U+" << cp);
    }

    // Latin-1 accented letters the base languages need are still there under the wider preset.
    CHECK(HotsetContains(out, 0xE9)); // é
    // A plain-ASCII letter is unaffected.
    CHECK(HotsetContains(out, 'A'));
}

TEST_CASE("Cooker: a preset selects the hot set it names, narrow or wide")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // The narrow (ascii) preset warms ASCII and nothing beyond Latin-1; the wide (latin-extended)
    // preset adds the Extended-A letters. A codepoint is in the hot set iff its preset names it.
    const path asciiDir = CaseDir("ascii");
    const path asciiOut = asciiDir / "ascii.vengpack";
    REQUIRE(cooker.CookPack(WritePack(asciiDir, ttf, "ascii"), asciiOut).has_value());
    CHECK(HotsetContains(asciiOut, 'A'));
    CHECK_FALSE(HotsetContains(asciiOut, 0x142)); // ł is outside ASCII

    const path extDir = CaseDir("wide");
    const path extOut = extDir / "wide.vengpack";
    REQUIRE(cooker.CookPack(WritePack(extDir, ttf, "latin-extended"), extOut).has_value());
    CHECK(HotsetContains(extOut, 'A'));
    CHECK(HotsetContains(extOut, 0x142)); // ł is in Latin Extended-A
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
