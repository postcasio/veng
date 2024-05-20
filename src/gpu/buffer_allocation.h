#ifndef _GPU_BUFFER_ALLOCATION_H_
#define _GPU_BUFFER_ALLOCATION_H_

#include "../gfxcommon.h"

class BufferAllocation
{
public:
    BufferAllocation(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    ~BufferAllocation();

    VmaAllocation allocation;
    VkBuffer buffer;
    VkBufferCreateInfo bufferCreateInfo{};
    VmaAllocationCreateInfo allocationCreateInfo{};

    void copyMemoryToAllocation(const void *pSrcHostPtr, VkDeviceSize offset, VkDeviceSize size);
};

#endif