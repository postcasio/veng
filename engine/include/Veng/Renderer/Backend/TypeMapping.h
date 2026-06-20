#pragma once

// Internal backend boundary: maps engine vocabulary types (Renderer/Types.h) to
// and from Vulkan. Not part of the public API — only backend .cpp files include
// this. Switches are exhaustive; an unmapped value is a fatal assert so gaps are
// loud and the fix is one line.

#include <array>

#include <Veng/Assert.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    inline vk::Format ToVk(Format format)
    {
        switch (format)
        {
        case Format::Undefined:
            return vk::Format::eUndefined;
        case Format::R8Unorm:
            return vk::Format::eR8Unorm;
        case Format::RGBA8Unorm:
            return vk::Format::eR8G8B8A8Unorm;
        case Format::RGBA8Srgb:
            return vk::Format::eR8G8B8A8Srgb;
        case Format::BGRA8Srgb:
            return vk::Format::eB8G8R8A8Srgb;
        case Format::R16Sfloat:
            return vk::Format::eR16Sfloat;
        case Format::RGBA16Sfloat:
            return vk::Format::eR16G16B16A16Sfloat;
        case Format::R32Sfloat:
            return vk::Format::eR32Sfloat;
        case Format::RG32Sfloat:
            return vk::Format::eR32G32Sfloat;
        case Format::RGB32Sfloat:
            return vk::Format::eR32G32B32Sfloat;
        case Format::RGBA32Sfloat:
            return vk::Format::eR32G32B32A32Sfloat;
        case Format::D16Unorm:
            return vk::Format::eD16Unorm;
        case Format::D32Sfloat:
            return vk::Format::eD32Sfloat;
        case Format::S8Uint:
            return vk::Format::eS8Uint;
        case Format::D16UnormS8Uint:
            return vk::Format::eD16UnormS8Uint;
        case Format::D24UnormS8Uint:
            return vk::Format::eD24UnormS8Uint;
        case Format::D32SfloatS8Uint:
            return vk::Format::eD32SfloatS8Uint;
        case Format::X8D24UnormPack32:
            return vk::Format::eX8D24UnormPack32;
        }
        VE_ASSERT(false, "ToVk(Format): unmapped format {}", static_cast<u32>(format));
    }

    inline Format FromVk(vk::Format format)
    {
        switch (format)
        {
        case vk::Format::eUndefined:
            return Format::Undefined;
        case vk::Format::eR8Unorm:
            return Format::R8Unorm;
        case vk::Format::eR8G8B8A8Unorm:
            return Format::RGBA8Unorm;
        case vk::Format::eR8G8B8A8Srgb:
            return Format::RGBA8Srgb;
        case vk::Format::eB8G8R8A8Srgb:
            return Format::BGRA8Srgb;
        case vk::Format::eR16Sfloat:
            return Format::R16Sfloat;
        case vk::Format::eR16G16B16A16Sfloat:
            return Format::RGBA16Sfloat;
        case vk::Format::eR32Sfloat:
            return Format::R32Sfloat;
        case vk::Format::eR32G32Sfloat:
            return Format::RG32Sfloat;
        case vk::Format::eR32G32B32Sfloat:
            return Format::RGB32Sfloat;
        case vk::Format::eR32G32B32A32Sfloat:
            return Format::RGBA32Sfloat;
        case vk::Format::eD16Unorm:
            return Format::D16Unorm;
        case vk::Format::eD32Sfloat:
            return Format::D32Sfloat;
        case vk::Format::eS8Uint:
            return Format::S8Uint;
        case vk::Format::eD16UnormS8Uint:
            return Format::D16UnormS8Uint;
        case vk::Format::eD24UnormS8Uint:
            return Format::D24UnormS8Uint;
        case vk::Format::eD32SfloatS8Uint:
            return Format::D32SfloatS8Uint;
        case vk::Format::eX8D24UnormPack32:
            return Format::X8D24UnormPack32;
        default:
            VE_ASSERT(false, "FromVk(vk::Format): unmapped format {}",
                      string_VkFormat(static_cast<VkFormat>(format)));
        }
    }

    inline vk::ImageType ToVk(ImageType type)
    {
        switch (type)
        {
        case ImageType::Type1D:
            return vk::ImageType::e1D;
        case ImageType::Type2D:
            return vk::ImageType::e2D;
        case ImageType::Type3D:
            return vk::ImageType::e3D;
        }
        VE_ASSERT(false, "ToVk(ImageType): unmapped value {}", static_cast<u32>(type));
    }

    inline vk::ImageLayout ToVk(ImageLayout layout)
    {
        switch (layout)
        {
        case ImageLayout::Undefined:
            return vk::ImageLayout::eUndefined;
        case ImageLayout::General:
            return vk::ImageLayout::eGeneral;
        case ImageLayout::ColorAttachment:
            return vk::ImageLayout::eColorAttachmentOptimal;
        case ImageLayout::DepthAttachment:
            return vk::ImageLayout::eDepthStencilAttachmentOptimal;
        case ImageLayout::ShaderReadOnly:
            return vk::ImageLayout::eShaderReadOnlyOptimal;
        case ImageLayout::TransferSrc:
            return vk::ImageLayout::eTransferSrcOptimal;
        case ImageLayout::TransferDst:
            return vk::ImageLayout::eTransferDstOptimal;
        case ImageLayout::PresentSrc:
            return vk::ImageLayout::ePresentSrcKHR;
        }
        VE_ASSERT(false, "ToVk(ImageLayout): unmapped value {}", static_cast<u32>(layout));
    }

    inline vk::IndexType ToVk(IndexType type)
    {
        switch (type)
        {
        case IndexType::U16:
            return vk::IndexType::eUint16;
        case IndexType::U32:
            return vk::IndexType::eUint32;
        }
        VE_ASSERT(false, "ToVk(IndexType): unmapped value {}", static_cast<u32>(type));
    }

    inline ImageLayout FromVk(vk::ImageLayout layout)
    {
        switch (layout)
        {
        case vk::ImageLayout::eUndefined:
            return ImageLayout::Undefined;
        case vk::ImageLayout::eGeneral:
            return ImageLayout::General;
        case vk::ImageLayout::eColorAttachmentOptimal:
            return ImageLayout::ColorAttachment;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            return ImageLayout::DepthAttachment;
        case vk::ImageLayout::eShaderReadOnlyOptimal:
            return ImageLayout::ShaderReadOnly;
        case vk::ImageLayout::eTransferSrcOptimal:
            return ImageLayout::TransferSrc;
        case vk::ImageLayout::eTransferDstOptimal:
            return ImageLayout::TransferDst;
        case vk::ImageLayout::ePresentSrcKHR:
            return ImageLayout::PresentSrc;
        default:
            VE_ASSERT(false, "FromVk(vk::ImageLayout): unmapped value {}",
                      string_VkImageLayout(static_cast<VkImageLayout>(layout)));
        }
    }

    inline vk::ImageViewType ToVk(ImageViewType type)
    {
        switch (type)
        {
        case ImageViewType::Type1D:
            return vk::ImageViewType::e1D;
        case ImageViewType::Type2D:
            return vk::ImageViewType::e2D;
        case ImageViewType::Type3D:
            return vk::ImageViewType::e3D;
        case ImageViewType::Cube:
            return vk::ImageViewType::eCube;
        case ImageViewType::Array1D:
            return vk::ImageViewType::e1DArray;
        case ImageViewType::Array2D:
            return vk::ImageViewType::e2DArray;
        case ImageViewType::CubeArray:
            return vk::ImageViewType::eCubeArray;
        }
        VE_ASSERT(false, "ToVk(ImageViewType): unmapped value {}", static_cast<u32>(type));
    }

    inline vk::CommandBufferLevel ToVk(CommandBufferLevel level)
    {
        switch (level)
        {
        case CommandBufferLevel::Primary:
            return vk::CommandBufferLevel::ePrimary;
        case CommandBufferLevel::Secondary:
            return vk::CommandBufferLevel::eSecondary;
        }
        VE_ASSERT(false, "ToVk(CommandBufferLevel): unmapped value {}", static_cast<u32>(level));
    }

    inline vk::CommandBufferUsageFlags ToVk(CommandBufferUsage usage)
    {
        vk::CommandBufferUsageFlags flags{};
        if (HasFlag(usage, CommandBufferUsage::OneTimeSubmit))
            flags |= vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        return flags;
    }

    inline vk::ImageUsageFlags ToVk(ImageUsage usage)
    {
        vk::ImageUsageFlags flags{};
        if (HasFlag(usage, ImageUsage::Sampled))
            flags |= vk::ImageUsageFlagBits::eSampled;
        if (HasFlag(usage, ImageUsage::Storage))
            flags |= vk::ImageUsageFlagBits::eStorage;
        if (HasFlag(usage, ImageUsage::ColorAttachment))
            flags |= vk::ImageUsageFlagBits::eColorAttachment;
        if (HasFlag(usage, ImageUsage::DepthAttachment))
            flags |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        if (HasFlag(usage, ImageUsage::TransferSrc))
            flags |= vk::ImageUsageFlagBits::eTransferSrc;
        if (HasFlag(usage, ImageUsage::TransferDst))
            flags |= vk::ImageUsageFlagBits::eTransferDst;
        return flags;
    }

    inline vk::BufferUsageFlags ToVk(BufferUsage usage)
    {
        vk::BufferUsageFlags flags{};
        if (HasFlag(usage, BufferUsage::Vertex))
            flags |= vk::BufferUsageFlagBits::eVertexBuffer;
        if (HasFlag(usage, BufferUsage::Index))
            flags |= vk::BufferUsageFlagBits::eIndexBuffer;
        if (HasFlag(usage, BufferUsage::Uniform))
            flags |= vk::BufferUsageFlagBits::eUniformBuffer;
        if (HasFlag(usage, BufferUsage::Storage))
            flags |= vk::BufferUsageFlagBits::eStorageBuffer;
        if (HasFlag(usage, BufferUsage::TransferSrc))
            flags |= vk::BufferUsageFlagBits::eTransferSrc;
        if (HasFlag(usage, BufferUsage::TransferDst))
            flags |= vk::BufferUsageFlagBits::eTransferDst;
        return flags;
    }

    inline vk::ShaderStageFlags ToVk(ShaderStage stage)
    {
        if (stage == ShaderStage::All)
            return vk::ShaderStageFlagBits::eAll;
        vk::ShaderStageFlags flags{};
        if (HasFlag(stage, ShaderStage::Vertex))
            flags |= vk::ShaderStageFlagBits::eVertex;
        if (HasFlag(stage, ShaderStage::Fragment))
            flags |= vk::ShaderStageFlagBits::eFragment;
        if (HasFlag(stage, ShaderStage::Compute))
            flags |= vk::ShaderStageFlagBits::eCompute;
        return flags;
    }

    /// @brief Maps a single ShaderStage enumerator to a vk::ShaderStageFlagBits.
    ///
    /// Asserts if @p stage is a combined flag set rather than a single stage.
    inline vk::ShaderStageFlagBits ToVkBit(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex:
            return vk::ShaderStageFlagBits::eVertex;
        case ShaderStage::Fragment:
            return vk::ShaderStageFlagBits::eFragment;
        case ShaderStage::Compute:
            return vk::ShaderStageFlagBits::eCompute;
        default:
            VE_ASSERT(false, "ToVkBit(ShaderStage): not a single stage ({})",
                      static_cast<u32>(stage));
        }
    }

    inline vk::AttachmentLoadOp ToVk(LoadOp op)
    {
        switch (op)
        {
        case LoadOp::Load:
            return vk::AttachmentLoadOp::eLoad;
        case LoadOp::Clear:
            return vk::AttachmentLoadOp::eClear;
        case LoadOp::DontCare:
            return vk::AttachmentLoadOp::eDontCare;
        }
        VE_ASSERT(false, "ToVk(LoadOp): unmapped value {}", static_cast<u32>(op));
    }

    inline vk::AttachmentStoreOp ToVk(StoreOp op)
    {
        switch (op)
        {
        case StoreOp::Store:
            return vk::AttachmentStoreOp::eStore;
        case StoreOp::DontCare:
            return vk::AttachmentStoreOp::eDontCare;
        }
        VE_ASSERT(false, "ToVk(StoreOp): unmapped value {}", static_cast<u32>(op));
    }

    inline vk::CompareOp ToVk(CompareOp op)
    {
        switch (op)
        {
        case CompareOp::Never:
            return vk::CompareOp::eNever;
        case CompareOp::Less:
            return vk::CompareOp::eLess;
        case CompareOp::Equal:
            return vk::CompareOp::eEqual;
        case CompareOp::LessOrEqual:
            return vk::CompareOp::eLessOrEqual;
        case CompareOp::Greater:
            return vk::CompareOp::eGreater;
        case CompareOp::NotEqual:
            return vk::CompareOp::eNotEqual;
        case CompareOp::GreaterOrEqual:
            return vk::CompareOp::eGreaterOrEqual;
        case CompareOp::Always:
            return vk::CompareOp::eAlways;
        }
        VE_ASSERT(false, "ToVk(CompareOp): unmapped value {}", static_cast<u32>(op));
    }

    inline vk::CullModeFlags ToVk(CullMode mode)
    {
        switch (mode)
        {
        case CullMode::None:
            return vk::CullModeFlagBits::eNone;
        case CullMode::Front:
            return vk::CullModeFlagBits::eFront;
        case CullMode::Back:
            return vk::CullModeFlagBits::eBack;
        case CullMode::FrontAndBack:
            return vk::CullModeFlagBits::eFrontAndBack;
        }
        VE_ASSERT(false, "ToVk(CullMode): unmapped value {}", static_cast<u32>(mode));
    }

    inline vk::PolygonMode ToVk(PolygonMode mode)
    {
        switch (mode)
        {
        case PolygonMode::Fill:
            return vk::PolygonMode::eFill;
        case PolygonMode::Line:
            return vk::PolygonMode::eLine;
        case PolygonMode::Point:
            return vk::PolygonMode::ePoint;
        }
        VE_ASSERT(false, "ToVk(PolygonMode): unmapped value {}", static_cast<u32>(mode));
    }

    inline vk::Filter ToVk(Filter filter)
    {
        switch (filter)
        {
        case Filter::Nearest:
            return vk::Filter::eNearest;
        case Filter::Linear:
            return vk::Filter::eLinear;
        }
        VE_ASSERT(false, "ToVk(Filter): unmapped value {}", static_cast<u32>(filter));
    }

    inline vk::SamplerMipmapMode ToVk(MipmapMode mode)
    {
        switch (mode)
        {
        case MipmapMode::Nearest:
            return vk::SamplerMipmapMode::eNearest;
        case MipmapMode::Linear:
            return vk::SamplerMipmapMode::eLinear;
        }
        VE_ASSERT(false, "ToVk(MipmapMode): unmapped value {}", static_cast<u32>(mode));
    }

    inline vk::SamplerAddressMode ToVk(AddressMode mode)
    {
        switch (mode)
        {
        case AddressMode::Repeat:
            return vk::SamplerAddressMode::eRepeat;
        case AddressMode::MirroredRepeat:
            return vk::SamplerAddressMode::eMirroredRepeat;
        case AddressMode::ClampToEdge:
            return vk::SamplerAddressMode::eClampToEdge;
        case AddressMode::ClampToBorder:
            return vk::SamplerAddressMode::eClampToBorder;
        }
        VE_ASSERT(false, "ToVk(AddressMode): unmapped value {}", static_cast<u32>(mode));
    }

    inline vk::BorderColor ToVk(BorderColor color)
    {
        switch (color)
        {
        case BorderColor::TransparentBlack:
            return vk::BorderColor::eFloatTransparentBlack;
        case BorderColor::OpaqueBlack:
            return vk::BorderColor::eIntOpaqueBlack;
        case BorderColor::OpaqueWhite:
            return vk::BorderColor::eFloatOpaqueWhite;
        }
        VE_ASSERT(false, "ToVk(BorderColor): unmapped value {}", static_cast<u32>(color));
    }

    inline vk::PipelineBindPoint ToVk(PipelineBindPoint point)
    {
        switch (point)
        {
        case PipelineBindPoint::Graphics:
            return vk::PipelineBindPoint::eGraphics;
        case PipelineBindPoint::Compute:
            return vk::PipelineBindPoint::eCompute;
        }
        VE_ASSERT(false, "ToVk(PipelineBindPoint): unmapped value {}", static_cast<u32>(point));
    }

    /// @brief Aggregates all per-DescriptorType properties into one structure.
    ///
    /// Single source of truth for DescriptorType ↔ Vulkan: the Vulkan descriptor
    /// type, the Primary Pool's per-type descriptor budget, and whether the type
    /// supports UpdateAfterBind (bindless) bindings. DescriptorSetLayout derives
    /// binding flags from SupportsBindless and Context sizes the Primary Pool from
    /// PoolBudget — adding a DescriptorType updates only GetDescriptorTypeInfo.
    struct DescriptorTypeInfo
    {
        /// @brief The corresponding Vulkan descriptor type.
        vk::DescriptorType VkType;
        /// @brief Maximum descriptors of this type in the Primary Pool.
        u32 PoolBudget;
        /// @brief Whether UpdateAfterBind is valid for this type.
        bool SupportsBindless;
    };

    /// @brief Returns the DescriptorTypeInfo for @p type.
    ///
    /// Asserts on an unmapped DescriptorType.
    inline DescriptorTypeInfo GetDescriptorTypeInfo(DescriptorType type)
    {
        switch (type)
        {
        case DescriptorType::CombinedImageSampler:
            return {vk::DescriptorType::eCombinedImageSampler, 10000, true};
        case DescriptorType::SampledImage:
            return {vk::DescriptorType::eSampledImage, 10000, true};
        case DescriptorType::StorageImage:
            return {vk::DescriptorType::eStorageImage, 10000, true};
        case DescriptorType::UniformBuffer:
            return {vk::DescriptorType::eUniformBuffer, 10000, true};
        case DescriptorType::StorageBuffer:
            return {vk::DescriptorType::eStorageBuffer, 10000, true};
        case DescriptorType::Sampler:
            return {vk::DescriptorType::eSampler, 1000, true};
        // A dynamic uniform buffer rebinds its region every frame, never
        // UpdateAfterBind — the per-frame ring selects the region with a bind-time
        // dynamic offset, so it does not opt into bindless.
        case DescriptorType::UniformBufferDynamic:
            return {vk::DescriptorType::eUniformBufferDynamic, 1000, false};
        }
        VE_ASSERT(false, "GetDescriptorTypeInfo: unmapped DescriptorType {}",
                  static_cast<u32>(type));
    }

    /// @brief All DescriptorType values, for iterating GetDescriptorTypeInfo
    /// (e.g. to build the Primary Pool's pool sizes).
    inline constexpr std::array AllDescriptorTypes = {
        DescriptorType::CombinedImageSampler, DescriptorType::SampledImage,
        DescriptorType::StorageImage,         DescriptorType::UniformBuffer,
        DescriptorType::StorageBuffer,        DescriptorType::Sampler,
        DescriptorType::UniformBufferDynamic,
    };

    inline vk::DescriptorType ToVk(DescriptorType type)
    {
        return GetDescriptorTypeInfo(type).VkType;
    }

    inline vk::BlendFactor ToVk(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero:
            return vk::BlendFactor::eZero;
        case BlendFactor::One:
            return vk::BlendFactor::eOne;
        case BlendFactor::SrcColor:
            return vk::BlendFactor::eSrcColor;
        case BlendFactor::OneMinusSrcColor:
            return vk::BlendFactor::eOneMinusSrcColor;
        case BlendFactor::SrcAlpha:
            return vk::BlendFactor::eSrcAlpha;
        case BlendFactor::OneMinusSrcAlpha:
            return vk::BlendFactor::eOneMinusSrcAlpha;
        case BlendFactor::DstAlpha:
            return vk::BlendFactor::eDstAlpha;
        case BlendFactor::OneMinusDstAlpha:
            return vk::BlendFactor::eOneMinusDstAlpha;
        }
        VE_ASSERT(false, "ToVk(BlendFactor): unmapped value {}", static_cast<u32>(factor));
    }

    inline vk::BlendOp ToVk(BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add:
            return vk::BlendOp::eAdd;
        case BlendOp::Subtract:
            return vk::BlendOp::eSubtract;
        case BlendOp::ReverseSubtract:
            return vk::BlendOp::eReverseSubtract;
        case BlendOp::Min:
            return vk::BlendOp::eMin;
        case BlendOp::Max:
            return vk::BlendOp::eMax;
        }
        VE_ASSERT(false, "ToVk(BlendOp): unmapped value {}", static_cast<u32>(op));
    }

    inline vk::PipelineColorBlendAttachmentState ToVk(const BlendState& blend)
    {
        return {
            .blendEnable = blend.Enable ? vk::True : vk::False,
            .srcColorBlendFactor = ToVk(blend.SrcColorFactor),
            .dstColorBlendFactor = ToVk(blend.DstColorFactor),
            .colorBlendOp = ToVk(blend.ColorOp),
            .srcAlphaBlendFactor = ToVk(blend.SrcAlphaFactor),
            .dstAlphaBlendFactor = ToVk(blend.DstAlphaFactor),
            .alphaBlendOp = ToVk(blend.AlphaOp),
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };
    }

    inline vk::ClearValue ToVk(const ClearValue& clear)
    {
        vk::ClearValue result{};
        if (const auto* color = std::get_if<ClearColor>(&clear))
        {
            result.color = vk::ClearColorValue{std::array{color->R, color->G, color->B, color->A}};
            return result;
        }
        const auto& depth = std::get<ClearDepth>(clear);
        result.depthStencil = vk::ClearDepthStencilValue{depth.Depth, depth.Stencil};
        return result;
    }
}
