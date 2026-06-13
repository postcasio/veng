#pragma once

#include <span>
#include <type_traits>

#include <Veng/Veng.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/Types.h>

// Typed wrappers over Buffer that fix usage flags and element type at compile
// time, so call sites stop restating BufferUsage and computing byte sizes by
// hand. Each wrapper is a thin value type holding a Ref<Buffer> (composition);
// raw Buffer::Create remains for staging and exotic cases.
//
// Alignment note for UniformBuffer<T>/StorageBuffer<T>: T's C++ layout must
// match the shader's std140 (UBO) / std430 (SSBO) expectations — veng does not
// translate layouts. Use alignas and padding to match the shader (vec3 padded
// to 16 bytes in std140, etc.); plan 12's reflection can validate this against
// SPIR-V later.
namespace Veng::Renderer
{
    template <typename V>
    class VertexBuffer
    {
        static_assert(std::is_trivially_copyable_v<V>, "vertex type must be trivially copyable");

    public:
        VertexBuffer() = default;

        static VertexBuffer Create(Context& context, string_view name, usize vertexCount)
        {
            return VertexBuffer(Buffer::Create(context, {
                .Name = string(name),
                .Size = vertexCount * sizeof(V),
                .Usage = BufferUsage::Vertex | BufferUsage::TransferDst,
            }), vertexCount);
        }

        void Upload(std::span<const V> vertices, usize firstVertex = 0) const
        {
            m_Buffer->Upload({reinterpret_cast<const u8*>(vertices.data()), vertices.size_bytes()},
                             firstVertex * sizeof(V));
        }

        [[nodiscard]] usize GetVertexCount() const { return m_Count; }
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        VertexBuffer(Ref<Buffer> buffer, usize count) : m_Buffer(std::move(buffer)), m_Count(count) {}

        Ref<Buffer> m_Buffer;
        usize m_Count = 0;
    };

    // Not a template: index width is a vocabulary choice (IndexType), not a C++
    // element type.
    class IndexBuffer
    {
    public:
        IndexBuffer() = default;

        static IndexBuffer Create(Context& context, string_view name, usize indexCount, IndexType type = IndexType::U32)
        {
            const u64 stride = type == IndexType::U16 ? sizeof(u16) : sizeof(u32);
            return IndexBuffer(Buffer::Create(context, {
                .Name = string(name),
                .Size = indexCount * stride,
                .Usage = BufferUsage::Index | BufferUsage::TransferDst,
            }), indexCount, type);
        }

        void Upload(std::span<const u32> indices, usize firstIndex = 0) const
        {
            VE_ASSERT(m_Type == IndexType::U32,
                      "IndexBuffer '{}' is U16; cannot upload u32 indices", m_Buffer->GetName());
            m_Buffer->Upload({reinterpret_cast<const u8*>(indices.data()), indices.size_bytes()},
                             firstIndex * sizeof(u32));
        }

        void Upload(std::span<const u16> indices, usize firstIndex = 0) const
        {
            VE_ASSERT(m_Type == IndexType::U16,
                      "IndexBuffer '{}' is U32; cannot upload u16 indices", m_Buffer->GetName());
            m_Buffer->Upload({reinterpret_cast<const u8*>(indices.data()), indices.size_bytes()},
                             firstIndex * sizeof(u16));
        }

        [[nodiscard]] usize GetIndexCount() const { return m_Count; }
        [[nodiscard]] IndexType GetIndexType() const { return m_Type; }
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        IndexBuffer(Ref<Buffer> buffer, usize count, IndexType type) :
            m_Buffer(std::move(buffer)), m_Count(count), m_Type(type) {}

        Ref<Buffer> m_Buffer;
        usize m_Count = 0;
        IndexType m_Type = IndexType::U32;
    };

    template <typename T>
    class UniformBuffer
    {
        static_assert(std::is_trivially_copyable_v<T>, "uniform type must be trivially copyable");

    public:
        UniformBuffer() = default;

        static UniformBuffer Create(Context& context, string_view name)
        {
            return UniformBuffer(Buffer::Create(context, {
                .Name = string(name),
                .Size = sizeof(T),
                .Usage = BufferUsage::Uniform | BufferUsage::TransferDst,
            }));
        }

        void Upload(const T& value) const
        {
            m_Buffer->Upload({reinterpret_cast<const u8*>(&value), sizeof(T)});
        }

        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        explicit UniformBuffer(Ref<Buffer> buffer) : m_Buffer(std::move(buffer)) {}

        Ref<Buffer> m_Buffer;
    };

    template <typename T>
    class StorageBuffer
    {
        static_assert(std::is_trivially_copyable_v<T>, "storage element type must be trivially copyable");

    public:
        StorageBuffer() = default;

        static StorageBuffer Create(Context& context, string_view name, usize elementCount)
        {
            return StorageBuffer(Buffer::Create(context, {
                .Name = string(name),
                .Size = elementCount * sizeof(T),
                .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
            }), elementCount);
        }

        void Upload(std::span<const T> elements, usize firstElement = 0) const
        {
            m_Buffer->Upload({reinterpret_cast<const u8*>(elements.data()), elements.size_bytes()},
                             firstElement * sizeof(T));
        }

        [[nodiscard]] usize GetElementCount() const { return m_Count; }
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        StorageBuffer(Ref<Buffer> buffer, usize count) : m_Buffer(std::move(buffer)), m_Count(count) {}

        Ref<Buffer> m_Buffer;
        usize m_Count = 0;
    };

    // ---- CommandBuffer typed binds (declared in CommandBuffer.h) -------------

    template <typename V>
    void CommandBuffer::BindVertexBuffer(const VertexBuffer<V>& buffer)
    {
        BindVertexBuffer(buffer.GetBuffer());
    }

    inline void CommandBuffer::BindIndexBuffer(const IndexBuffer& buffer)
    {
        BindIndexBuffer(buffer.GetBuffer(), buffer.GetIndexType());
    }

    // ---- DescriptorSet typed-buffer writes (declared in DescriptorSet.h) -----

    template <typename T>
    void DescriptorSet::Write(const u32 binding, const UniformBuffer<T>& buffer)
    {
        Write(binding, buffer.GetBuffer());
    }

    template <typename T>
    void DescriptorSet::Write(const u32 binding, const StorageBuffer<T>& buffer)
    {
        Write(binding, buffer.GetBuffer());
    }
}
