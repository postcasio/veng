#include "image_view.h"
#include "../engine.h"

ImageView::ImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags)
{
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.image = image;
    imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewCreateInfo.format = format;
    imageViewCreateInfo.subresourceRange.aspectMask = aspectFlags;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = 1;

    VK_CHECK_RESULT(vkCreateImageView(renderer()->device->device, &imageViewCreateInfo, nullptr, &view), "failed to create texture image view!");
}

ImageView::~ImageView()
{
    vkDestroyImageView(renderer()->device->device, view, nullptr);
}