#ifndef _FRAME_BUFFER_H_
#define _FRAME_BUFFER_H_

#include "../gfxcommon.h"
#include "swap_chain.h"
#include "render_pass.h"
#include "image_view.h"

class Framebuffer
{
public:
    Framebuffer(SwapChain *chain, RenderPass *renderPass, ImageView *depthImageView, ImageView *colorImageView);
    Framebuffer(SwapChain *chain, RenderPass *renderPass, std::vector<std::unique_ptr<ImageView>> &depthImageViews);
    ~Framebuffer();

    VkFramebuffer currentFramebuffer();

    std::vector<VkFramebuffer> framebuffers;
};

#endif // _FRAME_BUFFER_H_