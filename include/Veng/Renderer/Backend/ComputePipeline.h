#pragma once

#include <Veng/Renderer/Backend/PipelineLayout.h>
#include <Veng/Renderer/Backend/Shader.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct ComputePipelineInfo
    {
        string Name;

        Ref<PipelineLayout> PipelineLayout;
        PipelineShaderStageInfo ShaderStage;

        // Optional specialization constants for the compute shader.
        vk::SpecializationInfo* SpecializationInfo = nullptr;
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

        [[nodiscard]] string GetName() const { return m_Name; }
        [[nodiscard]] vk::Pipeline GetVkPipeline() const { return m_VkPipeline; }
        [[nodiscard]] Ref<PipelineLayout> GetPipelineLayout() const { return m_PipelineLayout; }

    private:
        string m_Name;
        vk::Pipeline m_VkPipeline{};
        Ref<PipelineLayout> m_PipelineLayout;
    };
}

