#ifndef _PIPELINE_LAYOUT_H_
#define _PIPELINE_LAYOUT_H_

#include "../gfxcommon.h"
#include "shader.h"
#include "descriptor_set_layout.h"

class PipelineLayout
{
public:
    PipelineLayout(std::vector<DescriptorSetLayout *> descriptorSetLayouts);
    ~PipelineLayout();

    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
};

#endif