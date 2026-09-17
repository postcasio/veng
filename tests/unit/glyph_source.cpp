// Runtime glyph rasterizer cases. Loads the synthetic VengTestKern.ttf fixture (four glyphs —
// space, A, V, T — at 1000 units/em, a 600-unit 'A' advance, and a legacy kern table with an
// AV pair of -80 units) directly from its bytes and checks the device-free GlyphSource: coverage
// query, glyph-index lookup, MSDF vs SDF channel counts and metrics against the face's own, and
// the em-normalized kerning. CPU-only — no graphics device is touched — plus the reflected
// GlyphFieldType enum's name table.

#include <doctest/doctest.h>

#include <Veng/Reflection/EnumName.h>
#include <Veng/Text/GlyphSource.h>

#include <cstddef>
#include <fstream>
#include <vector>

using namespace Veng;
using namespace Veng::Text;

namespace
{
    // The bundled kerning fixture, reused rather than shipping a new third-party face.
    std::vector<std::byte> ReadFixtureFont()
    {
        const std::string path =
            std::string(VENG_COOKER_TEST_FIXTURE_DIR) + "/fonts/VengTestKern.ttf";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        REQUIRE_MESSAGE(file.good(), "failed to open fixture font: " << path);
        const std::streamsize size = file.tellg();
        file.seekg(0);
        std::vector<std::byte> bytes(static_cast<usize>(size));
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        REQUIRE(file.good());
        return bytes;
    }

    // The fixture's em square and its authored metrics, in em units.
    constexpr f32 AdvanceA = 0.6f; // 600 / 1000 units
    constexpr f32 KernAV = -0.08f; // -80 / 1000 units
}

TEST_CASE("glyph source: loads a face and answers coverage")
{
    const std::vector<std::byte> font = ReadFixtureFont();
    GlyphSource source;

    const Result<FaceId> face = source.LoadFace(font);
    REQUIRE_MESSAGE(face.has_value(), face.error());

    // 'A' and space are in the four-glyph fixture; a letter it does not carry is not.
    CHECK(source.HasGlyph(*face, 'A'));
    CHECK(source.HasGlyph(*face, ' '));
    CHECK_FALSE(source.HasGlyph(*face, 'Z'));
    CHECK_FALSE(
        source.HasGlyph(*face, 0x4E00)); // CJK 'one' — absent, so the fallback chain moves on

    // A covered codepoint resolves to a real glyph index; an uncovered one is .notdef (0).
    CHECK(source.GlyphIndex(*face, 'A') != 0);
    CHECK(source.GlyphIndex(*face, 'Z') == 0);

    const FaceMetrics metrics = source.GetFaceMetrics(*face);
    CHECK(metrics.UnitsPerEm == doctest::Approx(1000.0f));
    CHECK(metrics.Ascender > 0.0f);
    CHECK(metrics.Descender < 0.0f);
    CHECK(metrics.LineHeight > 0.0f);
}

TEST_CASE("glyph source: rasterizes MSDF and SDF with matching metrics")
{
    const std::vector<std::byte> font = ReadFixtureFont();
    GlyphSource source;
    const Result<FaceId> face = source.LoadFace(font);
    REQUIRE(face.has_value());
    const u32 glyphA = source.GlyphIndex(*face, 'A');
    REQUIRE(glyphA != 0);

    constexpr f32 pixelSize = 32.0f;
    constexpr u32 distanceRange = 4;

    SUBCASE("MSDF returns four channels")
    {
        const Result<RasterizedGlyph> glyph =
            source.Rasterize(*face, glyphA, pixelSize, GlyphFieldType::Msdf, distanceRange);
        REQUIRE_MESSAGE(glyph.has_value(), glyph.error());
        CHECK(glyph->FieldType == GlyphFieldType::Msdf);
        CHECK(glyph->Channels == 4);
        CHECK(glyph->Width > 0);
        CHECK(glyph->Height > 0);
        CHECK(glyph->Pixels.size() ==
              static_cast<usize>(glyph->Width) * glyph->Height * glyph->Channels);
        // A non-degenerate quad, em-space, y-up.
        CHECK(glyph->PlaneMax.x > glyph->PlaneMin.x);
        CHECK(glyph->PlaneMax.y > glyph->PlaneMin.y);
        // The advance is a size-independent face property.
        CHECK(glyph->Advance == doctest::Approx(AdvanceA).epsilon(0.02f));
    }

    SUBCASE("SDF returns one channel")
    {
        const Result<RasterizedGlyph> glyph =
            source.Rasterize(*face, glyphA, pixelSize, GlyphFieldType::Sdf, distanceRange);
        REQUIRE_MESSAGE(glyph.has_value(), glyph.error());
        CHECK(glyph->FieldType == GlyphFieldType::Sdf);
        CHECK(glyph->Channels == 1);
        CHECK(glyph->Width > 0);
        CHECK(glyph->Height > 0);
        CHECK(glyph->Pixels.size() == static_cast<usize>(glyph->Width) * glyph->Height);
        CHECK(glyph->PlaneMax.x > glyph->PlaneMin.x);
        CHECK(glyph->PlaneMax.y > glyph->PlaneMin.y);
        CHECK(glyph->Advance == doctest::Approx(AdvanceA).epsilon(0.02f));
    }
}

TEST_CASE("glyph source: kerning matches the face")
{
    const std::vector<std::byte> font = ReadFixtureFont();
    GlyphSource source;
    const Result<FaceId> face = source.LoadFace(font);
    REQUIRE(face.has_value());

    const u32 a = source.GlyphIndex(*face, 'A');
    const u32 v = source.GlyphIndex(*face, 'V');
    REQUIRE(a != 0);
    REQUIRE(v != 0);

    // The authored AV pair round-trips as an em-normalized adjustment.
    CHECK(source.GetKerning(*face, a, v) == doctest::Approx(KernAV).epsilon(0.02f));
    // An un-kerned pair reports zero.
    CHECK(source.GetKerning(*face, a, a) == doctest::Approx(0.0f));
}

TEST_CASE("glyph field type reflects its enumerator names")
{
    CHECK(EnumeratorName(GlyphFieldType::Msdf) == "Msdf");
    CHECK(EnumeratorName(GlyphFieldType::Sdf) == "Sdf");
    CHECK(ParseEnum<GlyphFieldType>("Sdf") == GlyphFieldType::Sdf);
    CHECK(ParseEnum<GlyphFieldType>("Msdf") == GlyphFieldType::Msdf);
}
