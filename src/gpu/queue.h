#ifndef _GPU_QUEUE_H_
#define _GPU_QUEUE_H_

#include "../gfxcommon.h"

#include "logical_device.h"

class Queue
{
public:
    Queue(LogicalDevice &device, uint32_t familyIndex, uint32_t queueIndex);
    ~Queue();

    void submit(VkSubmitInfo &submitInfo, Fence &fence);
    void submit(VkSubmitInfo &submitInfo);
    VkResult present(VkPresentInfoKHR &presentInfo);
    void waitIdle();

    VkQueue queue;

    LogicalDevice &device;
};

#endif