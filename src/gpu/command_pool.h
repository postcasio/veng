#ifndef _COMMAND_POOL_H_
#define _COMMAND_POOL_H_

#include "../gfxcommon.h"
#include "command_buffer.h"
#include "logical_device.h"

class CommandBuffer;

class CommandPool
{
public:
    CommandPool(LogicalDevice &device);
    ~CommandPool();

    std::unique_ptr<CommandBuffer> createCommandBuffer();
    void destroyCommandBuffers(std::vector<VkCommandBuffer> buffers);

    VkCommandPool pool;
    LogicalDevice &device;
};

#endif // _COMMAND_POOL_H_