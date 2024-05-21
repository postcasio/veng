#include "framebuffer.h"
#include "../engine.h"
#include "swap_chain.h"

Framebuffer::Framebuffer(SwapChain *chain, RenderPass *renderPass, ImageView *depthImageView, ImageView *colorImageView)
{
    framebuffers.resize(chain->imageCount);

    for (size_t i = 0; i < chain->imageCount; i++)
    {
#ifdef ENABLE_MULTISAMPLING
        std::vector<VkImageView> attachments = {
            colorImageView->view,
            depthImageView->view,
            chain->imageViews[i]->view};
#else
        std::vector<VkImageView> attachments = {
            chain->imageViews[i]->view,
            depthImageView->view};
#endif

        VkFramebufferCreateInfo framebufferInfo{};

        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass->renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = chain->extent.width;
        framebufferInfo.height = chain->extent.height;
        framebufferInfo.layers = 1;

        VK_CHECK_RESULT(vkCreateFramebuffer(renderer()->device->device, &framebufferInfo, nullptr, &framebuffers[i]), "failed to create framebuffer!");
    }
}

Framebuffer::~Framebuffer()
{
    for (auto framebuffer : framebuffers)
    {
        vkDestroyFramebuffer(device()->device, framebuffer, nullptr);
    }
}

VkFramebuffer Framebuffer::currentFramebuffer()
{
    return framebuffers[currentImage()];
}