#include "pipeline_layout.h"
#include "../engine.h"

PipelineLayout::PipelineLayout(LogicalDevice &device, std::vector<DescriptorSetLayout *> descriptorSetLayouts) : device(device)
{
    std::vector<VkDescriptorSetLayout> layouts;
    layouts.resize(descriptorSetLayouts.size());
    for (size_t i = 0; i < descriptorSetLayouts.size(); i++)
    {
        layouts[i] = descriptorSetLayouts[i]->layout;
    }

    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = layouts.size();
    pipelineLayoutInfo.pSetLayouts = layouts.data();

    VK_CHECK_RESULT(vkCreatePipelineLayout(device.device, &pipelineLayoutInfo, nullptr, &layout), "failed to create pipeline layout!");
}

PipelineLayout::~PipelineLayout()
{
    device.destroyPipelineLayout(layout);
}