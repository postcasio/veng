#include <Veng/Renderer/Backend/PipelineLayout.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    PipelineLayout::PipelineLayout(const PipelineLayoutInfo& info) : m_Name(info.Name),
                                                                     m_DescriptorSetLayouts(info.DescriptorSetLayouts),
                                                                     m_PushConstantRanges(info.PushConstantRanges)
    {
        vector<vk::DescriptorSetLayout> descriptorSetLayouts;
        descriptorSetLayouts.reserve(m_DescriptorSetLayouts.size());

        for (const auto& descriptorSetLayout : m_DescriptorSetLayouts)
        {
            descriptorSetLayouts.push_back(descriptorSetLayout->GetVkDescriptorSetLayout());
        }

        vector<vk::PushConstantRange> pushConstantRanges;
        pushConstantRanges.reserve(info.PushConstantRanges.size());

        for (const auto& pushConstantRangeInfo : info.PushConstantRanges)
        {
            pushConstantRanges.push_back({
                .stageFlags = pushConstantRangeInfo.Stages,
                .offset = pushConstantRangeInfo.Offset,
                .size = pushConstantRangeInfo.Size
            });
        }

        const vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo{
            .flags = info.Flags,
            .setLayoutCount = static_cast<u32>(descriptorSetLayouts.size()),
            .pSetLayouts = descriptorSetLayouts.data(),
            .pushConstantRangeCount = static_cast<u32>(pushConstantRanges.size()),
            .pPushConstantRanges = pushConstantRanges.data(),
        };

        m_VkPipelineLayout = Context::Instance().GetVkDevice().createPipelineLayout(pipelineLayoutCreateInfo).value;

        DebugMarkers::MarkPipelineLayout(m_VkPipelineLayout, m_Name);
    }

    PipelineLayout::~PipelineLayout()
    {
        Context::Instance().GetVkDevice().destroyPipelineLayout(m_VkPipelineLayout);
    }
}
