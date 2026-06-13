#include <Veng/Renderer/ImageView.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer
{
    ImageView::Native& ImageView::GetNative() const { return *m_Native; }

    ImageView::ImageView(const ImageViewInfo& info) : m_Name(info.Name), m_Format(info.Image->GetFormat()),
                                                      m_Native(CreateUnique<Native>()), m_Image(info.Image)
    {
        const vk::ImageViewCreateInfo createInfo{
            .image = info.Image->GetNative().Image,
            .viewType = ToVk(info.ViewType),
            .format = ToVk(info.Image->GetFormat()),
            .components = {
                vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                vk::ComponentSwizzle::eIdentity
            },
            .subresourceRange = {
                .aspectMask = Utils::GetAspectFlags(ToVk(info.Image->GetFormat())),
                .baseMipLevel = info.BaseMipLevel,
                .levelCount = info.MipLevels,
                .baseArrayLayer = info.BaseArrayLayer,
                .layerCount = info.ArrayLayers
            }
        };

        m_Native->ImageView = GetVkDevice(Context::Instance()).createImageView(createInfo).value;

        DebugMarkers::MarkImageView(m_Native->ImageView, info.Name);
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
            GetVkDevice(Context::Instance()).destroyImageView(m_Native->ImageView);
        }
        else
        {
            Context::Instance().GetNative().Retire(m_Native->ImageView);
        }
    }
}
