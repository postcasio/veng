#pragma once

#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Shader.h>
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

        [[nodiscard]] const string& GetName() const { return m_Name; }

        [[nodiscard]] const vector<PipelineLayoutPushConstantRangeInfo>& GetPushConstantRanges() const
        {
            return m_PushConstantRanges;
        }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
        vector<Ref<DescriptorSetLayout>> m_DescriptorSetLayouts;
        vector<PipelineLayoutPushConstantRangeInfo> m_PushConstantRanges;
    };
}
