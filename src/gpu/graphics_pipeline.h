#ifndef _GRAPHICS_PIPELINE_H_
#define _GRAPHICS_PIPELINE_H_

#include "../gfxcommon.h"
#include "pipeline_layout.h"
#include "render_pass.h"
#include "logical_device.h"

class GraphicsPipeline
{
public:
    GraphicsPipeline(LogicalDevice &device, PipelineLayout &layout, Shader &vertShader, Shader &fragShader, RenderPass &renderPass);
    ~GraphicsPipeline();

    VkGraphicsPipelineCreateInfo graphicsPipelineCreateInfo{};
    VkPipeline pipeline;

    LogicalDevice &device;
};

#endif