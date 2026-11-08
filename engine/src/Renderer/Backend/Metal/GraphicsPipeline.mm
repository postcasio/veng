#include <Veng/Renderer/GraphicsPipeline.h>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    GraphicsPipeline::Native& GraphicsPipeline::GetNative() const
    {
        return *m_Native;
    }

    GraphicsPipeline::GraphicsPipeline(Context& context, const GraphicsPipelineInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()),
          m_PipelineLayout(info.PipelineLayout)
    {
        VE_ASSERT(
            false,
            "GraphicsPipeline '{}': the Metal backend runs compute and transfer work only; it "
            "builds no render pipeline",
            m_Name);
    }

    GraphicsPipeline::~GraphicsPipeline() = default;
}
