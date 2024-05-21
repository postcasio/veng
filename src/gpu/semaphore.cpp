#include "semaphore.h"

Semaphore::Semaphore(LogicalDevice &device) : device(device)
{
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VK_CHECK_RESULT(vkCreateSemaphore(device.device, &semaphoreCreateInfo, nullptr, &semaphore), "failed to create semaphore!");
}

Semaphore::~Semaphore()
{
    vkDestroySemaphore(device.device, semaphore, nullptr);
}