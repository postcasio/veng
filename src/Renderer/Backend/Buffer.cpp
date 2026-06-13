#include <Veng/Renderer/Buffer.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    Buffer::Native& Buffer::GetNative() const { return *m_Native; }

    Buffer::Buffer(const BufferInfo& info) : m_Name(info.Name), m_Native(CreateUnique<Native>()), m_Size(info.Size)
    {
        const VkBufferCreateInfo bufferCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = m_Size,
            .usage = static_cast<VkBufferUsageFlags>(ToVk(info.Usage)),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };

        VkBuffer buffer;

        const VmaAllocationCreateInfo allocationCreateInfo = {
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            .pool = VK_NULL_HANDLE
        };

        VK_RAW_ASSERT(
            vmaCreateBuffer(GetVmaAllocator(Context::Instance()), &bufferCreateInfo, &allocationCreateInfo, &buffer, &
                m_Native->Allocation, nullptr), "failed to create buffer!");

        m_Native->Buffer = buffer;

        vmaSetAllocationName(GetVmaAllocator(Context::Instance()), m_Native->Allocation, info.Name.c_str());

        DebugMarkers::MarkBuffer(m_Native->Buffer, m_Name);
    }

    Buffer::~Buffer()
    {
        Context::Instance().GetNative().Retire(m_Native->Buffer, m_Native->Allocation);
    }

    void Buffer::Upload(const std::span<const u8> data) const
    {
        VK_RAW_ASSERT(
            vmaCopyMemoryToAllocation(GetVmaAllocator(Context::Instance()), data.data(), m_Native->Allocation, 0, data.size()),
            "failed to upload buffer data!");
    }

    vector<u8> Buffer::Download() const
    {
        vector<u8> data(m_Size);

        VK_RAW_ASSERT(
            vmaCopyAllocationToMemory(GetVmaAllocator(Context::Instance()), m_Native->Allocation, 0, data.data(), m_Size),
            "failed to download buffer data!");

        return data;
    }
}
