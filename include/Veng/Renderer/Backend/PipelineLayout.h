#pragma once

#include <Veng/Renderer/Backend/DescriptorSetLayout.h>
#include <Veng/Renderer/Backend/Shader.h>

namespace Veng::Renderer
{
    struct PipelineShaderStageInfo
    {
        vk::ShaderStageFlagBits Stage;
        Shader* Module;
    };

    struct PipelineAttachmentInfo
    {
        vk::Format Format = vk::Format::eUndefined;
        vk::PipelineColorBlendAttachmentState BlendMode = {
            .blendEnable = VK_FALSE,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };
    };

    struct PipelineLayoutPushConstantRangeInfo
    {
        vk::ShaderStageFlags Stages{};
        u32 Offset = 0;
        u32 Size;
    };

    struct PipelineLayoutInfo
    {
        string Name;
        vk::PipelineLayoutCreateFlags Flags{};
        vector<Ref<DescriptorSetLayout>> DescriptorSetLayouts{};
        vector<PipelineLayoutPushConstantRangeInfo> PushConstantRanges{};
    };

    class PipelineLayout
    {
    public:
        static Ref<PipelineLayout> Create(const PipelineLayoutInfo& info)
        {
            return CreateRef<PipelineLayout>(info);
        }

        explicit PipelineLayout(const PipelineLayoutInfo& info);
        ~PipelineLayout();

        [[nodiscard]] vk::PipelineLayout GetVkPipelineLayout() const { return m_VkPipelineLayout; }
        [[nodiscard]] string GetName() const { return m_Name; }

        [[nodiscard]] const vector<PipelineLayoutPushConstantRangeInfo>& GetPushConstantRanges() const
        {
            return m_PushConstantRanges;
        }

    private:
        string m_Name;
        vk::PipelineLayout m_VkPipelineLayout = nullptr;
        vector<Ref<DescriptorSetLayout>> m_DescriptorSetLayouts;
        vector<PipelineLayoutPushConstantRangeInfo> m_PushConstantRanges;
    };
}
