#ifndef _PIPELINE_LAYOUT_H_
#define _PIPELINE_LAYOUT_H_

#include "../gfxcommon.h"
#include "shader.h"
#include "descriptor_set_layout.h"
#include "logical_device.h"

class PipelineLayout
{
public:
    PipelineLayout(LogicalDevice &device, std::vector<DescriptorSetLayout *> descriptorSetLayouts);
    PipelineLayout(LogicalDevice &device, std::vector<DescriptorSetLayout *> descriptorSetLayouts, VkPushConstantRange pushConstantRange);
    ~PipelineLayout();

    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};

    LogicalDevice &device;
};

#endif