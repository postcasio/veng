#include "command_buffer.h"
#include "command_pool.h"
#include "../engine.h"

CommandBuffer::CommandBuffer(CommandPool &pool) : pool(pool)
{
    buffers.resize(MAX_FRAMES_IN_FLIGHT);

    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = pool.pool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = (uint32_t)buffers.size();

    VK_CHECK_RESULT(vkAllocateCommandBuffers(renderer()->device->device, &commandBufferAllocateInfo, buffers.data()), "failed to allocate command buffers!");
}

VkCommandBuffer CommandBuffer::currentBuffer()
{
    return buffers[currentFrame()];
}

CommandBuffer::~CommandBuffer()
{
    pool.destroyCommandBuffers(buffers);
}

void CommandBuffer::reset()
{
    vkResetCommandBuffer(currentBuffer(), 0);
}

void CommandBuffer::begin(VkCommandBufferUsageFlags flags)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = flags;              // Optional
    beginInfo.pInheritanceInfo = nullptr; // Optional

    VK_CHECK_RESULT(vkBeginCommandBuffer(currentBuffer(), &beginInfo), "failed to begin recording command buffer!");
}

void CommandBuffer::beginRenderPass(RenderPass &renderPass, Framebuffer &framebuffer, VkExtent2D extent, std::vector<VkClearValue> clearValues)
{
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass.renderPass;
    renderPassInfo.framebuffer = framebuffer.currentFramebuffer();

    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(currentBuffer(), &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void CommandBuffer::bindPipeline(GraphicsPipeline &pipeline)
{
    vkCmdBindPipeline(currentBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
}

void CommandBuffer::setViewport(const VkViewport viewport)
{
    vkCmdSetViewport(currentBuffer(), 0, 1, &viewport);
}

void CommandBuffer::setScissor(const VkRect2D scissor)
{
    vkCmdSetScissor(currentBuffer(), 0, 1, &scissor);
}

void CommandBuffer::bindDescriptorSet(PipelineLayout &layout, uint32_t firstSet, DescriptorSet &descriptorSet)
{
    vkCmdBindDescriptorSets(currentBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, firstSet, 1, &descriptorSet.sets[currentFrame()], 0, nullptr);
}

void CommandBuffer::endRenderPass()
{
    vkCmdEndRenderPass(currentBuffer());
}

void CommandBuffer::copyBufferToImage(BufferAllocation &buffer, ImageAllocation &image, uint32_t width, uint32_t height)
{
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = {0, 0, 0};
    region.imageExtent = {
        width,
        height,
        1};

    vkCmdCopyBufferToImage(
        currentBuffer(),
        buffer.buffer,
        image.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);
}

void CommandBuffer::pipelineBarrier(VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers)
{
    vkCmdPipelineBarrier(
        currentBuffer(),
        srcStageMask,
        dstStageMask,
        dependencyFlags,
        memoryBarrierCount,
        pMemoryBarriers,
        bufferMemoryBarrierCount,
        pBufferMemoryBarriers,
        imageMemoryBarrierCount,
        pImageMemoryBarriers);
}

void CommandBuffer::end()
{
    VK_CHECK_RESULT(vkEndCommandBuffer(currentBuffer()), "failed to record command buffer!");
}

void CommandBuffer::pushConstants(PipelineLayout &layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void *pValues)
{
    vkCmdPushConstants(currentBuffer(), layout.layout, stageFlags, offset, size, pValues);
}

void CommandBuffer::setDepthBias(float constantFactor, float clamp, float slopeFactor)
{
    std::cout << "Depth bias " << constantFactor << " " << clamp << " " << slopeFactor << std::endl;
    vkCmdSetDepthBias(currentBuffer(), constantFactor, clamp, slopeFactor);
}