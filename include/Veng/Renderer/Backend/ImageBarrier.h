#pragma once

#include <Veng/Renderer/Backend/Image.h>

namespace Veng::Renderer
{
    struct ImageBarrier
    {
        Image& Image;
        vk::ImageLayout NewLayout;
        u32 BaseLayer = 0;
        u32 LayerCount = 1;
        u32 BaseMipLevel = 0;
        u32 MipLevelCount = 1;
    };
}
