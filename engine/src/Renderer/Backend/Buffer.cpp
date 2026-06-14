#include <Veng/Renderer/Buffer.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    Buffer::Native& Buffer::GetNative() const { return *m_Native; }

    Buffer::Buffer(Context& context, const BufferInfo& info) : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()), m_Size(info.Size)
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
            vmaCreateBuffer(GetVmaAllocator(m_Context), &bufferCreateInfo, &allocationCreateInfo, &buffer, &
                m_Native->Allocation, nullptr), "failed to create buffer!");

        m_Native->Buffer = buffer;

        vmaSetAllocationName(GetVmaAllocator(m_Context), m_Native->Allocation, info.Name.c_str());

        DebugMarkers::MarkBuffer(GetVkDevice(m_Context), m_Native->Buffer, m_Name);
    }

    Buffer::~Buffer()
    {
        // A released buffer (ReleaseBuffer) has nulled handles and is owned
        // elsewhere — its destructor must not retire anything.
        if (!m_Native->Buffer)
        {
            return;
        }

        m_Context.GetNative().Retire(m_Native->Buffer, m_Native->Allocation);
    }

    void Buffer::UploadSync(const std::span<const u8> data, const u64 offset) const
    {
        VE_ASSERT(offset + data.size() <= m_Size,
                  "Buffer '{}' upload out of range: offset {} + size {} > buffer size {}",
                  m_Name, offset, data.size(), m_Size);

        VK_RAW_ASSERT(
            vmaCopyMemoryToAllocation(GetVmaAllocator(m_Context), data.data(), m_Native->Allocation, offset, data.size()),
            "failed to upload buffer data!");
    }

    vector<u8> Buffer::Download() const
    {
        vector<u8> data(m_Size);

        VK_RAW_ASSERT(
            vmaCopyAllocationToMemory(GetVmaAllocator(m_Context), m_Native->Allocation, 0, data.data(), m_Size),
            "failed to download buffer data!");

        return data;
    }
}
