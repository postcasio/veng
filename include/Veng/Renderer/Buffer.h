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
            return CreateRef<Buffer>(info);
        }

        explicit Buffer(const BufferInfo& info);
        ~Buffer();

        void Upload(std::span<const u8> data) const;
        [[nodiscard]] vector<u8> Download() const;

        [[nodiscard]] const string& GetName() const { return m_Name; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;

        Unique<Native> m_Native;
        u64 m_Size;
    };
}
