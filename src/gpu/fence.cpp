#include "fence.h"

Fence::Fence(LogicalDevice &device, VkFenceCreateFlags flags) : device(device)
{
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags = flags;

    VK_CHECK_RESULT(vkCreateFence(device.device, &fenceCreateInfo, nullptr, &fence), "failed to create fence!");
}

Fence::~Fence()
{
    device.destroyFence(fence);
}

void Fence::wait()
{
    vkWaitForFences(device.device, 1, &fence, VK_TRUE, UINT64_MAX);
}

void Fence::reset()
{
    vkResetFences(device.device, 1, &fence);
}