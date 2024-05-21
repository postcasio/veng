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

void CommandBuffer::begin()
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;                  // Optional
    beginInfo.pInheritanceInfo = nullptr; // Optional

    VK_CHECK_RESULT(vkBeginCommandBuffer(currentBuffer(), &beginInfo), "failed to begin recording command buffer!");
}

void CommandBuffer::beginRenderPass(RenderPass &renderPass, Framebuffer &framebuffer, SwapChain &swapChain)
{
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass.renderPass;
    renderPassInfo.framebuffer = framebuffer.currentFramebuffer();

    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapChain.extent;

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(currentBuffer(), &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void CommandBuffer::bindPipeline(GraphicsPipeline &pipeline)
{
    vkCmdBindPipeline(currentBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
}

void CommandBuffer::setViewport(const VkViewport *viewport)
{
    vkCmdSetViewport(currentBuffer(), 0, 1, viewport);
}

void CommandBuffer::setScissor(const VkRect2D *scissor)
{
    vkCmdSetScissor(currentBuffer(), 0, 1, scissor);
}

void CommandBuffer::bindDescriptorSet(PipelineLayout &layout, uint32_t firstSet, DescriptorSet &descriptorSet)
{
    vkCmdBindDescriptorSets(currentBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, firstSet, 1, &descriptorSet.sets[currentFrame()], 0, nullptr);
}

void CommandBuffer::endRenderPass()
{
    vkCmdEndRenderPass(currentBuffer());
}

void CommandBuffer::end()
{
    VK_CHECK_RESULT(vkEndCommandBuffer(currentBuffer()), "failed to record command buffer!");
}
