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
    /// @brief The vertex attribute format of an engine format.
    /// @param format The attribute's engine format.
    /// @return The vertex format.
    [[nodiscard]] inline MTLVertexFormat ToMetalVertexFormat(const Format format)
    {
        switch (format)
        {
        case Format::R32Sfloat:
            return MTLVertexFormatFloat;
        case Format::RG32Sfloat:
            return MTLVertexFormatFloat2;
        case Format::RGB32Sfloat:
            return MTLVertexFormatFloat3;
        case Format::RGBA32Sfloat:
            return MTLVertexFormatFloat4;
        case Format::R16Sfloat:
            return MTLVertexFormatHalf;
        case Format::RG16Sfloat:
            return MTLVertexFormatHalf2;
        case Format::RGBA16Sfloat:
            return MTLVertexFormatHalf4;
        case Format::R8Unorm:
            return MTLVertexFormatUCharNormalized;
        case Format::RG8Unorm:
            return MTLVertexFormatUChar2Normalized;
        case Format::RGBA8Unorm:
            return MTLVertexFormatUChar4Normalized;
        case Format::RG16Unorm:
            return MTLVertexFormatUShort2Normalized;
        case Format::RGBA16Unorm:
            return MTLVertexFormatUShort4Normalized;
        case Format::RGBA16Uint:
            return MTLVertexFormatUShort4;
        case Format::R32Uint:
            return MTLVertexFormatUInt;
        case Format::A2B10G10R10Unorm:
            return MTLVertexFormatUInt1010102Normalized;
        default:
            break;
        }
        VE_ASSERT(false, "ToMetalVertexFormat: {} is not a vertex attribute format",
                  FormatName(format));
        return MTLVertexFormatInvalid;
    }

    /// @brief The blend factor.
    [[nodiscard]] inline MTLBlendFactor ToMetal(const BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero:
            return MTLBlendFactorZero;
        case BlendFactor::One:
            return MTLBlendFactorOne;
        case BlendFactor::SrcColor:
            return MTLBlendFactorSourceColor;
        case BlendFactor::OneMinusSrcColor:
            return MTLBlendFactorOneMinusSourceColor;
        case BlendFactor::SrcAlpha:
            return MTLBlendFactorSourceAlpha;
        case BlendFactor::OneMinusSrcAlpha:
            return MTLBlendFactorOneMinusSourceAlpha;
        case BlendFactor::DstAlpha:
            return MTLBlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDstAlpha:
            return MTLBlendFactorOneMinusDestinationAlpha;
        case BlendFactor::ConstantColor:
            return MTLBlendFactorBlendColor;
        case BlendFactor::OneMinusConstantColor:
            return MTLBlendFactorOneMinusBlendColor;
        }
        VE_ASSERT(false, "ToMetal(BlendFactor): unmapped factor {}", static_cast<u32>(factor));
    }

    /// @brief The blend operation.
    [[nodiscard]] inline MTLBlendOperation ToMetal(const BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add:
            return MTLBlendOperationAdd;
        case BlendOp::Subtract:
            return MTLBlendOperationSubtract;
        case BlendOp::ReverseSubtract:
            return MTLBlendOperationReverseSubtract;
        case BlendOp::Min:
            return MTLBlendOperationMin;
        case BlendOp::Max:
            return MTLBlendOperationMax;
        }
        VE_ASSERT(false, "ToMetal(BlendOp): unmapped op {}", static_cast<u32>(op));
    }

    /// @brief The load action.
    [[nodiscard]] inline MTLLoadAction ToMetal(const LoadOp op)
    {
        switch (op)
        {
        case LoadOp::Load:
            return MTLLoadActionLoad;
        case LoadOp::Clear:
            return MTLLoadActionClear;
        case LoadOp::DontCare:
            return MTLLoadActionDontCare;
        }
        VE_ASSERT(false, "ToMetal(LoadOp): unmapped op {}", static_cast<u32>(op));
    }

    /// @brief The store action.
    [[nodiscard]] inline MTLStoreAction ToMetal(const StoreOp op)
    {
        switch (op)
        {
        case StoreOp::Store:
            return MTLStoreActionStore;
        case StoreOp::DontCare:
            return MTLStoreActionDontCare;
        }
        VE_ASSERT(false, "ToMetal(StoreOp): unmapped op {}", static_cast<u32>(op));
    }

    /// @brief The face culling mode; culling both faces is the pipeline's to do by not rasterizing.
    [[nodiscard]] inline MTLCullMode ToMetal(const CullMode mode)
    {
        switch (mode)
        {
        case CullMode::None:
        case CullMode::FrontAndBack:
            return MTLCullModeNone;
        case CullMode::Front:
            return MTLCullModeFront;
        case CullMode::Back:
            return MTLCullModeBack;
        }
        VE_ASSERT(false, "ToMetal(CullMode): unmapped mode {}", static_cast<u32>(mode));
    }

    /// @brief The primitive a topology draws.
    [[nodiscard]] inline MTLPrimitiveType ToMetal(const PrimitiveTopology topology)
    {
        switch (topology)
        {
        case PrimitiveTopology::TriangleList:
            return MTLPrimitiveTypeTriangle;
        case PrimitiveTopology::LineList:
            return MTLPrimitiveTypeLine;
        }
        VE_ASSERT(false, "ToMetal(PrimitiveTopology): unmapped topology {}",
                  static_cast<u32>(topology));
    }

    /// @brief The primitive class a topology's pipeline is built for.
    [[nodiscard]] inline MTLPrimitiveTopologyClass ToMetalClass(const PrimitiveTopology topology)
    {
        switch (topology)
        {
        case PrimitiveTopology::TriangleList:
            return MTLPrimitiveTopologyClassTriangle;
        case PrimitiveTopology::LineList:
            return MTLPrimitiveTopologyClassLine;
        }
        VE_ASSERT(false, "ToMetalClass(PrimitiveTopology): unmapped topology {}",
                  static_cast<u32>(topology));
    }

    /// @brief The index type.
    [[nodiscard]] inline MTLIndexType ToMetal(const IndexType type)
    {
        switch (type)
        {
        case IndexType::U16:
            return MTLIndexTypeUInt16;
        case IndexType::U32:
            return MTLIndexTypeUInt32;
        }
        VE_ASSERT(false, "ToMetal(IndexType): unmapped type {}", static_cast<u32>(type));
    }
}
