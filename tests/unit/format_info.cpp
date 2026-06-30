// FormatInfo block-geometry test: the split GetFormatBlockInfo arms (BC4 = 8-byte 4x4 block, BC5
// joins the 16-byte arm) and the BytesForLevel ceil-divide over partial edge blocks. Pure integer
// arithmetic, no ICD.

#include <doctest/doctest.h>

#include <Veng/Renderer/FormatInfo.h>

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
