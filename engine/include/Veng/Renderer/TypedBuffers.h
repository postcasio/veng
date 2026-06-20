#pragma once

#include <span>
#include <type_traits>

#include <Veng/Veng.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/Types.h>

/// @brief Typed wrappers over Buffer that fix usage flags and element type at compile time.
///
/// Eliminates restating BufferUsage and computing byte sizes at call sites. Each wrapper
/// is a thin value type holding a `Ref<Buffer>` (composition); raw Buffer::Create remains
/// for staging and exotic cases.
///
/// Alignment note for `UniformBuffer<T>`/`StorageBuffer<T>`: T's C++ layout must match the
/// shader's std140 (UBO) / std430 (SSBO) expectations — veng does not translate layouts.
/// Use alignas and padding to match the shader (e.g. vec3 padded to 16 bytes in std140).
namespace Veng::Renderer
{
    /// @brief A typed GPU vertex buffer holding vertexCount elements of type V.
    ///
    /// @tparam V  Vertex element type; must be trivially copyable.
    template <typename V>
    class VertexBuffer
    {
        static_assert(std::is_trivially_copyable_v<V>, "vertex type must be trivially copyable");

    public:
        /// @brief Constructs an empty (null) vertex buffer.
        VertexBuffer() = default;

        /// @brief Allocates a vertex buffer for vertexCount elements.
        /// @param context      Context for buffer creation.
        /// @param name         Debug name.
        /// @param vertexCount  Number of vertex elements to allocate.
        static VertexBuffer Create(Context& context, string_view name, usize vertexCount)
        {
            return VertexBuffer(
                Buffer::Create(context,
                               {
                                   .Name = string(name),
                                   .Size = vertexCount * sizeof(V),
                                   .Usage = BufferUsage::Vertex | BufferUsage::TransferDst,
                               }),
                vertexCount);
        }

        /// @brief Synchronously uploads vertices into the buffer.
        /// @param vertices     Source vertex data.
        /// @param firstVertex  Byte-offset destination expressed as a vertex index.
        void UploadSync(std::span<const V> vertices, usize firstVertex = 0) const
        {
            m_Buffer->UploadSync(
                {reinterpret_cast<const u8*>(vertices.data()), vertices.size_bytes()},
                firstVertex * sizeof(V));
        }

        /// @brief Returns the number of vertex elements the buffer was allocated for.
        [[nodiscard]] usize GetVertexCount() const { return m_Count; }
        /// @brief Returns the underlying Buffer.
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        VertexBuffer(Ref<Buffer> buffer, usize count) : m_Buffer(std::move(buffer)), m_Count(count)
        {
        }

        /// @brief Underlying GPU buffer.
        Ref<Buffer> m_Buffer;
        /// @brief Number of vertex elements.
        usize m_Count = 0;
    };

    /// @brief A GPU index buffer with a runtime-selected index width.
    ///
    /// Not a template: index width is a vocabulary choice (IndexType), not a C++ element type.
    class IndexBuffer
    {
    public:
        /// @brief Constructs an empty (null) index buffer.
        IndexBuffer() = default;

        /// @brief Allocates an index buffer for indexCount elements.
        /// @param context     Context for buffer creation.
        /// @param name        Debug name.
        /// @param indexCount  Number of index elements to allocate.
        /// @param type        Index element width (U16 or U32).
        static IndexBuffer Create(Context& context, string_view name, usize indexCount,
                                  IndexType type = IndexType::U32)
        {
            const u64 stride = type == IndexType::U16 ? sizeof(u16) : sizeof(u32);
            return IndexBuffer(
                Buffer::Create(context,
                               {
                                   .Name = string(name),
                                   .Size = indexCount * stride,
                                   .Usage = BufferUsage::Index | BufferUsage::TransferDst,
                               }),
                indexCount, type);
        }

        /// @brief Synchronously uploads u32 indices into a U32 index buffer.
        /// @pre IndexType must be U32 — asserted otherwise.
        void UploadSync(std::span<const u32> indices, usize firstIndex = 0) const
        {
            VE_ASSERT(m_Type == IndexType::U32,
                      "IndexBuffer '{}' is U16; cannot upload u32 indices", m_Buffer->GetName());
            m_Buffer->UploadSync(
                {reinterpret_cast<const u8*>(indices.data()), indices.size_bytes()},
                firstIndex * sizeof(u32));
        }

        /// @brief Synchronously uploads u16 indices into a U16 index buffer.
        /// @pre IndexType must be U16 — asserted otherwise.
        void UploadSync(std::span<const u16> indices, usize firstIndex = 0) const
        {
            VE_ASSERT(m_Type == IndexType::U16,
                      "IndexBuffer '{}' is U32; cannot upload u16 indices", m_Buffer->GetName());
            m_Buffer->UploadSync(
                {reinterpret_cast<const u8*>(indices.data()), indices.size_bytes()},
                firstIndex * sizeof(u16));
        }

