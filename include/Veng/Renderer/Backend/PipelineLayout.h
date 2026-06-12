#pragma once

#include <Veng/Renderer/Backend/DescriptorSetLayout.h>
#include <Veng/Renderer/Backend/Shader.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    struct PipelineShaderStageInfo
    {
        ShaderStage Stage;
        const Shader& Module;
    };

    struct PipelineAttachmentInfo
    {
        Format Format = Format::Undefined;
        BlendState Blend = BlendState::Opaque();
    };

    struct PipelineLayoutPushConstantRangeInfo
    {
        ShaderStage Stages{};
        u32 Offset = 0;
        u32 Size;
    };

    struct PipelineLayoutInfo
    {
        string Name;
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
        [[nodiscard]] const string& GetName() const { return m_Name; }

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
