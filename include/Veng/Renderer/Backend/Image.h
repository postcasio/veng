#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/DescriptorSet.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/Sampler.h>

namespace Veng::Renderer
{
    class CommandBuffer;

    struct ImageInfo
    {
        string Name = "Image";
        uvec3 Extent = {1, 1, 1};
        u32 MipLevels = 1;
        u32 Layers = 1;
        vk::Format Format;
        vk::ImageType Type = vk::ImageType::e2D;
        vk::ImageUsageFlags Usage;
    };

    class Image
    {
    public:
        static Ref<Image> Create(vk::Image vkImage, const ImageInfo& info)
        {
            return CreateRef<Image>(vkImage, info);
        }

        static Ref<Image> Create(const ImageInfo& info)
        {
            return CreateRef<Image>(info);
        }

        Image(vk::Image vkImage, const ImageInfo& info);
        explicit Image(const ImageInfo& info);

        ~Image();

        [[nodiscard]] string GetName() const { return m_Name; }
        [[nodiscard]] vk::Image GetVkImage() const { return m_VkImage; }
        [[nodiscard]] vk::Format GetFormat() const { return m_Format; }
        [[nodiscard]] vk::ImageUsageFlags GetUsage() const { return m_Usage; }
        [[nodiscard]] uvec3 GetExtent() const { return m_Extent; }
        [[nodiscard]] u32 GetWidth() const { return m_Extent.x; }
        [[nodiscard]] u32 GetHeight() const { return m_Extent.y; }
        [[nodiscard]] u32 GetDepth() const { return m_Extent.z; }
        [[nodiscard]] u32 GetMipLevels() const { return m_MipLevels; }
        [[nodiscard]] u32 GetLayers() const { return m_Layers; }
        [[nodiscard]] bool IsManaged() const { return m_Managed; }
        [[nodiscard]] vk::ImageType GetType() const { return m_Type; }

        // Layout tracking is per layer * per mip.
        void SetLayout(const vk::ImageLayout layout) { for (auto& l : m_Layouts) l = layout; }

        void SetLayout(const u32 layer, const u32 mip, const vk::ImageLayout layout)
        {
            m_Layouts[GetLayoutIndex(layer, mip)] = layout;
        }

        void SetLayout(const u32 baseLayer, const u32 layerCount, const u32 baseMip, const u32 mipCount, const vk::ImageLayout layout)
        {
            for (u32 layer = baseLayer; layer < baseLayer + layerCount; layer++)
                for (u32 mip = baseMip; mip < baseMip + mipCount; mip++)
                    m_Layouts[GetLayoutIndex(layer, mip)] = layout;
        }

        [[nodiscard]] vk::ImageLayout GetLayout(const u32 layer, const u32 mip) const
        {
            return m_Layouts[GetLayoutIndex(layer, mip)];
        }

        void GenerateMipmaps(CommandBuffer& commandBuffer);
        void Upload(std::span<u8> span);
        std::span<u8> Download();

    private:
        [[nodiscard]] u32 GetLayoutIndex(const u32 layer, const u32 mip) const
        {
            return layer * m_MipLevels + mip;
        }

        string m_Name;
        uvec3 m_Extent;
        u32 m_MipLevels;
        u32 m_Layers;
        vector<vk::ImageLayout> m_Layouts{};
        vk::Format m_Format;
        vk::ImageType m_Type;
        vk::ImageUsageFlags m_Usage;
        bool m_Managed;

        vk::Image m_VkImage;
        VmaAllocationInfo m_VmaAllocationInfo{};
        VmaAllocation m_VmaAllocation = nullptr;
    };
}
