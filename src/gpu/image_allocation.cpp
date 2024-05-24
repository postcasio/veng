#include "image_allocation.h"
#include "../engine.h"

ImageAllocation::ImageAllocation(VkImage image, ImageType type) : image(image), type(type)
{
    shouldDestroyImage = false;
    arrayLayers = 1;
}

ImageAllocation::ImageAllocation(uint32_t width, uint32_t height, VkSampleCountFlagBits samples, ImageType type, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, const char *name) : type(type), width(width), height(height), format(format)
{
    std::string fullName = "ImageAllocation";

    if (name != nullptr)
    {
        fullName.append(" - ");
        fullName.append(name);
    }

    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.extent.width = width;
    imageCreateInfo.extent.height = height;
    imageCreateInfo.extent.depth = 1;
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.format = format;
    imageCreateInfo.tiling = tiling;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.usage = usage;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

#ifdef ENABLE_MULTISAMPLING
    imageCreateInfo.samples = samples;
#endif

    allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationCreateInfo.requiredFlags = properties;
    allocationCreateInfo.pool = VK_NULL_HANDLE;
    allocationCreateInfo.flags = VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
    allocationCreateInfo.pUserData = (void *)fullName.c_str();

    switch (type)
    {
    case ImageType::Texture2D:
        imageCreateInfo.arrayLayers = 1;
        arrayLayers = 1;
        break;
    case ImageType::TextureCube:
        imageCreateInfo.arrayLayers = 6;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        arrayLayers = 6;
        break;
    }

    VK_CHECK_RESULT(vmaCreateImage(renderer()->allocator, &imageCreateInfo, &allocationCreateInfo, &image, &allocation, nullptr), "failed to create image!");
}

ImageAllocation::~ImageAllocation()
{
    if (shouldDestroyImage)
    {
        vmaDestroyImage(renderer()->allocator, image, allocation);
    }
}

void ImageAllocation::copyBufferToImage(BufferAllocation &buffer, uint32_t width, uint32_t height)
{
    auto commandBuffer = renderer()->commandPool->createCommandBuffer();

    commandBuffer->begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    commandBuffer->copyBufferToImage(buffer, *this, width, height);

    commandBuffer->end();

    auto underlyingBuffer = commandBuffer->currentBuffer();

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &underlyingBuffer;

    device()->graphicsQueue->submit(submitInfo);
    device()->graphicsQueue->waitIdle();
}

void ImageAllocation::transitionImageLayout(VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    auto commandBuffer = renderer()->commandPool->createCommandBuffer();
    commandBuffer->begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VkImageMemoryBarrier barrier{};

    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;

    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    barrier.image = image;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        if (renderer()->hasStencilComponent(format))
        {
            barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
    }
    else
    {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    }
    else
    {
        throw std::invalid_argument("unsupported layout transition!");
    }

    commandBuffer->pipelineBarrier(
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier);

    commandBuffer->end();

    auto underlyingBuffer = commandBuffer->currentBuffer();

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &underlyingBuffer;

    device()->graphicsQueue->submit(submitInfo);
    device()->graphicsQueue->waitIdle();
};

void ImageAllocation::uploadTexture(const void *pixels, VkDeviceSize imageSize,
                                    VkFormat format, uint32_t texWidth, uint32_t texHeight)
{
    BufferAllocation stagingBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    stagingBuffer.copyMemoryToAllocation(pixels, 0, imageSize);

    transitionImageLayout(format, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(stagingBuffer,
                      static_cast<uint32_t>(texWidth),
                      static_cast<uint32_t>(texHeight));
    transitionImageLayout(
        format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

std::unique_ptr<ImageView> ImageAllocation::createView(VkFormat format, VkImageAspectFlags aspectFlags)
{
    return std::make_unique<ImageView>(*this, format, aspectFlags);
}