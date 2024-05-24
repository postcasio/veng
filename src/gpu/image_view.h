#ifndef _GPU_IMAGE_VIEW_H_
#define _GPU_IMAGE_VIEW_H_

class ImageAllocation;

#include "../gfxcommon.h"
#include "image_allocation.h"
class ImageView
{
public:
    ImageView(ImageAllocation &image, VkFormat format, VkImageAspectFlags aspectFlags);
    ~ImageView();

    VkImageViewCreateInfo imageViewCreateInfo{};
    VkImageView view;

    ImageAllocation &image;
};

#endif