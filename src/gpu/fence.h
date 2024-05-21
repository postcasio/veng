#ifndef _GPU_FENCE_H_
#define _GPU_FENCE_H_

#include "../gfxcommon.h"

#include "logical_device.h"

class Fence
{
public:
    Fence(LogicalDevice &device, VkFenceCreateFlags flags);
    ~Fence();

    void wait();
    void reset();
    VkFenceCreateInfo fenceCreateInfo;
    VkFence fence;

    LogicalDevice &device;
};

#endif