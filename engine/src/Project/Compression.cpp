#include <Veng/Project/CompressionRole.h>
#include <Veng/Project/CompressionFormat.h>

#include <Veng/Assert.h>

namespace Veng
{
    std::string_view ToString(CompressionRole role)
    {
        switch (role)
        {
        case CompressionRole::Color:
            return "Color";
        case CompressionRole::Normal:
            return "Normal";
        case CompressionRole::Mask:
            return "Mask";
        case CompressionRole::HDR:
            return "HDR";
        case CompressionRole::UI:
            return "UI";
        }
        return {};
    }

    optional<CompressionRole> ParseCompressionRole(std::string_view name)
    {
        for (const CompressionRole role : CompressionRoles)
        {
            if (ToString(role) == name)
            {
                return role;
            }
        }
        return std::nullopt;
    }

    std::string_view ToString(CompressionFormat format)
    {
        switch (format)
        {
        case CompressionFormat::RGBA8Unorm:
            return "RGBA8Unorm";
        case CompressionFormat::RGBA8Srgb:
            return "RGBA8Srgb";
        case CompressionFormat::BC7Unorm:
            return "BC7Unorm";
        case CompressionFormat::BC7Srgb:
            return "BC7Srgb";
        case CompressionFormat::ASTC4x4Unorm:
            return "ASTC4x4Unorm";
        case CompressionFormat::ASTC4x4Srgb:
            return "ASTC4x4Srgb";
        case CompressionFormat::RGBA16Sfloat:
            return "RGBA16Sfloat";
        case CompressionFormat::BC5Unorm:
            return "BC5Unorm";
        case CompressionFormat::BC4Unorm:
            return "BC4Unorm";
        }
        return {};
    }

    optional<CompressionFormat> ParseCompressionFormat(std::string_view name)
    {
        for (const CompressionFormat format : CompressionFormats)
        {
            if (ToString(format) == name)
            {
                return format;
            }
        }
        return std::nullopt;
    }

    Renderer::Format ToRendererFormat(CompressionFormat format)
    {
        switch (format)
        {
        case CompressionFormat::RGBA8Unorm:
            return Renderer::Format::RGBA8Unorm;
        case CompressionFormat::RGBA8Srgb:
            return Renderer::Format::RGBA8Srgb;
        case CompressionFormat::BC7Unorm:
            return Renderer::Format::BC7Unorm;
        case CompressionFormat::BC7Srgb:
            return Renderer::Format::BC7Srgb;
        case CompressionFormat::ASTC4x4Unorm:
            return Renderer::Format::ASTC4x4Unorm;
        case CompressionFormat::ASTC4x4Srgb:
            return Renderer::Format::ASTC4x4Srgb;
        case CompressionFormat::RGBA16Sfloat:
            return Renderer::Format::RGBA16Sfloat;
        case CompressionFormat::BC5Unorm:
            return Renderer::Format::BC5Unorm;
        case CompressionFormat::BC4Unorm:
            return Renderer::Format::BC4Unorm;
        }
        VE_ASSERT(false, "ToRendererFormat: unmapped CompressionFormat {}",
                  static_cast<u32>(format));
    }
}
