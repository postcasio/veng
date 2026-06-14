#pragma once

#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/ShaderModule.h>

namespace Veng::Renderer
{
    class Context;

    struct ComputePipelineInfo
    {
        string Name;

        Ref<PipelineLayout> PipelineLayout;
        PipelineShaderStageInfo ShaderStage;
    };

    class ComputePipeline
    {
    public:
        static Ref<ComputePipeline> Create(Context& context, const ComputePipelineInfo& info)
        {
            return Ref<ComputePipeline>(new ComputePipeline(context, info));
        }

        ~ComputePipeline();

        ComputePipeline(const ComputePipeline&) = delete;
        ComputePipeline& operator=(const ComputePipeline&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        ComputePipeline(Context& context, const ComputePipelineInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Context& m_Context;
        string m_Name;
        Unique<Native> m_Native;
        Ref<PipelineLayout> m_PipelineLayout;
    };
}

