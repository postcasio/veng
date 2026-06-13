#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Image;
    class Context;

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
            return Ref<ImageView>(new ImageView(info));
        }

        ~ImageView();

        ImageView(const ImageView&) = delete;
        ImageView& operator=(const ImageView&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] Format GetFormat() const { return m_Format; }
        [[nodiscard]] Ref<Image> GetImage() const { return m_Image; }

        // Subresource range this view covers. The render graph reads these to emit
        // barriers over exactly the affected mips/layers (see RenderGraph).
        [[nodiscard]] u32 GetBaseMipLevel() const { return m_BaseMipLevel; }
        [[nodiscard]] u32 GetMipLevels() const { return m_MipLevels; }
        [[nodiscard]] u32 GetBaseArrayLayer() const { return m_BaseArrayLayer; }
        [[nodiscard]] u32 GetArrayLayers() const { return m_ArrayLayers; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit ImageView(const ImageViewInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Context& m_Context;
        string m_Name;
        Format m_Format;
        u32 m_BaseMipLevel;
        u32 m_MipLevels;
        u32 m_BaseArrayLayer;
        u32 m_ArrayLayers;
        Unique<Native> m_Native;
        Ref<Image> m_Image;
    };
}
