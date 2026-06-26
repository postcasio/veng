// Device-free cases for the live-preview gate's host-safe profile: the structural
// guarantee that the editor's default preview never names a block codec, so it is
// samplable on any GPU regardless of which compression features the host enables.
// The Context-dependent IsConfigPreviewable path is covered by the validation gate.

#include <doctest/doctest.h>

#include "PreviewCapability.h"

#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>

using namespace VengEditor;
using Veng::CompressionFormat;
using Veng::CompressionRole;
using Veng::CompressionRoles;

namespace
{
    // Whether a format is a block codec the host must explicitly enable to sample.
    bool IsBlockCodec(CompressionFormat format)
    {
        switch (format)
        {
        case CompressionFormat::BC7Unorm:
        case CompressionFormat::BC7Srgb:
        case CompressionFormat::ASTC4x4Unorm:
        case CompressionFormat::ASTC4x4Srgb:
            return true;
        case CompressionFormat::RGBA8Unorm:
        case CompressionFormat::RGBA8Srgb:
        case CompressionFormat::RGBA16Sfloat:
            return false;
        }
        return false;
    }

    CompressionFormat RoleFormat(const Veng::RoleToFormat& table, CompressionRole role)
    {
        switch (role)
        {
        case CompressionRole::Color:
            return table.Color;
        case CompressionRole::Normal:
            return table.Normal;
        case CompressionRole::Mask:
            return table.Mask;
        case CompressionRole::HDR:
            return table.HDR;
        case CompressionRole::UI:
            return table.UI;
        }
        return table.Color;
    }
}

TEST_CASE("host-safe formats name no block codec for any role")
{
    const Veng::RoleToFormat safe = HostSafeFormats();
    for (const CompressionRole role : CompressionRoles)
    {
        CAPTURE(static_cast<int>(role));
        CHECK_FALSE(IsBlockCodec(RoleFormat(safe, role)));
    }
}

TEST_CASE("host-safe configuration carries the host-safe profile")
{
    const Veng::BuildConfiguration config = HostSafeConfiguration();
    CHECK_FALSE(config.Name.empty());
    for (const CompressionRole role : CompressionRoles)
    {
        CAPTURE(static_cast<int>(role));
        CHECK_FALSE(IsBlockCodec(RoleFormat(config.Formats, role)));
    }
}

TEST_CASE("host-safe color stays sRGB and HDR stays float")
{
    // The safe profile preserves encoding intent so a preview's color space and dynamic range
    // match the ship output even though the codec is uncompressed.
    const Veng::RoleToFormat safe = HostSafeFormats();
    CHECK(safe.Color == CompressionFormat::RGBA8Srgb);
    CHECK(safe.HDR == CompressionFormat::RGBA16Sfloat);
}
