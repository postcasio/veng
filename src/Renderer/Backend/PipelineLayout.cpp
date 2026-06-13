#include <Veng/Renderer/PipelineLayout.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    PipelineLayout::Native& PipelineLayout::GetNative() const { return *m_Native; }

    PipelineLayout::PipelineLayout(const PipelineLayoutInfo& info) : m_Context(Context::Instance()), m_Name(info.Name),
                                                                     m_Native(CreateUnique<Native>()),
                                                                     m_DescriptorSetLayouts(info.DescriptorSetLayouts),
                                                                     m_PushConstantRanges(info.PushConstantRanges)
    {
        vector<vk::DescriptorSetLayout> descriptorSetLayouts;
        descriptorSetLayouts.reserve(m_DescriptorSetLayouts.size());

        for (const auto& descriptorSetLayout : m_DescriptorSetLayouts)
        {
            descriptorSetLayouts.push_back(descriptorSetLayout->GetNative().Layout);
        }

        vector<vk::PushConstantRange> pushConstantRanges;
        pushConstantRanges.reserve(info.PushConstantRanges.size());

        for (const auto& pushConstantRange : info.PushConstantRanges)
        {
            pushConstantRanges.push_back({
                .stageFlags = ToVk(pushConstantRange.Stages),
                .offset = pushConstantRange.Offset,
                .size = pushConstantRange.Size
            });
        }

        const vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo{
            .setLayoutCount = static_cast<u32>(descriptorSetLayouts.size()),
            .pSetLayouts = descriptorSetLayouts.data(),
            .pushConstantRangeCount = static_cast<u32>(pushConstantRanges.size()),
            .pPushConstantRanges = pushConstantRanges.data(),
        };

        m_Native->Layout = GetVkDevice(m_Context).createPipelineLayout(pipelineLayoutCreateInfo).value;

        DebugMarkers::MarkPipelineLayout(m_Native->Layout, m_Name);
    }

    PipelineLayout::~PipelineLayout()
    {
        m_Context.GetNative().Retire(m_Native->Layout);
    }
}
