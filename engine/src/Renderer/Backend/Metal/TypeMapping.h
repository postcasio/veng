#pragma once

// The Metal backend's mapping from the engine's vocabulary enums. Every switch is exhaustive and
// asserts on a value it does not map, so a new enumerator is a loud one-line fix.

#include "Metal.h"

#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer::Metal
{
    /// @brief The Metal pixel format of an engine format, or nullopt where Apple silicon has none.
    ///
    /// D16UnormS8Uint, D24UnormS8Uint and X8D24UnormPack32 have no Metal equivalent on Apple
    /// silicon, and RGB32Sfloat is a vertex format with no texture form.
    /// @param format The engine format.
    /// @return The pixel format, or nullopt.
    [[nodiscard]] inline optional<MTLPixelFormat> ToMetal(const Format format)
    {
        switch (format)
        {
        case Format::Undefined:
            return MTLPixelFormatInvalid;
        case Format::R8Unorm:
            return MTLPixelFormatR8Unorm;
        case Format::RGBA8Unorm:
            return MTLPixelFormatRGBA8Unorm;
        case Format::RGBA8Srgb:
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case Format::BGRA8Srgb:
            return MTLPixelFormatBGRA8Unorm_sRGB;
        case Format::R16Sfloat:
            return MTLPixelFormatR16Float;
        case Format::RGBA16Sfloat:
            return MTLPixelFormatRGBA16Float;
        case Format::R32Sfloat:
            return MTLPixelFormatR32Float;
        case Format::RG32Sfloat:
            return MTLPixelFormatRG32Float;
        case Format::RGB32Sfloat:
            return std::nullopt;
        case Format::RGBA32Sfloat:
            return MTLPixelFormatRGBA32Float;
        case Format::D16Unorm:
            return MTLPixelFormatDepth16Unorm;
        case Format::D32Sfloat:
            return MTLPixelFormatDepth32Float;
        case Format::S8Uint:
            return MTLPixelFormatStencil8;
        case Format::D16UnormS8Uint:
        case Format::D24UnormS8Uint:
        case Format::X8D24UnormPack32:
            return std::nullopt;
        case Format::D32SfloatS8Uint:
            return MTLPixelFormatDepth32Float_Stencil8;
        case Format::RG16Sfloat:
            return MTLPixelFormatRG16Float;
        case Format::A2B10G10R10Unorm:
            return MTLPixelFormatRGB10A2Unorm;
        case Format::RGBA16Uint:
            return MTLPixelFormatRGBA16Uint;
        case Format::BC7Unorm:
            return MTLPixelFormatBC7_RGBAUnorm;
        case Format::BC7Srgb:
            return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        case Format::ASTC4x4Unorm:
            return MTLPixelFormatASTC_4x4_LDR;
        case Format::ASTC4x4Srgb:
            return MTLPixelFormatASTC_4x4_sRGB;
        case Format::R32Uint:
            return MTLPixelFormatR32Uint;
        case Format::BC5Unorm:
            return MTLPixelFormatBC5_RGUnorm;
        case Format::BC4Unorm:
            return MTLPixelFormatBC4_RUnorm;
        case Format::B10G11R11Ufloat:
            return MTLPixelFormatRG11B10Float;
        case Format::RG8Unorm:
            return MTLPixelFormatRG8Unorm;
        case Format::RGBA16Unorm:
            return MTLPixelFormatRGBA16Unorm;
        case Format::A2R10G10B10Unorm:
            return MTLPixelFormatBGR10A2Unorm;
        case Format::BC6HUfloat:
            return MTLPixelFormatBC6H_RGBUfloat;
        case Format::RG16Unorm:
            return MTLPixelFormatRG16Unorm;
        }
        VE_ASSERT(false, "ToMetal(Format): unmapped format {}", static_cast<u32>(format));
    }

    /// @brief The Metal pixel format of an engine format, fatal where Apple silicon has none.
    /// @param format The engine format.
    /// @param what   What the format is for, for the message.
    /// @return The pixel format.
    [[nodiscard]] inline MTLPixelFormat RequireMetal(const Format format, const string_view what)
    {
        const optional<MTLPixelFormat> mapped = ToMetal(format);
        VE_ASSERT(mapped.has_value(), "{}: Apple silicon has no Metal pixel format for {}", what,
                  FormatName(format));
        return *mapped;
    }

    /// @brief Whether an engine format is a depth or stencil format.
    [[nodiscard]] constexpr bool IsDepthStencil(const Format format)
    {
        switch (format)
        {
        case Format::D16Unorm:
        case Format::D32Sfloat:
        case Format::S8Uint:
        case Format::D16UnormS8Uint:
        case Format::D24UnormS8Uint:
        case Format::D32SfloatS8Uint:
        case Format::X8D24UnormPack32:
            return true;
        default:
            return false;
        }
    }

    /// @brief Whether an engine format is block-compressed.
    [[nodiscard]] constexpr bool IsCompressed(const Format format)
    {
        return GetFormatBlockInfo(format).BlockWidth > 1;
    }

    /// @brief The texture type of a whole image.
    /// @param type   The image's dimensionality.
    /// @param layers The image's array layers.
    /// @return The texture type.
    [[nodiscard]] inline MTLTextureType ToMetalTextureType(const ImageType type, const u32 layers)
    {
        switch (type)
        {
        case ImageType::Type1D:
            return layers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
        case ImageType::Type2D:
            return layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        case ImageType::Type3D:
            return MTLTextureType3D;
        }
        VE_ASSERT(false, "ToMetalTextureType: unmapped image type {}", static_cast<u32>(type));
    }

    /// @brief The texture type of a view.
    [[nodiscard]] inline MTLTextureType ToMetal(const ImageViewType type)
    {
        switch (type)
        {
        case ImageViewType::Type1D:
            return MTLTextureType1D;
        case ImageViewType::Type2D:
            return MTLTextureType2D;
        case ImageViewType::Type3D:
            return MTLTextureType3D;
        case ImageViewType::Cube:
            return MTLTextureTypeCube;
        case ImageViewType::Array1D:
            return MTLTextureType1DArray;
        case ImageViewType::Array2D:
            return MTLTextureType2DArray;
        case ImageViewType::CubeArray:
            return MTLTextureTypeCubeArray;
        }
        VE_ASSERT(false, "ToMetal(ImageViewType): unmapped view type {}", static_cast<u32>(type));
    }

    /// @brief The minification or magnification filter.
    [[nodiscard]] inline MTLSamplerMinMagFilter ToMetal(const Filter filter)
    {
        switch (filter)
        {
        case Filter::Nearest:
            return MTLSamplerMinMagFilterNearest;
        case Filter::Linear:
            return MTLSamplerMinMagFilterLinear;
        }
        VE_ASSERT(false, "ToMetal(Filter): unmapped filter {}", static_cast<u32>(filter));
    }

    /// @brief The mip filter.
    [[nodiscard]] inline MTLSamplerMipFilter ToMetal(const MipmapMode mode)
    {
        switch (mode)
        {
        case MipmapMode::Nearest:
            return MTLSamplerMipFilterNearest;
        case MipmapMode::Linear:
            return MTLSamplerMipFilterLinear;
        }
        VE_ASSERT(false, "ToMetal(MipmapMode): unmapped mode {}", static_cast<u32>(mode));
    }

    /// @brief The address mode.
    [[nodiscard]] inline MTLSamplerAddressMode ToMetal(const AddressMode mode)
    {
        switch (mode)
        {
        case AddressMode::Repeat:
            return MTLSamplerAddressModeRepeat;
        case AddressMode::MirroredRepeat:
            return MTLSamplerAddressModeMirrorRepeat;
        case AddressMode::ClampToEdge:
            return MTLSamplerAddressModeClampToEdge;
        case AddressMode::ClampToBorder:
            return MTLSamplerAddressModeClampToBorderColor;
        }
        VE_ASSERT(false, "ToMetal(AddressMode): unmapped mode {}", static_cast<u32>(mode));
    }

    /// @brief The border colour.
    [[nodiscard]] inline MTLSamplerBorderColor ToMetal(const BorderColor color)
    {
        switch (color)
        {
        case BorderColor::TransparentBlack:
            return MTLSamplerBorderColorTransparentBlack;
        case BorderColor::OpaqueBlack:
            return MTLSamplerBorderColorOpaqueBlack;
        case BorderColor::OpaqueWhite:
            return MTLSamplerBorderColorOpaqueWhite;
        }
        VE_ASSERT(false, "ToMetal(BorderColor): unmapped colour {}", static_cast<u32>(color));
    }

    /// @brief The comparison function.
    [[nodiscard]] inline MTLCompareFunction ToMetal(const CompareOp op)
    {
        switch (op)
        {
        case CompareOp::Never:
            return MTLCompareFunctionNever;
        case CompareOp::Less:
            return MTLCompareFunctionLess;
        case CompareOp::Equal:
            return MTLCompareFunctionEqual;
        case CompareOp::LessOrEqual:
            return MTLCompareFunctionLessEqual;
        case CompareOp::Greater:
            return MTLCompareFunctionGreater;
        case CompareOp::NotEqual:
            return MTLCompareFunctionNotEqual;
        case CompareOp::GreaterOrEqual:
            return MTLCompareFunctionGreaterEqual;
        case CompareOp::Always:
            return MTLCompareFunctionAlways;
        }
        VE_ASSERT(false, "ToMetal(CompareOp): unmapped op {}", static_cast<u32>(op));
    }
}
