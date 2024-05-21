#ifndef _GPU_SEMAPHORE_H_
#define _GPU_SEMAPHORE_H_

#include "../gfxcommon.h"
#include "logical_device.h"

class Semaphore
{

public:
    Semaphore(LogicalDevice &device);
    ~Semaphore();

    VkSemaphoreCreateInfo semaphoreCreateInfo;
    VkSemaphore semaphore;

    LogicalDevice &device;
};

#endif