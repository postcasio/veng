#include <Veng/Renderer/Image.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

#include <vulkan/vulkan_format_traits.hpp>

namespace Veng::Renderer
{
    Image::Native& Image::GetNative() const { return *m_Native; }

    Image::Image(Context& context, const ImageInfo& info, Unique<Native> native) :
        m_Context(context),
        m_Name(info.Name),
        m_Extent(info.Extent),
        m_MipLevels(info.MipLevels),
        m_Layers(info.Layers),
        m_Format(info.Format),
        m_Type(info.Type),
        m_Usage(info.Usage),
        m_Managed(false),
        m_Native(std::move(native))
    {
        m_Native->InitStates(m_Layers, m_MipLevels);

        DebugMarkers::MarkImage(GetVkDevice(m_Context), m_Native->Image, m_Name);
    }

    Image::Image(Context& context, const ImageInfo& info) :
        m_Context(context),
        m_Name(info.Name),
        m_Extent(info.Extent),
        m_MipLevels(info.MipLevels),
        m_Layers(info.Layers),
        m_Format(info.Format),
        m_Type(info.Type),
        m_Usage(info.Usage),
        m_Managed(true),
        m_Native(CreateUnique<Native>())
    {
        m_Native->InitStates(m_Layers, m_MipLevels);

        vk::ImageCreateFlags flags;

        if (m_Layers == 6)
        {
            flags |= vk::ImageCreateFlagBits::eCubeCompatible;
        }

        VkImageCreateInfo imageCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = static_cast<VkImageCreateFlags>(flags),
            .imageType = static_cast<VkImageType>(ToVk(m_Type)),
            .format = static_cast<VkFormat>(ToVk(m_Format)),
            .extent = {m_Extent.x, m_Extent.y, m_Extent.z},
            .mipLevels = m_MipLevels,
            .arrayLayers = m_Layers,
            .samples = static_cast<VkSampleCountFlagBits>(vk::SampleCountFlagBits::e1),
            .tiling = static_cast<VkImageTiling>(vk::ImageTiling::eOptimal),
            .usage = static_cast<VkImageUsageFlags>(ToVk(m_Usage)),
            .sharingMode = static_cast<VkSharingMode>(vk::SharingMode::eExclusive),
            .initialLayout = static_cast<VkImageLayout>(vk::ImageLayout::eUndefined)
        };

        VmaAllocationCreateInfo allocationCreateInfo{
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            .pool = VK_NULL_HANDLE,
        };

        VkImage image;

        VK_RAW_ASSERT(vmaCreateImage(
                          GetVmaAllocator(m_Context),
                          &imageCreateInfo,
                          &allocationCreateInfo,
                          &image,
                          &m_Native->Allocation,
                          &m_Native->AllocationInfo), fmt::format("Failed to create image {}", m_Name));

        m_Native->Image = image;

        vmaSetAllocationName(
            GetVmaAllocator(m_Context),
            m_Native->Allocation,
            m_Name.c_str());

        DebugMarkers::MarkImage(GetVkDevice(m_Context), m_Native->Image, m_Name);
    }

    Image::~Image()
    {
        if (m_Managed)
        {
            m_Context.GetNative().Retire(m_Native->Image, m_Native->Allocation);
        }
    }


    void Image::GenerateMipmaps(CommandBuffer& commandBuffer)
    {
        u32 mipWidth = m_Extent.x;
        u32 mipHeight = m_Extent.y;

        for (u32 i = 1; i < m_MipLevels; i++)
        {
            Backend::TransitionImage(commandBuffer, *this, ImageLayout::TransferSrc, 0, 1, i - 1, 1);

            commandBuffer.BlitImage({
                .SourceImage = shared_from_this(),
                .DestinationImage = shared_from_this(),
                .SourceMipLevel = i - 1,
                .DestinationMipLevel = i,
                .SourceOffset = {0, 0, 0},
                .DestinationOffset = {0, 0, 0},
                .SourceExtent = {mipWidth, mipHeight, 1},
                .DestinationExtent = {mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1}
            });

            Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, 1, i - 1, 1);

            if (mipWidth > 1) mipWidth /= 2;
            if (mipHeight > 1) mipHeight /= 2;
        }

        Backend::TransitionImage(commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, 1, m_MipLevels - 1, 1);
    }

    void Image::Upload(std::span<const u8> span)
    {
        auto stagingBuffer = Buffer::Create(m_Context, {
            .Name = m_Name + " (Upload)",
            .Size = span.size(),
            .Usage = BufferUsage::TransferSrc,
        });

        stagingBuffer->Upload(span);

        auto commandBuffer = CommandBuffer::Create(m_Context);

        commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);
        Backend::TransitionImage(*commandBuffer, *this, ImageLayout::TransferDst, 0, m_Layers, 0, m_MipLevels);
        commandBuffer->CopyBufferToImage(stagingBuffer, shared_from_this());

        if (m_MipLevels > 1)
        {
            GenerateMipmaps(*commandBuffer);
        }
        else
        {
            Backend::TransitionImage(*commandBuffer, *this, ImageLayout::ShaderReadOnly, 0, m_Layers, 0, 1);
        }

        commandBuffer->End();

        m_Context.SubmitImmediateCommands(*commandBuffer);
    }

    vector<u8> Image::Download()
    {
        auto buffer = Buffer::Create(m_Context, {
            .Name = m_Name + " (Download)",
            .Size = m_Extent.x * m_Extent.y * vk::blockSize(ToVk(m_Format)),
            .Usage = BufferUsage::TransferDst,
        });

        const ImageLayout originalLayout = FromVk(m_Native->At(0, 0).Layout);

        auto commandBuffer = CommandBuffer::Create(m_Context);

        commandBuffer->Begin(CommandBufferUsage::OneTimeSubmit);

        Backend::TransitionImage(*commandBuffer, *this, ImageLayout::TransferSrc);

        commandBuffer->CopyImageToBuffer(shared_from_this(), buffer);

        // Restore the image to the layout it had on entry so callers see no
        // change; skip if it was never transitioned (can't transition to
        // Undefined).
        if (originalLayout != ImageLayout::Undefined)
        {
            Backend::TransitionImage(*commandBuffer, *this, originalLayout);
        }

        commandBuffer->End();

        m_Context.SubmitImmediateCommands(*commandBuffer);

        return buffer->Download();
    }
}
