#pragma once

#include <Veng/Renderer/Image.h>

namespace Veng::Renderer
{
    struct ImageBarrier
    {
        Image& Image;
        ImageLayout NewLayout;
        u32 BaseLayer = 0;
        u32 LayerCount = 1;
        u32 BaseMipLevel = 0;
        u32 MipLevelCount = 1;
    };
}
