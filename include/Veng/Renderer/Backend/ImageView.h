#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    class Image;

    struct ImageViewInfo
    {
        string Name;
        Ref<Image> Image;

        vk::ImageViewType ViewType = vk::ImageViewType::e2D;
        u32 BaseMipLevel = 0;
        u32 MipLevels = 1;
        u32 BaseArrayLayer = 0;
        u32 ArrayLayers = 1;
        vk::ComponentMapping Components = {
            vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity
        };
        vk::ImageViewCreateFlags Flags = {};
    };

    class ImageView
    {
    public:
        static Ref<ImageView> Create(const ImageViewInfo& info)
        {
            return CreateRef<ImageView>(info);
        }

        ~ImageView();
        explicit ImageView(const ImageViewInfo& info);

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] vk::ImageView GetVkImageView() const { return m_VkImageView; }
        [[nodiscard]] vk::Format GetFormat() const { return m_Format; }
        [[nodiscard]] Ref<Image> GetImage() const { return m_Image; }

    private:
        string m_Name;
        vk::Format m_Format;
        vk::ImageView m_VkImageView;
        Ref<Image> m_Image;
    };
}
