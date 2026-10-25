#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct SwapChainSupportDetails
    {
        vk::SurfaceCapabilitiesKHR Capabilities;
        vector<vk::SurfaceFormatKHR> Formats;
        vector<vk::PresentModeKHR> PresentModes;
    };
}
