#include <Veng/Renderer/Backend/Image.h>

#include <Veng/Renderer/Backend/Buffer.h>
#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

#include <vulkan/vulkan_format_traits.hpp>

namespace Veng::Renderer
{
    Image::Image(const vk::Image vkImage, const ImageInfo& info) :
        m_Name(info.Name),
        m_Extent(info.Extent),
        m_MipLevels(info.MipLevels),
        m_Layers(info.Layers),
        m_Format(info.Format),
        m_Type(info.Type),
        m_Usage(info.Usage),
        m_Managed(false),
        m_VkImage(vkImage)
    {
        m_Layouts.resize(m_Layers * m_MipLevels, vk::ImageLayout::eUndefined);

        DebugMarkers::MarkImage(m_VkImage, m_Name);
    }

    Image::Image(const ImageInfo& info) :
        m_Name(info.Name),
        m_Extent(info.Extent),
        m_MipLevels(info.MipLevels),
        m_Layers(info.Layers),
        m_Format(info.Format),
        m_Type(info.Type),
        m_Usage(info.Usage),
        m_Managed(true)
    {
        m_Layouts.resize(m_Layers * m_MipLevels, vk::ImageLayout::eUndefined);

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
                          Context::Instance().GetAllocator(),
                          &imageCreateInfo,
                          &allocationCreateInfo,
                          &image,
                          &m_VmaAllocation,
                          &m_VmaAllocationInfo), fmt::format("Failed to create image {}", m_Name));

        m_VkImage = image;

        vmaSetAllocationName(
            Context::Instance().GetAllocator(),
            m_VmaAllocation,
            m_Name.c_str());

        DebugMarkers::MarkImage(m_VkImage, m_Name);
    }

    Image::~Image()
    {
        if (m_Managed)
        {
            Context::Instance().Retire(m_VkImage, m_VmaAllocation);
        }
    }


    void Image::GenerateMipmaps(CommandBuffer& commandBuffer)
    {
        ImageBarrier barrier{
            .Image = *this,
            .NewLayout = vk::ImageLayout::eTransferSrcOptimal,
            .BaseMipLevel = 0,
        };

        u32 mipWidth = m_Extent.x;
        u32 mipHeight = m_Extent.y;

        for (u32 i = 1; i < m_MipLevels; i++)
        {
            barrier.NewLayout = vk::ImageLayout::eTransferSrcOptimal;
            barrier.BaseMipLevel = i - 1;

            commandBuffer.PipelineBarrier(barrier);

            commandBuffer.BlitImage({
                .SourceImage = *this,
                .DestinationImage = *this,
                .SourceMipLevel = i - 1,
                .DestinationMipLevel = i,
                .SourceOffset = {0, 0, 0},
                .DestinationOffset = {0, 0, 0},
                .SourceExtent = {mipWidth, mipHeight, 1},
                .DestinationExtent = {mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1}
            });

            barrier.NewLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

            commandBuffer.PipelineBarrier(barrier);

            if (mipWidth > 1) mipWidth /= 2;
            if (mipHeight > 1) mipHeight /= 2;
        }

        barrier.BaseMipLevel = m_MipLevels - 1;
        barrier.NewLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        commandBuffer.PipelineBarrier(barrier);
    }

    void Image::Upload(std::span<const u8> span)
    {
        auto stagingBuffer = Buffer::Create({
            .Name = m_Name + " (Upload)",
            .Size = span.size(),
            .Usage = BufferUsage::TransferSrc,
        });

        stagingBuffer->Upload(span);

        auto commandBuffer = CommandBuffer::Create();

        commandBuffer->Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
        commandBuffer->PipelineBarrier(ImageBarrier{
            .Image = *this,
            .NewLayout = vk::ImageLayout::eTransferDstOptimal,
            .LayerCount = m_Layers,
            .BaseMipLevel = 0,
            .MipLevelCount = m_MipLevels
        });
        commandBuffer->CopyBufferToImage(*stagingBuffer, *this);

        if (m_MipLevels > 1)
        {
            GenerateMipmaps(*commandBuffer);
        }
        else
        {
            commandBuffer->PipelineBarrier(ImageBarrier{
                .Image = *this,
                .NewLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .LayerCount = m_Layers,
                .MipLevelCount = 1
            });
        }

        commandBuffer->End();

        Context::Instance().SubmitImmediateCommands(*commandBuffer);
    }

    vector<u8> Image::Download()
    {
        auto buffer = Buffer::Create({
            .Name = m_Name + " (Download)",
            .Size = m_Extent.x * m_Extent.y * vk::blockSize(ToVk(m_Format)),
            .Usage = BufferUsage::TransferDst,
        });

        auto layout = GetLayout(0, 0);

        auto commandBuffer = CommandBuffer::Create();

        commandBuffer->Begin(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        commandBuffer->PipelineBarrier(ImageBarrier{
            .Image = *this,
            .NewLayout = vk::ImageLayout::eTransferSrcOptimal
        });

        commandBuffer->CopyImageToBuffer(*this, *buffer);

        commandBuffer->PipelineBarrier(ImageBarrier{
            .Image = *this,
            .NewLayout = layout
        });

        commandBuffer->End();

        Context::Instance().SubmitImmediateCommands(*commandBuffer);

        return buffer->Download();
    }
}
