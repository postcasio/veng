#include <Veng/Renderer/Backend/ComputePipeline.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    ComputePipeline::ComputePipeline(const ComputePipelineInfo& info)
        : m_Name(info.Name), m_PipelineLayout(info.PipelineLayout)
    {
        VE_ASSERT(m_PipelineLayout != nullptr, "ComputePipeline '{}' requires a PipelineLayout", m_Name);
        VE_ASSERT(info.ShaderStage.Stage == ShaderStage::Compute,
                  "ComputePipeline '{}' requires a compute shader stage", m_Name);

        const vk::PipelineShaderStageCreateInfo shaderStageInfo{
            .stage = ToVkBit(info.ShaderStage.Stage),
            .module = info.ShaderStage.Module.GetVkModule(),
            // Shader stores/owns this string; it must live through pipeline creation.
            .pName = info.ShaderStage.Module.GetEntryPoint().c_str(),
            .pSpecializationInfo = info.SpecializationInfo,
        };

        const vk::ComputePipelineCreateInfo pipelineCreateInfo{
            .stage = shaderStageInfo,
            .layout = m_PipelineLayout->GetVkPipelineLayout(),
            .basePipelineHandle = nullptr,
            .basePipelineIndex = 0,
        };

        m_VkPipeline = Context::Instance()
                           .GetVkDevice()
                           .createComputePipeline(nullptr, pipelineCreateInfo)
                           .value;

        DebugMarkers::MarkPipeline(m_VkPipeline, m_Name);
    }

    ComputePipeline::~ComputePipeline()
    {
        if (m_VkPipeline)
        {
            Context::Instance().Retire(m_VkPipeline);
            m_VkPipeline = nullptr;
        }
    }
}
