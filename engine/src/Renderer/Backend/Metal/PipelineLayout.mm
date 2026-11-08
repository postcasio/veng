#include <Veng/Renderer/PipelineLayout.h>

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    PipelineLayout::Native& PipelineLayout::GetNative() const
    {
        return *m_Native;
    }

    PipelineLayout::PipelineLayout(Context& context, const PipelineLayoutInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()),
          m_DescriptorSetLayouts(info.DescriptorSetLayouts),
          m_PushConstantRanges(info.PushConstantRanges)
    {
        // Sets 0-2 are the bindless registry's in every layout, as on every backend.
        const BindlessRegistry& registry = context.GetBindlessRegistry();
        vector<u32> dynamicCounts{
            registry.GetSet0Layout()->GetNative().Placement.DynamicCount,
            registry.GetVolumeSetLayout()->GetNative().Placement.DynamicCount,
            registry.GetCubeSetLayout()->GetNative().Placement.DynamicCount,
        };
        for (const Ref<DescriptorSetLayout>& layout : m_DescriptorSetLayouts)
        {
            dynamicCounts.push_back(layout->GetNative().Placement.DynamicCount);
        }
        m_Native->DynamicSlots = Metal::DynamicBufferSlots(m_Name, dynamicCounts);

        for (const PushConstantRange& range : m_PushConstantRanges)
        {
            m_Native->PushBytes = std::max(m_Native->PushBytes, range.Offset + range.Size);
        }
        VE_ASSERT(m_Native->PushBytes <= Metal::MaxPushConstantBytes,
                  "PipelineLayout '{}': a {}-byte push block is past the {} the Metal backend "
                  "carries",
                  m_Name, m_Native->PushBytes, Metal::MaxPushConstantBytes);
    }

    PipelineLayout::~PipelineLayout() = default;
}
