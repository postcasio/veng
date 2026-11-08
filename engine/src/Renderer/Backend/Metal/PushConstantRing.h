#pragma once

// Push constants on Metal, device-free. Metal 4's argument table binds addresses, not bytes, so a
// command buffer keeps a CPU shadow of the bound layout's push block, and before a draw or dispatch
// that follows a change copies the block into a linear ring and binds that copy by address. The ring
// belongs to the command buffer and rewinds when the command buffer is begun again, which is after
// the work last recorded into it (a frame slot's, a worker's upload) has completed.

#include <array>
#include <cstring>
#include <span>

#include <Veng/Veng.h>

#include <Veng/Assert.h>

namespace Veng::Renderer::Metal
{
    /// @brief The largest push block a pipeline layout declares on Metal.
    inline constexpr u32 MaxPushConstantBytes = 256;

    /// @brief The alignment of each block copy in the ring, which a constant-buffer address needs.
    inline constexpr u32 PushConstantCopyAlignment = 256;

    /// @brief The size of one ring chunk; a command buffer takes more chunks as it fills them.
    inline constexpr u32 PushConstantChunkBytes = 64 * 1024;

    /// @brief The CPU shadow of a push block: partial writes land at their offsets, and the block
    ///        is copied whole when it is next bound.
    class PushConstantShadow
    {
    public:
        /// @brief Sets the size of the block the bound layout declares.
        ///
        /// The bytes already written stay, as push constants persist across a pipeline change;
        /// growing the block marks it dirty.
        /// @param size The block's size in bytes; at most MaxPushConstantBytes.
        void SetSize(const u32 size)
        {
            VE_ASSERT(size <= MaxPushConstantBytes,
                      "a {}-byte push block is past the {} the Metal backend carries", size,
                      MaxPushConstantBytes);
            if (size != m_Size)
            {
                m_Size = size;
                m_Dirty = m_Size > 0;
            }
        }

        /// @brief Writes @p bytes at @p offset and marks the block dirty.
        /// @param offset The byte offset within the block.
        /// @param bytes  The bytes to write; the range lies within the block.
        void Write(const u32 offset, const std::span<const std::byte> bytes)
        {
            VE_ASSERT(offset + bytes.size() <= m_Size,
                      "push constants [{}, {}) lie past the bound {}-byte block", offset,
                      offset + bytes.size(), m_Size);
            std::memcpy(m_Bytes.data() + offset, bytes.data(), bytes.size());
            m_Dirty = true;
        }

        /// @brief Marks the block to be copied before the next draw or dispatch.
        void MarkDirty() { m_Dirty = m_Size > 0; }

        /// @brief Clears the dirty mark once the block has been copied and bound.
        void MarkClean() { m_Dirty = false; }

        /// @brief Whether the block changed since it was last copied.
        [[nodiscard]] bool IsDirty() const { return m_Dirty; }

        /// @brief The block's size in bytes.
        [[nodiscard]] u32 GetSize() const { return m_Size; }

        /// @brief The block's bytes.
        [[nodiscard]] std::span<const std::byte> GetBytes() const
        {
            return {m_Bytes.data(), m_Size};
        }

    private:
        /// @brief The block's bytes.
        std::array<std::byte, MaxPushConstantBytes> m_Bytes{};
        /// @brief The bound layout's block size.
        u32 m_Size = 0;
        /// @brief Whether the block changed since it was last copied.
        bool m_Dirty = false;
    };

    /// @brief Where a block copy landed: the chunk and the byte offset within it.
    struct RingPlacement
    {
        /// @brief The index of the chunk among the ring's chunks.
        u32 Chunk = 0;
        /// @brief The byte offset within the chunk.
        u32 Offset = 0;
    };

    /// @brief A linear allocator over a growing list of equal chunks, rewound as a whole.
    ///
    /// Allocate reports a chunk index past the chunks held when the caller must append one; the
    /// chunks themselves (device buffers) are the caller's.
    class LinearRing
    {
    public:
        /// @brief Places @p size bytes at the next aligned offset, in a later chunk when this one is
        ///        full.
        /// @param size The bytes to place; at most PushConstantChunkBytes.
        /// @return The placement.
        [[nodiscard]] RingPlacement Allocate(const u32 size)
        {
            VE_ASSERT(size > 0 && size <= PushConstantChunkBytes,
                      "a {}-byte ring placement is past the {}-byte chunk", size,
                      PushConstantChunkBytes);
            if (m_Cursor + size > PushConstantChunkBytes)
            {
                ++m_Chunk;
                m_Cursor = 0;
            }
            const RingPlacement placement{.Chunk = m_Chunk, .Offset = m_Cursor};
            m_Cursor = (m_Cursor + size + PushConstantCopyAlignment - 1) /
                       PushConstantCopyAlignment * PushConstantCopyAlignment;
            m_ChunksUsed = std::max(m_ChunksUsed, m_Chunk + 1);
            return placement;
        }

        /// @brief Rewinds to the first chunk's start, keeping every chunk for reuse.
        void Rewind()
        {
            m_Chunk = 0;
            m_Cursor = 0;
        }

        /// @brief The number of chunks any allocation since construction has reached.
        [[nodiscard]] u32 GetChunksUsed() const { return m_ChunksUsed; }

    private:
        /// @brief The chunk the next allocation tries.
        u32 m_Chunk = 0;
        /// @brief The next free byte in that chunk.
        u32 m_Cursor = 0;
        /// @brief The most chunks reached.
        u32 m_ChunksUsed = 0;
    };
}
