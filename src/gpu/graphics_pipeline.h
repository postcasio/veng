#ifndef _GRAPHICS_PIPELINE_H_
#define _GRAPHICS_PIPELINE_H_

#include "../gfxcommon.h"
#include "pipeline_layout.h"
#include "render_pass.h"
#include "logical_device.h"

class GraphicsPipeline
{
public:
    GraphicsPipeline(LogicalDevice &device, PipelineLayout &layout, Shader &vertShader, Shader &fragShader, RenderPass &renderPass, VkCullModeFlagBits cullMode = VK_CULL_MODE_BACK_BIT, bool depthBiasEnable = false);
    ~GraphicsPipeline();
    void createPipeline(PipelineLayout &layout, RenderPass &renderPass, std::vector<VkPipelineShaderStageCreateInfo> shaderStages, VkCullModeFlagBits cullMode = VK_CULL_MODE_BACK_BIT, bool depthBiasEnable = false);

    VkGraphicsPipelineCreateInfo graphicsPipelineCreateInfo{};
    VkPipeline pipeline;

    LogicalDevice &device;
};

#endif