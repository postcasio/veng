#include "Utils.h"

namespace Veng::Renderer
{
    bool Utils::IsDepthFormat(const vk::Format format)
    {
        switch (format)
        {
        case vk::Format::eD16Unorm:
        case vk::Format::eX8D24UnormPack32:
        case vk::Format::eD32Sfloat:
        case vk::Format::eS8Uint:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
            return true;
        default:
            return false;
        }
    }

    bool Utils::IsStencilFormat(const vk::Format format)
    {
        switch (format)
        {
        case vk::Format::eS8Uint:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
            return true;
        default:
            return false;
        }
    }

    vk::ImageLayout Utils::GetFormatAttachmentImageLayout(const vk::Format format)
    {
        if (IsStencilFormat(format))
        {
            return vk::ImageLayout::eDepthStencilAttachmentOptimal;
        }
        else if (IsDepthFormat(format))
        {
            return vk::ImageLayout::eDepthStencilAttachmentOptimal;
        }
        else
        {
            return vk::ImageLayout::eColorAttachmentOptimal;
        }
    }

    vk::ImageAspectFlags Utils::GetAspectFlags(vk::Format format)
    {
        if (IsStencilFormat(format))
        {
            return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        }
        else if (IsDepthFormat(format))
        {
            return vk::ImageAspectFlagBits::eDepth;
        }
        else
        {
            return vk::ImageAspectFlagBits::eColor;
        }
    }
}
