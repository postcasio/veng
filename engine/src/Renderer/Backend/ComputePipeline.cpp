#include <Veng/Renderer/ComputePipeline.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    ComputePipeline::Native& ComputePipeline::GetNative() const { return *m_Native; }

    ComputePipeline::ComputePipeline(Context& context, const ComputePipelineInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()), m_PipelineLayout(info.PipelineLayout)
    {
        VE_ASSERT(m_PipelineLayout != nullptr, "ComputePipeline '{}' requires a PipelineLayout", m_Name);
        VE_ASSERT(info.ShaderStage.Stage == ShaderStage::Compute,
                  "ComputePipeline '{}' requires a compute shader stage", m_Name);

        const vk::PipelineShaderStageCreateInfo shaderStageInfo{
            .stage = ToVkBit(info.ShaderStage.Stage),
            .module = info.ShaderStage.Module->GetNative().Module,
            // Must outlive createComputePipeline; ShaderModule holds the string.
            .pName = info.ShaderStage.Module->GetEntryPoint().c_str(),
            .pSpecializationInfo = nullptr,
        };

        const vk::ComputePipelineCreateInfo pipelineCreateInfo{
            .stage = shaderStageInfo,
            .layout = m_PipelineLayout->GetNative().Layout,
            .basePipelineHandle = nullptr,
            .basePipelineIndex = 0,
        };

        m_Native->Pipeline = GetVkDevice(m_Context)
                           .createComputePipeline(GetVkPipelineCache(m_Context), pipelineCreateInfo)
                           .value;

        DebugMarkers::MarkPipeline(GetVkDevice(m_Context), m_Native->Pipeline, m_Name);
    }

    ComputePipeline::~ComputePipeline()
    {
        if (m_Native->Pipeline)
        {
            m_Context.GetNative().Retire(m_Native->Pipeline);
            m_Native->Pipeline = nullptr;
        }
    }
}
