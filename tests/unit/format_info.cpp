// FormatInfo block-geometry test: the split GetFormatBlockInfo arms (BC4 = 8-byte 4x4 block, BC5
// joins the 16-byte arm) and the BytesForLevel ceil-divide over partial edge blocks. Pure integer
// arithmetic, no ICD. Plus FormatName's coverage of the declared enumerator set.

#include <doctest/doctest.h>

#include <Veng/Renderer/FormatInfo.h>

#include <set>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("FormatInfo: block geometry of the block-compressed formats")
{
    // BC7, BC5, and ASTC 4x4 are 16-byte 4x4 blocks; BC4 is the 8-byte half-rate arm.
    CHECK(GetFormatBlockInfo(Format::BC7Unorm).Bytes == 16u);
    CHECK(GetFormatBlockInfo(Format::ASTC4x4Unorm).Bytes == 16u);
    CHECK(GetFormatBlockInfo(Format::BC5Unorm).Bytes == 16u);
    CHECK(GetFormatBlockInfo(Format::BC4Unorm).Bytes == 8u);

    for (const Format format : {Format::BC5Unorm, Format::BC4Unorm})
    {
        CHECK(GetFormatBlockInfo(format).BlockWidth == 4u);
        CHECK(GetFormatBlockInfo(format).BlockHeight == 4u);
    }
}

TEST_CASE("FormatInfo: the sixteen-bit-per-channel RGBA formats cost eight bytes per texel")
{
    // An uncompressed format reports a 1x1 block whose Bytes is its bytes-per-texel. RGBA16Unorm
    // trades a half's magnitude-proportional ulp for uniform quantisation at no change in size, so
    // its cost matching RGBA16Sfloat's exactly is the claim worth pinning.
    for (const Format format : {Format::RGBA16Unorm, Format::RGBA16Sfloat, Format::RGBA16Uint})
    {
        CHECK(GetFormatBlockInfo(format).BlockWidth == 1u);
        CHECK(GetFormatBlockInfo(format).BlockHeight == 1u);
        CHECK(GetFormatBlockInfo(format).Bytes == 8u);
        CHECK(BytesForLevel(format, 4, 4) == 128u);
    }
}

TEST_CASE("FormatInfo: BytesForLevel over BC5 and BC4 block counts")
{
    // An 8x8 level is 2x2 = 4 blocks: 64 bytes for BC5 (16/block), 32 bytes for BC4 (8/block).
    CHECK(BytesForLevel(Format::BC5Unorm, 8, 8) == 64u);
    CHECK(BytesForLevel(Format::BC4Unorm, 8, 8) == 32u);

    // A 1x1 level still pays for one whole padded block.
    CHECK(BytesForLevel(Format::BC5Unorm, 1, 1) == 16u);
    CHECK(BytesForLevel(Format::BC4Unorm, 1, 1) == 8u);

    // A non-multiple-of-4 level rounds up to whole blocks: a 5x5 level is 2x2 = 4 blocks.
    CHECK(BytesForLevel(Format::BC5Unorm, 5, 5) == 64u);
    CHECK(BytesForLevel(Format::BC4Unorm, 5, 5) == 32u);

    // BC5 matches the BC7/ASTC 16-byte arm exactly; BC4 is half that.
    CHECK(BytesForLevel(Format::BC5Unorm, 16, 16) == BytesForLevel(Format::BC7Unorm, 16, 16));
    CHECK(BytesForLevel(Format::BC4Unorm, 16, 16) == BytesForLevel(Format::BC5Unorm, 16, 16) / 2);
}

TEST_CASE("FormatInfo: FormatName covers every declared enumerator, distinctly")
{
    // The property that matters is coverage: a format added to Types.h and left out of the switch
    // reports "Unknown", so a diagnostic silently stops naming it. Walking the declared range
    // catches that at the point the enumerator is added rather than the day someone reads a dump.
    // RGBA16Unorm is the last declared value; the loop is the whole closed set.
    constexpr auto Last = static_cast<u32>(Format::RGBA16Unorm);
    std::set<string_view> names;
    for (u32 value = 0; value <= Last; ++value)
    {
        const string_view name = FormatName(static_cast<Format>(value));
        CHECK(name != "Unknown");
        names.insert(name);
    }
    // Distinct, so a reported name identifies exactly one format.
    CHECK(names.size() == Last + 1);

    // The name is the C++ spelling, so a dump greps back to the declaration.
    CHECK(FormatName(Format::BC7Srgb) == "BC7Srgb");
    CHECK(FormatName(Format::Undefined) == "Undefined");

    // A value past the declared set is named rather than asserted — a diagnostic's contract.
    CHECK(FormatName(static_cast<Format>(Last + 1)) == "Unknown");
}
