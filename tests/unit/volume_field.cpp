// VolumeFieldData size/extent validation: the ExpectedByteSize depth-fold and the IsValid
// predicate the build asserts on. Pure integer arithmetic over FormatInfo, no ICD — a voxel span
// that disagrees with Resolution x per-texel size is rejected here before any GPU work.

#include <vector>

#include <doctest/doctest.h>

#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VolumeField.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("VolumeFieldData: ExpectedByteSize folds the depth axis over the per-texel size")
{
    // RGBA16Sfloat is 8 bytes per texel; a non-cubic extent so a dropped depth axis cannot hide.
    VolumeFieldData data;
    data.Resolution = {8, 4, 6};
    data.Format = Format::RGBA16Sfloat;
    CHECK(data.ExpectedByteSize() == static_cast<usize>(8) * 4 * 6 * 8);

    // RGBA8 is 4 bytes per texel.
    data.Format = Format::RGBA8Unorm;
    CHECK(data.ExpectedByteSize() == static_cast<usize>(8) * 4 * 6 * 4);

    // A single voxel is one texel's worth of bytes.
    data.Resolution = {1, 1, 1};
    data.Format = Format::RGBA16Sfloat;
    CHECK(data.ExpectedByteSize() == 8u);
}

TEST_CASE("VolumeFieldData: IsValid accepts a span that exactly fills the full extent")
{
    VolumeFieldData data;
    data.Resolution = {4, 4, 4};
    data.Format = Format::RGBA16Sfloat;

    const std::vector<u8> voxels(data.ExpectedByteSize(), 0);
    data.Voxels = voxels;
    CHECK(data.IsValid());
}

TEST_CASE("VolumeFieldData: IsValid rejects a mis-sized voxel span")
{
    VolumeFieldData data;
    data.Resolution = {4, 4, 4};
    data.Format = Format::RGBA16Sfloat;
    const usize expected = data.ExpectedByteSize();

    // One texel short and one texel long both fail — the span must match exactly.
    const std::vector<u8> tooSmall(expected - 8, 0);
    data.Voxels = tooSmall;
    CHECK_FALSE(data.IsValid());

    const std::vector<u8> tooLarge(expected + 8, 0);
    data.Voxels = tooLarge;
    CHECK_FALSE(data.IsValid());

    // An empty span against a non-empty extent is invalid.
    data.Voxels = {};
    CHECK_FALSE(data.IsValid());
}

TEST_CASE("VolumeFieldData: IsValid rejects a zero extent and an unsupported format")
{
    // A zero-extent axis makes the whole volume zero-sized: ExpectedByteSize is 0, so no span
    // (not even the empty one) is valid.
    VolumeFieldData zeroExtent;
    zeroExtent.Resolution = {4, 0, 4};
    zeroExtent.Format = Format::RGBA16Sfloat;
    CHECK(zeroExtent.ExpectedByteSize() == 0u);
    CHECK_FALSE(zeroExtent.IsValid());

    // A format FormatInfo does not size reports a zero per-texel size, so it rejects loudly rather
    // than copying a mis-sized staging buffer.
    VolumeFieldData unknownFormat;
    unknownFormat.Resolution = {4, 4, 4};
    unknownFormat.Format = Format::Undefined;
    CHECK(unknownFormat.ExpectedByteSize() == 0u);
    const std::vector<u8> some(256, 0);
    unknownFormat.Voxels = some;
    CHECK_FALSE(unknownFormat.IsValid());
}
