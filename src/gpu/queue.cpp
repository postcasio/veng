#include "queue.h"

Queue::Queue(LogicalDevice &device, uint32_t familyIndex, uint32_t queueIndex) : device(device)
{
    vkGetDeviceQueue(device.device, familyIndex, queueIndex, &queue);
}

Queue::~Queue()
{
    // nothing to do here
}

void Queue::submit(VkSubmitInfo &submitInfo)
{
    VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE), "failed to submit to queue!");
}

void Queue::submit(VkSubmitInfo &submitInfo, Fence &fence)
{

    VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence.fence), "failed to submit to queue!");
}

VkResult Queue::present(VkPresentInfoKHR &presentInfo)
{
    return vkQueuePresentKHR(queue, &presentInfo);
}

void Queue::waitIdle()
{
    vkQueueWaitIdle(queue);
}