#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class CommandBuffer;
    class SwapChain;

    struct ImageInfo
    {
        string Name = "Image";
        uvec3 Extent = {1, 1, 1};
        u32 MipLevels = 1;
        u32 Layers = 1;
        Format Format;
        ImageType Type = ImageType::Type2D;
        ImageUsage Usage;
    };

    class Image : public std::enable_shared_from_this<Image>
    {
    public:
        static Ref<Image> Create(const ImageInfo& info)
        {
            return Ref<Image>(new Image(info));
        }

        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] Format GetFormat() const { return m_Format; }
        [[nodiscard]] ImageUsage GetUsage() const { return m_Usage; }
        [[nodiscard]] uvec3 GetExtent() const { return m_Extent; }
        [[nodiscard]] u32 GetWidth() const { return m_Extent.x; }
        [[nodiscard]] u32 GetHeight() const { return m_Extent.y; }
        [[nodiscard]] u32 GetDepth() const { return m_Extent.z; }
        [[nodiscard]] u32 GetMipLevels() const { return m_MipLevels; }
        [[nodiscard]] u32 GetLayers() const { return m_Layers; }
        [[nodiscard]] bool IsManaged() const { return m_Managed; }
        [[nodiscard]] ImageType GetType() const { return m_Type; }

        // Per-subresource layout/stage/access tracking lives in Native (it holds
        // vk:: types). The render graph and the engine's transfer paths read and
        // update it through the backend; consumers no longer reason about layout.

        void GenerateMipmaps(CommandBuffer& commandBuffer);
        void Upload(std::span<const u8> span);
        [[nodiscard]] vector<u8> Download();

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit Image(const ImageInfo& info);

        // Presentable (swapchain) images: the Native already wraps an
        // externally-owned vk::Image, so this constructor only sets up the
        // engine-side bookkeeping.
        Image(const ImageInfo& info, Unique<Native> native);

        string m_Name;
        uvec3 m_Extent;
        u32 m_MipLevels;
        u32 m_Layers;
        Format m_Format;
        ImageType m_Type;
        ImageUsage m_Usage;
        bool m_Managed;

        Unique<Native> m_Native;

        friend class SwapChain;
    };
}
