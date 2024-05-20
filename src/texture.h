#ifndef _TEXTURE_H_
#define _TEXTURE_H_

#include "gfxcommon.h"
#include "gpu/image_allocation.h"
#include "gpu/image_view.h"
#include "gpu/sampler.h"

#include <filesystem>

class Texture
{
public:
    std::unique_ptr<ImageAllocation> imageAllocation;
    std::unique_ptr<ImageView> imageView;
    std::unique_ptr<Sampler> sampler;

    std::filesystem::path path;

    ~Texture();
    Texture(std::filesystem::path const &path);
    Texture(std::filesystem::path const &path, VkFormat format);

private:
};

#endif