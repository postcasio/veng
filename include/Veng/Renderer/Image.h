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

    class Image
    {
    public:
        static Ref<Image> Create(const ImageInfo& info)
        {
            return CreateRef<Image>(info);
        }

        explicit Image(const ImageInfo& info);

        ~Image();

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

        // Layout tracking is per layer * per mip.
        void SetLayout(const ImageLayout layout) { for (auto& l : m_Layouts) l = layout; }

        void SetLayout(const u32 layer, const u32 mip, const ImageLayout layout)
        {
            m_Layouts[GetLayoutIndex(layer, mip)] = layout;
        }

        void SetLayout(const u32 baseLayer, const u32 layerCount, const u32 baseMip, const u32 mipCount, const ImageLayout layout)
        {
            for (u32 layer = baseLayer; layer < baseLayer + layerCount; layer++)
                for (u32 mip = baseMip; mip < baseMip + mipCount; mip++)
                    m_Layouts[GetLayoutIndex(layer, mip)] = layout;
        }

        [[nodiscard]] ImageLayout GetLayout(const u32 layer, const u32 mip) const
        {
            return m_Layouts[GetLayoutIndex(layer, mip)];
        }

        void GenerateMipmaps(CommandBuffer& commandBuffer);
        void Upload(std::span<const u8> span);
        [[nodiscard]] vector<u8> Download();

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        [[nodiscard]] u32 GetLayoutIndex(const u32 layer, const u32 mip) const
        {
            return layer * m_MipLevels + mip;
        }

        // Presentable (swapchain) images: the Native already wraps an
        // externally-owned vk::Image, so this constructor only sets up the
        // engine-side bookkeeping.
        Image(const ImageInfo& info, Unique<Native> native);

        string m_Name;
        uvec3 m_Extent;
        u32 m_MipLevels;
        u32 m_Layers;
        vector<ImageLayout> m_Layouts{};
        Format m_Format;
        ImageType m_Type;
        ImageUsage m_Usage;
        bool m_Managed;

        Unique<Native> m_Native;

        friend class SwapChain;
    };
}
