#ifndef _GPU_IMAGE_VIEW_H_
#define _GPU_IMAGE_VIEW_H_

#include "../gfxcommon.h"

class ImageView
{
public:
    ImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
    ~ImageView();

    VkImageViewCreateInfo imageViewCreateInfo{};
    VkImageView view;
};

#endif