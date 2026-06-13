#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class CommandBuffer;

    struct BufferInfo
    {
        string Name;
        u64 Size;
        BufferUsage Usage;
    };

    class Buffer
    {
    public:
        static Ref<Buffer> Create(const BufferInfo& info)
        {
            return Ref<Buffer>(new Buffer(info));
        }

        ~Buffer();

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        // Copy data into the buffer at byte offset (default 0). offset + size
        // must fit within the buffer.
        void Upload(std::span<const u8> data, u64 offset = 0) const;
        [[nodiscard]] vector<u8> Download() const;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] u64 GetSize() const { return m_Size; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit Buffer(const BufferInfo& info);

        string m_Name;

        Unique<Native> m_Native;
        u64 m_Size;
    };
}
