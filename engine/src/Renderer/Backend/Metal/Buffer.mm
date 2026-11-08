#include <Veng/Renderer/Buffer.h>

#include <cstring>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

#include "Natives.h"
#include "Recording.h"

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

        Context::Native& native = m_Context.GetNative();
        // Shared storage is host-visible and coherent, which is what every buffer but a
        // device-local one promises the host.
        const bool shared = !info.DeviceLocal;
        const MTLResourceOptions options =
            (shared ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate) |
            MTLResourceHazardTrackingModeUntracked;
        // A zero-sized buffer has nothing to place but still needs an address to bind.
        const u64 length = std::max<u64>(m_Size, 4);
        const MTLSizeAndAlign sizeAndAlign =
            [native.Device heapBufferSizeAndAlignWithLength:length options:options];
        m_Native->Allocation = native.Allocate(shared, sizeAndAlign.size, sizeAndAlign.align);
        m_Native->Buffer =
            [m_Native->Allocation.Heap newBufferWithLength:length
                                                   options:options
                                                    offset:m_Native->Allocation.Offset];
        VE_ASSERT(m_Native->Buffer != nil, "Buffer '{}': failed to place {} bytes", m_Name, length);
        [m_Native->Buffer setLabel:Metal::ToNSString(m_Name)];

        m_Native->HostAccess = shared;
        m_Native->HostMapped = info.HostMapped;
        if (shared)
        {
            m_Native->Contents = [m_Native->Buffer contents];
        }
    }

    Buffer::~Buffer()
    {
        if (m_Native->Buffer == nil)
        {
            return;
        }
        Context::Native& native = m_Context.GetNative();
        native.Retire(m_Native->Buffer);
        native.Retire(m_Native->Allocation);
        m_Native->Buffer = nil;
    }

    void Buffer::UploadSync(const std::span<const u8> data, const u64 offset) const
    {
        VE_ASSERT(offset + data.size() <= m_Size,
                  "Buffer '{}' upload out of range: offset {} + size {} > buffer size {}", m_Name,
                  offset, data.size(), m_Size);
        if (data.empty())
        {
            return;
        }

        if (m_Native->HostAccess)
        {
            std::memcpy(static_cast<u8*>(m_Native->Contents) + offset, data.data(), data.size());
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
                Metal::Recording::CopyBuffer(cmd, *staging, 0, *this, offset, data.size());
                // The copy's write is made visible to every later reader, whatever stage it reads
                // from.
                Metal::Recording::Barrier(cmd, Metal::MetalStage::Blit, Metal::MetalStage::All);
            });
    }

    Task<void> Buffer::Upload(TaskSystem& tasks, const std::span<const u8> data, const u64 offset)
    {
        // Copied, since the span may not survive the call; the vector overload takes ownership.
        return Upload(tasks, vector<u8>(data.begin(), data.end()), offset);
    }

    Task<void> Buffer::Upload(TaskSystem& tasks, vector<u8>&& data, const u64 offset)
    {
        VE_ASSERT(m_Native->HostAccess,
                  "Buffer '{}': a DeviceLocal buffer uploads through UploadSync, not on a worker",
                  m_Name);

        // Shared storage is a plain memcpy; the owning Ref keeps the buffer alive until the job
        // runs.
        Ref<Buffer> self = shared_from_this();
        return tasks.Submit([self = std::move(self), bytes = std::move(data), offset]
                            { self->UploadSync(bytes, offset); }, m_Name);
    }

    void* Buffer::GetMappedData() const
    {
        VE_ASSERT(m_Native->HostMapped,
                  "Buffer '{}': GetMappedData on a buffer not created with HostMapped", m_Name);
        return m_Native->Contents;
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
                    // Ordered after every earlier write to the buffer, from any stage.
                    Metal::Recording::Barrier(cmd, Metal::MetalStage::All, Metal::MetalStage::Blit);
                    Metal::Recording::CopyBuffer(cmd, *this, 0, *staging, 0, m_Size);
                });
            return staging->Download();
        }

        vector<u8> data(m_Size);
        std::memcpy(data.data(), m_Native->Contents, m_Size);
        return data;
    }
}
