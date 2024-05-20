#ifndef _GPU_RENDER_PASS_H_
#define _GPU_RENDER_PASS_H_

#include "../gfxcommon.h"

class RenderPass
{
public:
    RenderPass(std::vector<VkAttachmentDescription> attachments, VkSubpassDescription subpass);
    ~RenderPass();

    VkRenderPass renderPass;
    VkRenderPassCreateInfo renderPassCreateInfo{};
};

#endif