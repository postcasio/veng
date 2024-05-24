#ifndef _GPU_IMAGE_ALLOCATION_H_
#define _GPU_IMAGE_ALLOCATION_H_

class ImageView;

#include "../gfxcommon.h"
#include "buffer_allocation.h"
#include "image_view.h"

enum class ImageType
{
    Texture2D,
    TextureCube
};

class ImageAllocation
{
public:
    ImageAllocation(uint32_t width, uint32_t height, VkSampleCountFlagBits samples, ImageType type, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, const char *name = nullptr);
    ImageAllocation(VkImage image, ImageType type);
    ~ImageAllocation();

    VmaAllocation allocation;
    VkImage image;

    VkImageCreateInfo imageCreateInfo{};
    VmaAllocationCreateInfo allocationCreateInfo{};
    ImageType type;

    uint32_t width;
    uint32_t height;
    uint32_t arrayLayers;
    VkFormat format;

    void copyBufferToImage(BufferAllocation &buffer, uint32_t width, uint32_t height);
    void transitionImageLayout(VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    void uploadTexture(const void *pixels, VkDeviceSize imageSize,
                       VkFormat format, uint32_t texWidth, uint32_t texHeight);
    std::unique_ptr<ImageView> createView(VkFormat format, VkImageAspectFlags aspectFlags);

private:
    bool shouldDestroyImage = true;
};

#endif