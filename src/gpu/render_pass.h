#ifndef _GPU_RENDER_PASS_H_
#define _GPU_RENDER_PASS_H_

#include "../gfxcommon.h"
#include "logical_device.h"

class RenderPass
{
public:
    RenderPass(LogicalDevice &device, std::vector<VkAttachmentDescription> attachments, VkSubpassDescription subpass);
    ~RenderPass();

    VkRenderPass renderPass;
    VkRenderPassCreateInfo renderPassCreateInfo{};

    std::vector<VkAttachmentDescription> attachments;
    LogicalDevice &device;
};

#endif