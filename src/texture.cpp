#include "gfxcommon.h"
#include <filesystem>
#include <stb_image.h>

#include "engine.h"
#include "gpu/buffer_allocation.h"
#include "gpu/image_allocation.h"
#include "gpu/image_view.h"
#include "texture.h"

Texture::Texture(std::filesystem::path const &path)
    : Texture(path, VK_FORMAT_R8G8B8A8_SRGB) {}

Texture::Texture(std::filesystem::path const &path, VkFormat format) : path(path)
{
    int texWidth, texHeight, texChannels;
    stbi_uc *pixels = stbi_load(path.c_str(), &texWidth, &texHeight, &texChannels,
                                STBI_rgb_alpha);
    VkDeviceSize imageSize = texWidth * texHeight * 4;

    if (!pixels)
    {
        throw std::runtime_error("failed to load texture image!");
    }

    imageAllocation = std::make_unique<ImageAllocation>(
        texWidth, texHeight, VK_SAMPLE_COUNT_1_BIT, ImageType::Texture2D, format,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    imageAllocation->uploadTexture(pixels, imageSize, format, texWidth, texHeight);

    imageView = imageAllocation->createView(format, VK_IMAGE_ASPECT_COLOR_BIT);

    stbi_image_free(pixels);

    sampler = device()->createSampler();
}

Texture::~Texture()
{
    sampler.reset();
    imageView.reset();
    imageAllocation.reset();
}