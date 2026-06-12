#include <Veng/Renderer/Backend/Utils.h>

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

    vk::AccessFlags Utils::GetAccessMask(const vk::ImageLayout imageLayout)
    {
        switch (imageLayout)
        {
        case vk::ImageLayout::eUndefined:
            return {};
        case vk::ImageLayout::eColorAttachmentOptimal:
            return vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
            return vk::AccessFlagBits::eDepthStencilAttachmentRead;
        case vk::ImageLayout::eDepthAttachmentOptimal:
            return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        case vk::ImageLayout::eDepthReadOnlyOptimal:
            return vk::AccessFlagBits::eDepthStencilAttachmentRead;
        case vk::ImageLayout::eStencilAttachmentOptimal:
            return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        case vk::ImageLayout::eStencilReadOnlyOptimal:
            return vk::AccessFlagBits::eDepthStencilAttachmentRead;
        case vk::ImageLayout::eShaderReadOnlyOptimal:
            return vk::AccessFlagBits::eShaderRead;
        case vk::ImageLayout::eTransferSrcOptimal:
            return vk::AccessFlagBits::eTransferRead;
        case vk::ImageLayout::eTransferDstOptimal:
            return vk::AccessFlagBits::eTransferWrite;
        case vk::ImageLayout::ePresentSrcKHR:
            return vk::AccessFlagBits::eMemoryRead;
        default:
            VE_ASSERT(false, "Unsupported image layout");
        }
    }

    vk::PipelineStageFlags Utils::GetDestinationStageMask(const vk::ImageLayout imageLayout)
    {
        switch (imageLayout)
        {
        case vk::ImageLayout::eUndefined: return vk::PipelineStageFlagBits::eTopOfPipe;
        case vk::ImageLayout::eDepthReadOnlyOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eDepthStencilReadOnlyOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eStencilReadOnlyOptimal: return vk::PipelineStageFlagBits::eLateFragmentTests;
        case vk::ImageLayout::eShaderReadOnlyOptimal: return vk::PipelineStageFlagBits::eFragmentShader;
        case vk::ImageLayout::eTransferSrcOptimal: return vk::PipelineStageFlagBits::eTransfer;
        case vk::ImageLayout::eTransferDstOptimal: return vk::PipelineStageFlagBits::eTransfer;
        case vk::ImageLayout::eColorAttachmentOptimal: return vk::PipelineStageFlagBits::eColorAttachmentOutput;
        case vk::ImageLayout::eDepthAttachmentOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eStencilAttachmentOptimal: return vk::PipelineStageFlagBits::eLateFragmentTests;
        case vk::ImageLayout::ePresentSrcKHR:
            // Present isn't a pipeline stage; use AllCommands as a conservative barrier endpoint.
            return vk::PipelineStageFlagBits::eAllCommands;
        default:
            VE_ASSERT(false, "Unsupported image layout");
        }
    }

    vk::PipelineStageFlags Utils::GetSourceStageMask(const vk::ImageLayout imageLayout)
    {
        switch (imageLayout)
        {
        case vk::ImageLayout::eUndefined: return vk::PipelineStageFlagBits::eTopOfPipe;
        case vk::ImageLayout::eDepthReadOnlyOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eDepthStencilReadOnlyOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eStencilReadOnlyOptimal: return vk::PipelineStageFlagBits::eLateFragmentTests;
        case vk::ImageLayout::eShaderReadOnlyOptimal: return vk::PipelineStageFlagBits::eFragmentShader;
        case vk::ImageLayout::eTransferSrcOptimal: return vk::PipelineStageFlagBits::eTransfer;
        case vk::ImageLayout::eTransferDstOptimal: return vk::PipelineStageFlagBits::eTransfer;
        case vk::ImageLayout::eColorAttachmentOptimal: return vk::PipelineStageFlagBits::eColorAttachmentOutput;
        case vk::ImageLayout::eDepthAttachmentOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal: return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case vk::ImageLayout::eStencilAttachmentOptimal: return vk::PipelineStageFlagBits::eLateFragmentTests;
        case vk::ImageLayout::ePresentSrcKHR:
            // When transitioning from present, be conservative so we properly wait for external reads.
            return vk::PipelineStageFlagBits::eAllCommands;
        default:
            VE_ASSERT(false, "Unsupported image layout");
        }
    }
}
