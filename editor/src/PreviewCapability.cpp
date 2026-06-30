#include "PreviewCapability.h"

#include <Veng/Project/CompressionRole.h>
#include <Veng/Renderer/Context.h>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The codec family a format belongs to, the granularity the device features gate at.
        enum class Codec
        {
            Uncompressed,
            BC,
            ASTC
        };

        Codec CodecOf(CompressionFormat format)
        {
            switch (format)
            {
            case CompressionFormat::BC7Unorm:
            case CompressionFormat::BC7Srgb:
            case CompressionFormat::BC5Unorm:
            case CompressionFormat::BC4Unorm:
                return Codec::BC;
            case CompressionFormat::ASTC4x4Unorm:
            case CompressionFormat::ASTC4x4Srgb:
                return Codec::ASTC;
            case CompressionFormat::RGBA8Unorm:
            case CompressionFormat::RGBA8Srgb:
            case CompressionFormat::RGBA16Sfloat:
                return Codec::Uncompressed;
            }
            return Codec::Uncompressed;
        }

        // Reads the format a role resolves to from the fixed RoleToFormat record.
        CompressionFormat RoleFormat(const RoleToFormat& table, CompressionRole role)
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

    bool IsFormatPreviewable(CompressionFormat format, const Renderer::Context& context)
    {
        switch (CodecOf(format))
        {
        case Codec::Uncompressed:
            return true;
        case Codec::BC:
            return context.IsBlockCompressionSupported();
        case Codec::ASTC:
            return context.IsAstcSupported();
        }
        return false;
    }

    PreviewCapability IsConfigPreviewable(const BuildConfiguration& config,
                                          const Renderer::Context& context)
    {
        for (const CompressionRole role : CompressionRoles)
        {
            const CompressionFormat format = RoleFormat(config.Formats, role);
            if (IsFormatPreviewable(format, context))
            {
                continue;
            }

            const string_view codec =
                CodecOf(format) == Codec::ASTC ? "ASTC" : "block compression (BC)";
            return {.Previewable = false,
                    .Reason =
                        fmt::format("not previewable: this GPU lacks {}. Build-only.", codec)};
        }
        return {.Previewable = true, .Reason = {}};
    }

    RoleToFormat HostSafeFormats()
    {
        // Uncompressed formats sample on every device, so this profile is always previewable
        // regardless of which block codecs the host enables.
        return RoleToFormat{
            .Color = CompressionFormat::RGBA8Srgb,
            .Normal = CompressionFormat::RGBA8Unorm,
            .Mask = CompressionFormat::RGBA8Unorm,
            .HDR = CompressionFormat::RGBA16Sfloat,
            .UI = CompressionFormat::RGBA8Unorm,
        };
    }

    BuildConfiguration HostSafeConfiguration()
    {
        BuildConfiguration config;
        config.Name = "preview (host-safe)";
        config.Target = "preview";
        config.Formats = HostSafeFormats();
        config.OutputSuffix = "-preview";
        return config;
    }
}
