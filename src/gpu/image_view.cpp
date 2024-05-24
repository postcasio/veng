#include "image_view.h"
#include "../engine.h"

ImageView::ImageView(ImageAllocation &image, VkFormat format, VkImageAspectFlags aspectFlags) : image(image)
{
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.image = image.image;
    imageViewCreateInfo.format = format;
    imageViewCreateInfo.subresourceRange.aspectMask = aspectFlags;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = image.arrayLayers;

    switch (image.type)
    {
    case ImageType::Texture2D:
        imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

        break;
    case ImageType::TextureCube:
        imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
    }

    VK_CHECK_RESULT(vkCreateImageView(renderer()->device->device, &imageViewCreateInfo, nullptr, &view), "failed to create texture image view!");
}

ImageView::~ImageView()
{
    vkDestroyImageView(renderer()->device->device, view, nullptr);
}