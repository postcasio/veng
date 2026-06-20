#include <Veng/Renderer/ImageView.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer
{
    /// @brief Returns the backend-native image view handle.
    ImageView::Native& ImageView::GetNative() const
    {
        return *m_Native;
    }

    /// @brief Creates a Vulkan image view over the specified image subresource range.
    /// @param context  The owning render context.
    /// @param info     View configuration including the source image, view type, and mip/layer range.
    ImageView::ImageView(Context& context, const ImageViewInfo& info)
        : m_Context(context), m_Name(info.Name), m_Format(info.Image->GetFormat()),
          m_BaseMipLevel(info.BaseMipLevel), m_MipLevels(info.MipLevels),
          m_BaseArrayLayer(info.BaseArrayLayer), m_ArrayLayers(info.ArrayLayers),
          m_Native(CreateUnique<Native>()), m_Image(info.Image)
    {
        const vk::ImageViewCreateInfo createInfo{
            .image = info.Image->GetNative().Image,
            .viewType = ToVk(info.ViewType),
            .format = ToVk(info.Image->GetFormat()),
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity},
            .subresourceRange = {.aspectMask = Utils::GetAspectFlags(ToVk(info.Image->GetFormat())),
                                 .baseMipLevel = info.BaseMipLevel,
                                 .levelCount = info.MipLevels,
                                 .baseArrayLayer = info.BaseArrayLayer,
                                 .layerCount = info.ArrayLayers}};

        m_Native->ImageView = GetVkDevice(m_Context).createImageView(createInfo).value;

        DebugMarkers::MarkImageView(GetVkDevice(m_Context), m_Native->ImageView, info.Name);
    }

    /// @brief Destroys the image view, either immediately or deferred.
    ///
    /// Swapchain image views (unmanaged) must be destroyed before the swapchain and are
    /// always torn down with the GPU idle (WaitIdle before resize/teardown), so they are
    /// destroyed immediately. Managed-image views are deferred until the GPU is done.
    ImageView::~ImageView()
    {
        if (m_Image && !m_Image->IsManaged())
        {
            GetVkDevice(m_Context).destroyImageView(m_Native->ImageView);
        }
        else
        {
            m_Context.GetNative().Retire(m_Native->ImageView);
        }
    }
}
