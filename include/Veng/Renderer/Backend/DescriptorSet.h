#pragma once
#include <variant>

#include <Veng/Renderer/Backend/Buffer.h>
#include <Veng/Renderer/Backend/DescriptorSetLayout.h>
#include <Veng/Renderer/Backend/ImageView.h>
#include <Veng/Renderer/Backend/Sampler.h>

namespace Veng::Renderer
{
    struct DescriptorSetInfo
    {
        string Name;
        Ref<DescriptorSetLayout> Layout;
    };

    struct DescriptorSetImageBinding
    {
        Ref<ImageView> ImageView;
        Ref<Sampler> Sampler;

        DescriptorSetImageBinding(const Ref<Renderer::ImageView>& imageView,
                                  const Ref<Renderer::Sampler>& sampler) : ImageView(imageView), Sampler(sampler)
        {
        }

        ~DescriptorSetImageBinding() = default;
    };

    struct DescriptorImageInfo
    {
        Ref<ImageView> ImageView;
        Ref<Sampler> Sampler;
        vk::ImageLayout ImageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    };

    struct DescriptorBufferInfo
    {
        Ref<Buffer> Buffer;
        vk::DeviceSize Offset = 0;
        vk::DeviceSize Range = VK_WHOLE_SIZE;
    };

    struct DescriptorSetWriteInfo
    {
        vk::DescriptorType Type;
        u32 Binding = 0;
        u32 ArrayElement = 0;
        std::variant<vector<DescriptorImageInfo>, vector<DescriptorBufferInfo>> Data;
    };

    struct DescriptorSetUpdateInfo
    {
        vector<DescriptorSetWriteInfo> Writes;
    };

    class DescriptorSet
    {
    public:
        static Ref<DescriptorSet> Create(const DescriptorSetInfo& info)
        {
            return CreateRef<DescriptorSet>(info);
        }

        explicit DescriptorSet(const DescriptorSetInfo& info);
        ~DescriptorSet();

        void UpdateDescriptorSet(const DescriptorSetUpdateInfo& info);

        [[nodiscard]] string GetName() const { return m_Name; }
        [[nodiscard]] vk::DescriptorSet GetVkDescriptorSet() const { return m_DescriptorSet; }

    private:
        string m_Name;
        vk::DescriptorSet m_DescriptorSet;
        vector<Ref<void>> m_BoundResources{};
        Ref<DescriptorSetLayout> m_Layout;
    };
}
