#include <Veng/Renderer/Backend/MetalInterop.h>

#import <Metal/Metal.h>

#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer::Backend
{
    namespace
    {
        /// @brief Maps a Metal pixel format to the engine format an imported image takes.
        ///
        /// The closed set an external render target can be: an 8-bit BGRA whose store applies the
        /// sRGB transfer, the ten-bit packed ARGB word, and half-float RGBA. Plain BGRA8Unorm is
        /// deliberately absent — a consumer wanting 8-bit standard range wants the sRGB store, and
        /// accepting the unorm twin would silently write linear values under an sRGB tag.
        optional<Renderer::Format> EngineFormatFor(const MTLPixelFormat format)
        {
            switch (format)
            {
            case MTLPixelFormatBGRA8Unorm_sRGB:
                return Renderer::Format::BGRA8Srgb;
            case MTLPixelFormatBGR10A2Unorm:
                return Renderer::Format::A2R10G10B10Unorm;
            case MTLPixelFormatRGBA16Float:
                return Renderer::Format::RGBA16Sfloat;
            default:
                return std::nullopt;
            }
        }
    }

    optional<ExternalTextureDescription> DescribeExternalTexture(void* texture)
    {
        id<MTLTexture> mtlTexture = (id<MTLTexture>)texture;
        if (mtlTexture == nil)
        {
            return std::nullopt;
        }

        const optional<Renderer::Format> format = EngineFormatFor([mtlTexture pixelFormat]);
        if (!format)
        {
            return std::nullopt;
        }

        return ExternalTextureDescription{
            .Extent = uvec2{static_cast<u32>([mtlTexture width]),
                            static_cast<u32>([mtlTexture height])},
            .Format = *format,
        };
    }

    Ref<Image> ImportExternalTexture(Context& context, void* texture, const string_view name)
    {
        if (!context.IsExternalTextureImportSupported())
        {
            Log::Warn("Cannot import the external texture '{}': this device enabled no Metal "
                      "object interop.",
                      name);
            return nullptr;
        }

        const optional<ExternalTextureDescription> description = DescribeExternalTexture(texture);
        if (!description)
        {
            Log::Warn("Cannot import the external texture '{}': it is null or carries a pixel "
                      "format no engine format maps.",
                      name);
            return nullptr;
        }

        id<MTLTexture> mtlTexture = (id<MTLTexture>)texture;

        const ImageInfo info{
            .Name = string(name),
            .Extent = uvec3{description->Extent.x, description->Extent.y, 1u},
            .MipLevels = 1,
            .Layers = 1,
            .Format = description->Format,
            .Type = ImageType::Type2D,
            // Everything a composite target plus a readback or a sample could want: the import is
            // free of charge, since the texture's memory is allocated either way.
            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc | ImageUsage::TransferDst |
                     ImageUsage::Sampled,
        };

        // A single-plane colour texture is plane 0, which is what the import's aspect names.
        const vk::ImportMetalTextureInfoEXT importInfo{
            .plane = vk::ImageAspectFlagBits::ePlane0,
            .mtlTexture = mtlTexture,
        };

        const vk::ImageCreateInfo createInfo{
            .pNext = &importInfo,
            .imageType = ToVk(info.Type),
            .format = ToVk(info.Format),
            .extent = vk::Extent3D{info.Extent.x, info.Extent.y, info.Extent.z},
            .mipLevels = info.MipLevels,
            .arrayLayers = info.Layers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = ToVk(info.Usage),
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };

        const vk::Device device = GetVkDevice(context);

        auto native = CreateUnique<Image::Native>();
        native->Image = device.createImage(createInfo).value;

#ifdef VE_ENABLE_VALIDATION_LAYERS
        // The validation layer has no notion of an image whose memory came in through the import,
        // so it reports "used with no memory bound" on every use of one. The image already exists,
        // so the silencing bind is an allocate-then-bind rather than vmaCreateImage; it is accepted
        // and changes nothing — the render still lands in the external texture's memory. Made only
        // here, so a shipped build pays neither the allocation nor the call.
        const VkImage rawImage = static_cast<VkImage>(native->Image);
        // VMA's AUTO usages are legal only where it creates the resource itself and so knows its
        // details; this image already exists, so the memory is asked for by its properties.
        const VmaAllocationCreateInfo allocationCreateInfo{
            .usage = VMA_MEMORY_USAGE_UNKNOWN,
            .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        };
        VK_RAW_ASSERT(vmaAllocateMemoryForImage(GetVmaAllocator(context), rawImage,
                                                &allocationCreateInfo, &native->Allocation,
                                                &native->AllocationInfo),
                      fmt::format("Failed to allocate the validation-silencing memory for the "
                                  "imported image {}",
                                  name));
        VK_RAW_ASSERT(vmaBindImageMemory(GetVmaAllocator(context), native->Allocation, rawImage),
                      fmt::format("Failed to bind the validation-silencing memory to the imported "
                                  "image {}",
                                  name));
#endif

        // The VkImage reads the texture's memory for as long as it lives, so the engine holds its
        // own reference and drops it from the retire the image's destructor defers — after the
        // frame that last used it has had its fence waited.
        [mtlTexture retain];
        native->Teardown = [mtlTexture] { [mtlTexture release]; };

        return Image::CreateImported(context, info, std::move(native));
    }
}
