#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Image;

    struct ImageViewInfo
    {
        string Name;
        Ref<Image> Image;

        ImageViewType ViewType = ImageViewType::Type2D;
        u32 BaseMipLevel = 0;
        u32 MipLevels = 1;
        u32 BaseArrayLayer = 0;
        u32 ArrayLayers = 1;
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
        [[nodiscard]] Format GetFormat() const { return m_Format; }
        [[nodiscard]] Ref<Image> GetImage() const { return m_Image; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Format m_Format;
        Unique<Native> m_Native;
        Ref<Image> m_Image;
    };
}
