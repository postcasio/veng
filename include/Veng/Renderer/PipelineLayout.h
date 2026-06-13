#pragma once

#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Shader.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    struct PipelineShaderStageInfo
    {
        ShaderStage Stage;
        Ref<Shader> Module;
    };

    struct PipelineAttachmentInfo
    {
        Format Format = Format::Undefined;
        BlendState Blend = BlendState::Opaque();
    };

    // The shape of one push-constant range. Declared once on the pipeline
    // layout; CommandBuffer::PushConstants<T> reads it back to recover the
    // stages, offset, and size that would otherwise be restated at the call
    // site.
    struct PushConstantRange
    {
        ShaderStage Stages{};
        u32 Offset = 0;
        u32 Size{};

        // Typed helper so the size is never hand-written:
        template <typename T>
        static PushConstantRange Of(ShaderStage stages, u32 offset = 0)
        {
            return {.Stages = stages, .Offset = offset, .Size = sizeof(T)};
        }
    };

    struct PipelineLayoutInfo
    {
        string Name;
        vector<Ref<DescriptorSetLayout>> DescriptorSetLayouts{};
        vector<PushConstantRange> PushConstantRanges{};
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

        [[nodiscard]] const vector<PushConstantRange>& GetPushConstantRanges() const
        {
            return m_PushConstantRanges;
        }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
        vector<Ref<DescriptorSetLayout>> m_DescriptorSetLayouts;
        vector<PushConstantRange> m_PushConstantRanges;
    };
}
