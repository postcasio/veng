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
            return Ref<ComputePipeline>(new ComputePipeline(info));
        }

        ~ComputePipeline();

        ComputePipeline(const ComputePipeline&) = delete;
        ComputePipeline& operator=(const ComputePipeline&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit ComputePipeline(const ComputePipelineInfo& info);

        string m_Name;
        Unique<Native> m_Native;
        Ref<PipelineLayout> m_PipelineLayout;
    };
}

