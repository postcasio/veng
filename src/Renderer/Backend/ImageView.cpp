#include <Veng/Renderer/Backend/ImageView.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer
{
    ImageView::ImageView(const ImageViewInfo& info) : m_Name(info.Name), m_Format(ToVk(info.Image->GetFormat())),
                                                      m_Image(info.Image)
    {
        const vk::ImageViewCreateInfo createInfo{
            .flags = info.Flags,
            .image = info.Image->GetVkImage(),
            .viewType = info.ViewType,
            .format = ToVk(info.Image->GetFormat()),
            .components = info.Components,
            .subresourceRange = {
                .aspectMask = Utils::GetAspectFlags(ToVk(info.Image->GetFormat())),
                .baseMipLevel = info.BaseMipLevel,
                .levelCount = info.MipLevels,
                .baseArrayLayer = info.BaseArrayLayer,
                .layerCount = info.ArrayLayers
            }
        };

        m_VkImageView = Context::Instance().GetVkDevice().createImageView(createInfo).value;

        DebugMarkers::MarkImageView(m_VkImageView, info.Name);
    }

    ImageView::~ImageView()
    {
        // A view over an unmanaged image is a presentable (swapchain) image
        // view: it must be destroyed before its swapchain, and the swapchain is
        // only ever torn down with the GPU idle (resize/teardown WaitIdle
        // first), so destroy it immediately rather than deferring. Views over
        // managed images take the normal deferred-destruction path.
        if (m_Image && !m_Image->IsManaged())
        {
            Context::Instance().GetVkDevice().destroyImageView(m_VkImageView);
        }
        else
        {
            Context::Instance().Retire(m_VkImageView);
        }
    }
}
