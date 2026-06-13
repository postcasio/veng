#pragma once

#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Shader.h>

namespace Veng::Renderer
{
    struct ComputePipelineInfo
    {
        string Name;

        Ref<PipelineLayout> PipelineLayout;
        PipelineShaderStageInfo ShaderStage;
    };

    class ComputePipeline
    {
    public:
        static Ref<ComputePipeline> Create(const ComputePipelineInfo& info)
        {
            return CreateRef<ComputePipeline>(info);
        }

        explicit ComputePipeline(const ComputePipelineInfo& info);
        ~ComputePipeline();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
        Ref<PipelineLayout> m_PipelineLayout;
    };
}

