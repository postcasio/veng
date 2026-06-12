#include <Veng/Renderer/Backend/ImageView.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer
{
    ImageView::ImageView(const ImageViewInfo& info) : m_Name(info.Name), m_Format(info.Image->GetFormat()),
                                                      m_Image(info.Image)
    {
        const vk::ImageViewCreateInfo createInfo{
            .components = info.Components,
            .format = info.Image->GetFormat(),
            .image = info.Image->GetVkImage(),
            .viewType = info.ViewType,
            .subresourceRange = {
                .aspectMask = Utils::GetAspectFlags(info.Image->GetFormat()),
                .baseMipLevel = info.BaseMipLevel,
                .levelCount = info.MipLevels,
                .baseArrayLayer = info.BaseArrayLayer,
                .layerCount = info.ArrayLayers
            }
        };

        m_VkImageView = Context::Instance().GetVkDevice().createImageView(createInfo);

        DebugMarkers::MarkImageView(m_VkImageView, info.Name);
    }

    ImageView::~ImageView()
    {
        Context::Instance().GetVkDevice().destroyImageView(m_VkImageView);
    }
}
