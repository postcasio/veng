#include "command_pool.h"
#include "../renderer.h"
#include "logical_device.h"

CommandPool::CommandPool(LogicalDevice &device) : device(device)
{
    QueueFamilyIndices queueFamilyIndices = device.physicalDevice.findQueueFamilies();

    VkCommandPoolCreateInfo commandPoolCreateInfo{};
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolCreateInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    VK_CHECK_RESULT(vkCreateCommandPool(device.device, &commandPoolCreateInfo, nullptr, &pool), "failed to create command pool!");
}

CommandPool::~CommandPool()
{
    device.destroyCommandPool(pool);
}

std::unique_ptr<CommandBuffer> CommandPool::createCommandBuffer()
{
    return std::make_unique<CommandBuffer>(*this);
}

void CommandPool::destroyCommandBuffers(std::vector<VkCommandBuffer> buffers)
{
    vkFreeCommandBuffers(device.device, pool, static_cast<uint32_t>(buffers.size()), buffers.data());
}