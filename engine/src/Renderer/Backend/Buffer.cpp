#include <Veng/Renderer/Buffer.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Task/TaskSystem.h>

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

        // A host-mapped buffer is pinned in host-visible memory and mapped once
        // at creation, so its mapping is a stable pointer for per-frame writes.
        // Allowing a transfer-instead placement would let VMA put it in
        // device-local memory and defeat the persistent map, so that flag is
        // dropped on this path.
        const VmaAllocationCreateFlags allocationFlags = info.HostMapped
            ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT
            : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;

        const VmaAllocationCreateInfo allocationCreateInfo = {
            .flags = allocationFlags,
            .usage = VMA_MEMORY_USAGE_AUTO,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            .pool = VK_NULL_HANDLE
        };

        VmaAllocationInfo allocationInfo{};
        VK_RAW_ASSERT(
            vmaCreateBuffer(GetVmaAllocator(m_Context), &bufferCreateInfo, &allocationCreateInfo, &buffer, &
                m_Native->Allocation, &allocationInfo), "failed to create buffer!");

        m_Native->Buffer = buffer;
        if (info.HostMapped)
        {
            m_Native->MappedData = allocationInfo.pMappedData;
        }

        vmaSetAllocationName(GetVmaAllocator(m_Context), m_Native->Allocation, info.Name.c_str());

        DebugMarkers::MarkBuffer(GetVkDevice(m_Context), m_Native->Buffer, m_Name);
    }

    Buffer::~Buffer()
    {
        // A released buffer (via ReleaseBuffer) has a null handle — skip deferred destruction.
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

    Task<void> Buffer::Upload(TaskSystem& tasks, const std::span<const u8> data, const u64 offset)
    {
        // HOST_VISIBLE | HOST_COHERENT: upload is a plain memcpy — no staging,
        // no GPU command, no timeline gate. Capture an owning Ref so the buffer
        // survives until the job runs; copy the bytes since the span may not.
        Ref<Buffer> self = shared_from_this();
        vector<u8> bytes(data.begin(), data.end());

        return tasks.Submit([self = std::move(self), bytes = std::move(bytes), offset]
        {
            self->UploadSync(bytes, offset);
        });
    }

    void* Buffer::GetMappedData() const
    {
        VE_ASSERT(m_Native->MappedData != nullptr,
                  "Buffer '{}': GetMappedData on a buffer not created with HostMapped", m_Name);
        return m_Native->MappedData;
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
