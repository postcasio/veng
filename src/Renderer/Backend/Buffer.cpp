#include <Veng/Renderer/Backend/Buffer.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>


namespace Veng::Renderer
{
    Buffer::Buffer(const BufferInfo& info) : m_Name(info.Name), m_Size(info.Size)
    {
        const VkBufferCreateInfo bufferCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = m_Size,
            .usage = static_cast<VkBufferUsageFlags>(info.Usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };

        VkBuffer buffer;

        const VmaAllocationCreateInfo allocationCreateInfo = {
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            .pool = VK_NULL_HANDLE,
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT
        };

        VK_RAW_ASSERT(
            vmaCreateBuffer(Context::Instance().GetAllocator(), &bufferCreateInfo, &allocationCreateInfo, &buffer, &
                m_VmaAllocation, nullptr), "failed to create buffer!");

        m_VkBuffer = buffer;

        vmaSetAllocationName(Context::Instance().GetAllocator(), m_VmaAllocation, info.Name.c_str());

        DebugMarkers::MarkBuffer(m_VkBuffer, m_Name);
    }

    Buffer::~Buffer()
    {
        vmaDestroyBuffer(Context::Instance().GetAllocator(), m_VkBuffer, m_VmaAllocation);
    }

    void Buffer::Upload(const std::span<u8> data) const
    {
        VK_RAW_ASSERT(
            vmaCopyMemoryToAllocation(Context::Instance().GetAllocator(), data.data(), m_VmaAllocation, 0, data.size()),
            "failed to upload buffer data!");
    }

    std::span<u8> Buffer::Download() const
    {
        void* data = calloc(m_Size, sizeof(u8));

        VK_RAW_ASSERT(vmaCopyAllocationToMemory(Context::Instance().GetAllocator(), m_VmaAllocation, 0, data, m_Size),
                      "failed to download buffer data!");

        return {static_cast<u8*>(data), m_Size};
    }
}
