#include "buffer_allocation.h"
#include "../engine.h"

BufferAllocation::BufferAllocation(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
{
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = size;
    bufferCreateInfo.usage = usage;
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationCreateInfo.requiredFlags = properties;
    allocationCreateInfo.pool = VK_NULL_HANDLE;
    allocationCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT;
    allocationCreateInfo.pUserData = (void *)"BufferAllocation";

    VK_CHECK_RESULT(vmaCreateBuffer(renderer()->allocator, &bufferCreateInfo, &allocationCreateInfo, &buffer, &allocation, nullptr), "failed to create buffer!");
}

BufferAllocation::~BufferAllocation()
{
    vmaDestroyBuffer(renderer()->allocator, buffer, allocation);
}

void BufferAllocation::copyMemoryToAllocation(const void *pSrcHostPtr, VkDeviceSize offset, VkDeviceSize size)
{
    VK_CHECK_RESULT(vmaCopyMemoryToAllocation(renderer()->allocator, pSrcHostPtr, allocation, offset, size), "failed to copy memory to allocation!");
}
