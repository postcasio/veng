#pragma once

#include <Veng/Veng.h>

#include <span>

// Veng/Net/BitStream.h — the one shared sub-byte bit packer for the compressed net wire.
//
// The quantized spatial leaves and the packed input encoding both write fields that cost fewer
// than eight bits (a 9-bit rotation component, a 2-bit phase, a presence flag), so they share one
// writer/reader rather than each hand-rolling bit twiddling. Bits are packed most-significant-first
// within the stream; the reader mirrors the writer exactly, so a value written with WriteBits(v, n)
// reads back identically with ReadBits(n). Byte order of the backing bytes is irrelevant — the
// stream is a bit sequence, decoded bit for bit, not a reinterpreted integer.

namespace Veng::Net
{
    /// @brief Packs values sub-byte into a growable byte buffer, most-significant-bit first.
    ///
    /// WriteBits appends the low @p count bits of a value; the buffer grows a byte at a time as the
    /// bit cursor crosses a byte boundary. Finish pads the final partial byte with zero bits. The
    /// same-build, host-agnostic seam the rest of the net layer assumes holds here: the output is a
    /// bit sequence, not a struct image, so it carries no endianness.
    class BitWriter
    {
    public:
        /// @brief Appends the low @p count bits of @p value, most-significant of those bits first.
        /// @param value  The source value; only its low @p count bits are written.
        /// @param count  Number of bits to write (0..32); zero is a no-op.
        void WriteBits(u32 value, u32 count)
        {
            for (u32 i = 0; i < count; ++i)
            {
                const u32 bit = (value >> (count - 1 - i)) & 1u;
                if (m_BitInByte == 0)
                {
                    m_Bytes.push_back(0);
                }
                m_Bytes.back() |= static_cast<u8>(bit << (7 - m_BitInByte));
                m_BitInByte = (m_BitInByte + 1) & 7;
            }
        }

        /// @brief Appends a single bit.
        /// @param value  The bit to write.
        void WriteBit(bool value) { WriteBits(value ? 1u : 0u, 1); }

        /// @brief Returns the packed bytes, padding the final partial byte with zero bits.
        [[nodiscard]] const vector<u8>& Bytes() const { return m_Bytes; }

        /// @brief Moves the packed bytes out (padding already applied by construction).
        [[nodiscard]] vector<u8> Take() { return std::move(m_Bytes); }

        /// @brief The number of bits written so far.
        [[nodiscard]] usize BitCount() const
        {
            return m_Bytes.size() * 8 - (m_BitInByte == 0 ? 0 : 8 - m_BitInByte);
        }

    private:
        /// @brief The packed output; the last byte is partially filled while m_BitInByte != 0.
        vector<u8> m_Bytes;
        /// @brief Next bit position within the last byte (0..7); 0 means a fresh byte is needed.
        u32 m_BitInByte = 0;
    };

    /// @brief Reads values sub-byte from a byte span, mirroring BitWriter exactly.
    ///
    /// ReadBits pulls @p count bits most-significant first, matching WriteBits. Reading past the end
    /// yields zero bits (the padding a truncated packet presents), never a fault — the net layer's
    /// recoverable-on-malformed posture, so a hostile short packet drops rather than asserts.
    class BitReader
    {
    public:
        /// @brief Constructs a reader over @p bytes.
        /// @param bytes  The packed bit stream.
        explicit BitReader(std::span<const u8> bytes) : m_Bytes(bytes) {}

        /// @brief Reads @p count bits, most-significant first; bits past the end read as zero.
        /// @param count  Number of bits to read (0..32).
        /// @return The read value in its low @p count bits.
        [[nodiscard]] u32 ReadBits(u32 count)
        {
            u32 value = 0;
            for (u32 i = 0; i < count; ++i)
            {
                u32 bit = 0;
                const usize byteIndex = m_BitPos / 8;
                if (byteIndex < m_Bytes.size())
                {
                    const u32 bitInByte = static_cast<u32>(m_BitPos & 7);
                    bit = (m_Bytes[byteIndex] >> (7 - bitInByte)) & 1u;
                }
                value = (value << 1) | bit;
                ++m_BitPos;
            }
            return value;
        }

        /// @brief Reads a single bit (zero past the end).
        [[nodiscard]] bool ReadBit() { return ReadBits(1) != 0; }

        /// @brief The number of bits consumed so far.
        [[nodiscard]] usize BitPos() const { return m_BitPos; }

        /// @brief True once the reader has consumed at least as many bits as the span holds.
        [[nodiscard]] bool AtEnd() const { return m_BitPos >= m_Bytes.size() * 8; }

    private:
        /// @brief The packed input bits.
        std::span<const u8> m_Bytes;
        /// @brief The bit cursor, in bits from the stream start.
        usize m_BitPos = 0;
    };
}
