#include <Veng/Renderer/Buffer.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng::Renderer
{
    Buffer::Native& Buffer::GetNative() const
    {
        return *m_Native;
    }

    Buffer::Buffer(Context& context, const BufferInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()), m_Size(info.Size)
    {
        VE_ASSERT(!(info.HostMapped && info.DeviceLocal),
                  "Buffer '{}': HostMapped and DeviceLocal are mutually exclusive", m_Name);

        // A device-local buffer's host transfers are staged copies, so it is always a copy's
        // source and destination.
        const BufferUsage usage =
            info.DeviceLocal ? info.Usage | BufferUsage::TransferSrc | BufferUsage::TransferDst
                             : info.Usage;
        const VkBufferCreateInfo bufferCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = m_Size,
            .usage = static_cast<VkBufferUsageFlags>(ToVk(usage)),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};

        VkBuffer buffer;

        // A host-mapped buffer is pinned in host-visible memory and mapped once
        // at creation, so its mapping is a stable pointer for per-frame writes.
        // Allowing a transfer-instead placement would let VMA put it in
        // device-local memory and defeat the persistent map, so that flag is
        // dropped on this path.
        const VmaAllocationCreateFlags allocationFlags =
            info.HostMapped ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT
                            : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;

        // A device-local buffer asks for no host access, which is what lets VMA place it in
        // memory the host cannot see.
        const VmaAllocationCreateInfo allocationCreateInfo =
            info.DeviceLocal
                ? VmaAllocationCreateInfo{.flags = 0,
                                          .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                          .requiredFlags = 0,
                                          .pool = VK_NULL_HANDLE}
                : VmaAllocationCreateInfo{.flags = allocationFlags,
                                          .usage = VMA_MEMORY_USAGE_AUTO,
                                          .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                          .pool = VK_NULL_HANDLE};
        m_Native->HostAccess = !info.DeviceLocal;

        VmaAllocationInfo allocationInfo{};
        VK_RAW_ASSERT(vmaCreateBuffer(GetVmaAllocator(m_Context), &bufferCreateInfo,
                                      &allocationCreateInfo, &buffer, &m_Native->Allocation,
                                      &allocationInfo),
                      "failed to create buffer!");

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
                  "Buffer '{}' upload out of range: offset {} + size {} > buffer size {}", m_Name,
                  offset, data.size(), m_Size);

        if (m_Native->HostAccess)
        {
            VK_RAW_ASSERT(vmaCopyMemoryToAllocation(GetVmaAllocator(m_Context), data.data(),
                                                    m_Native->Allocation, offset, data.size()),
                          "failed to upload buffer data!");
            return;
        }
        if (data.empty())
        {
            return;
        }

        const Ref<Buffer> staging = Create(m_Context, {
                                                          .Name = m_Name + " (Upload)",
                                                          .Size = data.size(),
                                                          .Usage = BufferUsage::TransferSrc,
                                                      });
        staging->UploadSync(data);
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                const vk::CommandBuffer vkCmd = GetVkCommandBuffer(cmd);
                vkCmd.copyBuffer(
                    staging->GetNative().Buffer, m_Native->Buffer,
                    vk::BufferCopy{.srcOffset = 0, .dstOffset = offset, .size = data.size()});
                // The copy's write is made visible to every later reader, whatever stage it reads
                // from.
                const vk::MemoryBarrier visible{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                                .dstAccessMask = vk::AccessFlagBits::eMemoryRead};
                vkCmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                      vk::PipelineStageFlagBits::eAllCommands, {}, visible, {}, {});
            });
    }

    Task<void> Buffer::Upload(TaskSystem& tasks, const std::span<const u8> data, const u64 offset)
    {
        // Copy the bytes since the span may not survive the call; the vector
        // overload is the no-copy path for a caller that owns them.
        return Upload(tasks, vector<u8>(data.begin(), data.end()), offset);
    }

    Task<void> Buffer::Upload(TaskSystem& tasks, vector<u8>&& data, const u64 offset)
    {
        VE_ASSERT(m_Native->HostAccess,
                  "Buffer '{}': a DeviceLocal buffer uploads through UploadSync, not on a worker",
                  m_Name);

        // HOST_VISIBLE | HOST_COHERENT: upload is a plain memcpy — no staging,
        // no GPU command, no timeline gate. Capture an owning Ref so the buffer
        // survives until the job runs.
        Ref<Buffer> self = shared_from_this();

        return tasks.Submit([self = std::move(self), bytes = std::move(data), offset]
                            { self->UploadSync(bytes, offset); }, m_Name);
    }

    void* Buffer::GetMappedData() const
    {
        VE_ASSERT(m_Native->MappedData != nullptr,
                  "Buffer '{}': GetMappedData on a buffer not created with HostMapped", m_Name);
        return m_Native->MappedData;
    }

    vector<u8> Buffer::Download() const
    {
        if (!m_Native->HostAccess)
        {
            const Ref<Buffer> staging = Create(m_Context, {
                                                              .Name = m_Name + " (Download)",
                                                              .Size = m_Size,
                                                              .Usage = BufferUsage::TransferDst,
                                                          });
            m_Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    const vk::CommandBuffer vkCmd = GetVkCommandBuffer(cmd);
                    // Ordered after every earlier write to the buffer, from any stage.
                    const vk::MemoryBarrier written{
                        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferRead};
                    vkCmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                          vk::PipelineStageFlagBits::eTransfer, {}, written, {},
                                          {});
                    vkCmd.copyBuffer(
                        m_Native->Buffer, staging->GetNative().Buffer,
                        vk::BufferCopy{.srcOffset = 0, .dstOffset = 0, .size = m_Size});
                });
            return staging->Download();
        }

        vector<u8> data(m_Size);

        VK_RAW_ASSERT(vmaCopyAllocationToMemory(GetVmaAllocator(m_Context), m_Native->Allocation, 0,
                                                data.data(), m_Size),
                      "failed to download buffer data!");

        return data;
    }
}
