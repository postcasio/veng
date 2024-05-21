#ifndef _COMMAND_BUFFER_H_
#define _COMMAND_BUFFER_H_

#include "../gfxcommon.h"
#include "command_pool.h"
#include "framebuffer.h"
#include "render_pass.h"
#include "swap_chain.h"
#include "graphics_pipeline.h"
#include "descriptor_set.h"
#include <vector>

class CommandPool;
class Framebuffer;
class DescriptorSet;

class CommandBuffer
{
public:
    CommandBuffer(CommandPool &pool);
    ~CommandBuffer();

    void reset();
    VkCommandBuffer currentBuffer();
    void begin();
    void beginRenderPass(RenderPass &renderPass, Framebuffer &framebuffer, SwapChain &swapChain);
    void bindPipeline(GraphicsPipeline &pipeline);
    void setViewport(const VkViewport *viewport);
    void setScissor(const VkRect2D *scissor);
    void bindDescriptorSet(PipelineLayout &layout, uint32_t firstSet, DescriptorSet &descriptorSet);
    void endRenderPass();
    void end();

    std::vector<VkCommandBuffer> buffers;
    VkCommandBufferAllocateInfo commandBufferAllocateInfo;
    CommandPool &pool;
};

#endif // _COMMAND_BUFFER_H_