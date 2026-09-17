// Font cook: the `variations` block validation.
//
// A variable font carries a design space rather than one weight, and asking for an axis it does not
// carry has to fail loudly rather than quietly produce the default. The cook validates the block
// against the face's own axes. The fixture is the engine's own default UI font, which is variable
// (Weight 100–900, Width 75–100), so the test needs no font of its own; the static kerning fixture
// beside it supplies the not-a-variable-font case.
//
// The runtime now rasterizes every glyph from the embedded default-instance face bytes, so a
// declared instance no longer reaches the drawn glyphs; applying the variation coordinates at
// runtime (in GlyphSource) is a deferred follow-up. What this file pins is the validation, which
// still runs at cook time.

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
                           "\",\n  \"charset\": "
                           "\"ascii\"";
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

    // A fresh temp directory per case, so two cooks in one test never share an output path.
    path CaseDir(const char* name)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_font_var_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }
}

TEST_CASE("Cooker: a valid variations block on a variable font cooks a well-formed blob")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    REQUIRE(std::filesystem::exists(ttf));

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // A recognized axis at a valid coordinate passes validation and cooks a current-version blob
    // with the embedded face bytes. (The declared instance no longer reaches the drawn glyphs — the
    // runtime rasterizes the embedded default instance — so there is no baked metric to compare; the
    // property here is that a well-formed variations block validates and cooks.)
    const path dir = CaseDir("heavy");
    const path out = dir / "heavy.vengpack";
    REQUIRE(cooker.CookPack(WritePack(dir, ttf, R"({"Weight": 900})"), out).has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(out);
    REQUIRE(reader.has_value());
    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xF0A7});
    REQUIRE(entry.has_value());
    REQUIRE(entry->Blob.size() >= sizeof(CookedFontHeader));
    CookedFontHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));
    CHECK(header.Version == CookedFontVersion);
    CHECK(header.FaceBytes > 0u);
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
