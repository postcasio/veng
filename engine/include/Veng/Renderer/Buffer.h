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

    /// @brief Creation parameters for a GPU buffer.
    struct BufferInfo
    {
        /// @brief Debug name used in Vulkan object labels and error messages.
        string Name;
        /// @brief Byte size of the buffer.
        u64 Size;
        /// @brief How the buffer will be bound and accessed.
        BufferUsage Usage;

        /// @brief When true, the allocation is pinned in HOST_VISIBLE | HOST_COHERENT memory
        /// and mapped once at creation, so GetMappedData() returns a stable pointer for
        /// direct writes with no per-write map/unmap and no flush. Used for data rewritten
        /// every frame (the ring-buffered material param store); the default path lets VMA
        /// place the buffer in device-local memory and stage transfers as needed.
        bool HostMapped = false;
    };

    /// @brief A GPU buffer with deferred destruction.
    ///
    /// Constructed only through Create(). enable_shared_from_this lets the async Upload
    /// capture an owning Ref<Buffer> into the worker job so the buffer cannot be destroyed
    /// before the job runs.
    class Buffer : public std::enable_shared_from_this<Buffer>
    {
    public:
        /// @brief Creates a buffer from the given parameters.
        /// @param context The owning context; the buffer must not outlive it.
        /// @param info    Creation parameters.
        /// @return A shared reference to the new buffer.
        static Ref<Buffer> Create(Context& context, const BufferInfo& info)
        {
            return Ref<Buffer>(new Buffer(context, info));
        }

        /// @brief Defers destruction of the underlying Vulkan buffer until the GPU is done with it.
        ~Buffer();

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        /// @brief Copies data into the buffer at a byte offset, blocking the caller.
        ///
        /// Performs a host-visible memcpy; the device is never waited. offset + size
        /// must fit within the buffer.
        /// @param data   Bytes to write.
        /// @param offset Byte offset into the buffer (default 0).
        void UploadSync(std::span<const u8> data, u64 offset = 0) const;

        /// @brief Copies data into the buffer on a worker thread, returning immediately.
        ///
        /// A Buffer is always HOST_VISIBLE | HOST_COHERENT, so the upload is a plain memcpy
        /// with no staging, no GPU command, and no device wait — the job runs UploadSync
        /// off the main thread. The buffer is kept alive for the job's duration via
        /// shared_from_this().
        /// @param tasks  The task system to dispatch the upload job on.
        /// @param data   Bytes to write.
        /// @param offset Byte offset into the buffer (default 0).
        /// @return A Task that completes once the memcpy has run.
        [[nodiscard]] Task<void> Upload(TaskSystem& tasks, std::span<const u8> data,
                                        u64 offset = 0);

        /// @brief Downloads the full buffer contents to the host, blocking until complete.
        /// @return A byte vector containing a snapshot of the buffer.
        [[nodiscard]] vector<u8> Download() const;

        /// @brief Returns the persistent host mapping of a buffer created with BufferInfo::HostMapped.
        ///
        /// Writes through this pointer are visible to the device without a flush (HOST_COHERENT).
        /// @pre The buffer was created with BufferInfo::HostMapped == true.
        /// @return A pointer to the mapped memory region.
        [[nodiscard]] void* GetMappedData() const;

        /// @brief Returns the buffer's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the buffer's byte size.
        [[nodiscard]] u64 GetSize() const { return m_Size; }

        /// @brief Opaque backend handle; defined in Buffer.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        Buffer(Context& context, const BufferInfo& info);

        /// @brief The owning context; used for deferred destruction and host transfers.
        Context& m_Context;
        string m_Name;

        Unique<Native> m_Native;
        u64 m_Size;
    };
}
