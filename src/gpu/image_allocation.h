#ifndef _GPU_IMAGE_ALLOCATION_H_
#define _GPU_IMAGE_ALLOCATION_H_

#include "../gfxcommon.h"
#include "buffer_allocation.h"

class ImageAllocation
{
public:
    ImageAllocation(uint32_t width, uint32_t height, VkSampleCountFlagBits samples, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, const char *name = nullptr);
    ~ImageAllocation();

    VmaAllocation allocation;
    VkImage image;

    VkImageCreateInfo imageCreateInfo{};
    VmaAllocationCreateInfo allocationCreateInfo{};

    void copyBufferToImage(BufferAllocation &buffer, uint32_t width, uint32_t height);
    void transitionImageLayout(VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    void uploadTexture(const void *pixels, VkDeviceSize imageSize,
                       VkFormat format, uint32_t texWidth, uint32_t texHeight);
};

#endif