        /// @brief Returns the number of index elements the buffer was allocated for.
        [[nodiscard]] usize GetIndexCount() const { return m_Count; }
        /// @brief Returns the index element width.
        [[nodiscard]] IndexType GetIndexType() const { return m_Type; }
        /// @brief Returns the underlying Buffer.
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        IndexBuffer(Ref<Buffer> buffer, usize count, IndexType type)
            : m_Buffer(std::move(buffer)), m_Count(count), m_Type(type)
        {
        }

        /// @brief Underlying GPU buffer.
        Ref<Buffer> m_Buffer;
        /// @brief Number of index elements.
        usize m_Count = 0;
        /// @brief Index element width.
        IndexType m_Type = IndexType::U32;
    };

    /// @brief A typed GPU uniform buffer holding a single instance of T.
    ///
    /// @tparam T  Uniform block type; must be trivially copyable. Its layout must match
    ///            the shader's std140 expectations.
    template <typename T>
    class UniformBuffer
    {
        static_assert(std::is_trivially_copyable_v<T>, "uniform type must be trivially copyable");

    public:
        /// @brief Constructs an empty (null) uniform buffer.
        UniformBuffer() = default;

        /// @brief Allocates a uniform buffer sized for one T.
        /// @param context  Context for buffer creation.
        /// @param name     Debug name.
        static UniformBuffer Create(Context& context, string_view name)
        {
            return UniformBuffer(Buffer::Create(
                context, {
                             .Name = string(name),
                             .Size = sizeof(T),
                             .Usage = BufferUsage::Uniform | BufferUsage::TransferDst,
                         }));
        }

        /// @brief Synchronously uploads value into the buffer.
        /// @param value  The uniform data to upload.
        void UploadSync(const T& value) const
        {
            m_Buffer->UploadSync({reinterpret_cast<const u8*>(&value), sizeof(T)});
        }

        /// @brief Returns the underlying Buffer.
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        explicit UniformBuffer(Ref<Buffer> buffer) : m_Buffer(std::move(buffer)) {}

        /// @brief Underlying GPU buffer.
        Ref<Buffer> m_Buffer;
    };

    /// @brief A typed GPU storage buffer holding elementCount elements of type T.
    ///
    /// @tparam T  Element type; must be trivially copyable. Its layout must match the
    ///            shader's std430 expectations.
    template <typename T>
    class StorageBuffer
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "storage element type must be trivially copyable");

    public:
        /// @brief Constructs an empty (null) storage buffer.
        StorageBuffer() = default;

        /// @brief Allocates a storage buffer for elementCount elements of type T.
        /// @param context       Context for buffer creation.
        /// @param name          Debug name.
        /// @param elementCount  Number of T elements to allocate.
        static StorageBuffer Create(Context& context, string_view name, usize elementCount)
        {
            return StorageBuffer(
                Buffer::Create(context,
                               {
                                   .Name = string(name),
                                   .Size = elementCount * sizeof(T),
                                   .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                               }),
                elementCount);
        }

        /// @brief Synchronously uploads elements into the buffer.
        /// @param elements      Source element data.
        /// @param firstElement  Byte-offset destination expressed as an element index.
        void UploadSync(std::span<const T> elements, usize firstElement = 0) const
        {
            m_Buffer->UploadSync(
                {reinterpret_cast<const u8*>(elements.data()), elements.size_bytes()},
                firstElement * sizeof(T));
        }

        /// @brief Returns the number of elements the buffer was allocated for.
        [[nodiscard]] usize GetElementCount() const { return m_Count; }
        /// @brief Returns the underlying Buffer.
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

    private:
        StorageBuffer(Ref<Buffer> buffer, usize count) : m_Buffer(std::move(buffer)), m_Count(count)
        {
        }

        /// @brief Underlying GPU buffer.
        Ref<Buffer> m_Buffer;
        /// @brief Number of elements.
        usize m_Count = 0;
    };

    // ---- CommandBuffer typed binds (declared in CommandBuffer.h) -------------

    /// @brief Binds a typed vertex buffer; forwards to the raw-buffer overload.
    template <typename V>
    void CommandBuffer::BindVertexBuffer(const VertexBuffer<V>& buffer)
    {
        BindVertexBuffer(buffer.GetBuffer());
    }

    /// @brief Binds a typed index buffer; forwards to the raw-buffer overload.
    inline void CommandBuffer::BindIndexBuffer(const IndexBuffer& buffer)
    {
        BindIndexBuffer(buffer.GetBuffer(), buffer.GetIndexType());
    }

    // ---- DescriptorSet typed-buffer writes (declared in DescriptorSet.h) -----

    /// @brief Writes a typed uniform buffer into a descriptor set binding.
    template <typename T>
    void DescriptorSet::Write(const u32 binding, const UniformBuffer<T>& buffer)
    {
        Write(binding, buffer.GetBuffer());
    }

    /// @brief Writes a typed storage buffer into a descriptor set binding.
    template <typename T>
    void DescriptorSet::Write(const u32 binding, const StorageBuffer<T>& buffer)
    {
        Write(binding, buffer.GetBuffer());
    }
}
