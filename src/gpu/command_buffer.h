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
    void begin(VkCommandBufferUsageFlags flags = 0);
    void beginRenderPass(RenderPass &renderPass, Framebuffer &framebuffer, VkExtent2D extent, std::vector<VkClearValue> clearValues);
    void bindPipeline(GraphicsPipeline &pipeline);
    void setViewport(const VkViewport viewport);
    void setScissor(const VkRect2D scissor);
    void bindDescriptorSet(PipelineLayout &layout, uint32_t firstSet, DescriptorSet &descriptorSet);
    void endRenderPass();
    void copyBufferToImage(BufferAllocation &buffer, ImageAllocation &image, uint32_t width, uint32_t height);
    void pipelineBarrier(VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers);
    void pushConstants(PipelineLayout &layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void *pValues);
    void setDepthBias(float constantFactor, float clamp, float slopeFactor);
    void end();

    std::vector<VkCommandBuffer> buffers;
    VkCommandBufferAllocateInfo commandBufferAllocateInfo;
    CommandPool &pool;
};

#endif // _COMMAND_BUFFER_H_