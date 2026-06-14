#pragma once

#include <memory>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class TaskSystem;

    template <typename T>
    class Task;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;

    struct BufferInfo
    {
        string Name;
        u64 Size;
        BufferUsage Usage;
    };

    // enable_shared_from_this so the async Upload can capture an owning Ref<Buffer>
    // into the worker job — the buffer must not be destroyed before the job runs.
    class Buffer : public std::enable_shared_from_this<Buffer>
    {
    public:
        static Ref<Buffer> Create(Context& context, const BufferInfo& info)
        {
            return Ref<Buffer>(new Buffer(context, info));
        }

        ~Buffer();

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        // Copy data into the buffer at byte offset (default 0). offset + size
        // must fit within the buffer. Blocks the caller (a host-visible memcpy);
        // the device is never waited.
        void UploadSync(std::span<const u8> data, u64 offset = 0) const;

        // Copy data into the buffer on a worker thread, returning immediately.
        // A Buffer is always HOST_VISIBLE | HOST_COHERENT, so the upload is a
        // plain memcpy with no staging, no GPU command, and no device wait — the
        // job runs UploadSync off the main thread. The buffer is held alive for
        // the job's duration via shared_from_this().
        [[nodiscard]] Task<void> Upload(TaskSystem& tasks, std::span<const u8> data, u64 offset = 0);

        [[nodiscard]] vector<u8> Download() const;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] u64 GetSize() const { return m_Size; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Buffer(Context& context, const BufferInfo& info);

        // The context this resource was created with — used for deferred
        // destruction (Retire) and host transfers. A resource must not outlive
        // its context (see docs/ownership.md).
        Context& m_Context;
        string m_Name;

        Unique<Native> m_Native;
        u64 m_Size;
    };
}
