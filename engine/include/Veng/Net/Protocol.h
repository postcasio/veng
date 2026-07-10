#pragma once

#include <Veng/Net/Connection.h>
#include <Veng/Veng.h>

#include <bit>
#include <span>

// Net/Protocol.h — the wire vocabulary shared by the connection implementation.
//
// Internal (engine/src) header: sequence arithmetic, the sliding-window ack
// state, and the field-by-field packet-header codec. Kept out of the public
// surface but factored here so the sequencing and ack-bitfield logic is unit
// testable in isolation. Every target platform is little-endian; the codec asserts
// it and packs each field explicitly, never memcpy-ing a padded struct.

static_assert(std::endian::native == std::endian::little,
              "veng's wire format is little-endian; a big-endian target must byte-swap");

namespace Veng::Net
{
    /// @brief Circular u16 sequence comparison: is `a` newer than `b`?
    ///
    /// Treats the 16-bit sequence space as a circle, so it stays correct across the
    /// 65535 -> 0 wraparound: `a` is newer when it leads `b` by less than half the
    /// space. Undefined intent for exactly-opposite sequences (never outstanding).
    [[nodiscard]] inline bool SequenceGreaterThan(u16 a, u16 b)
    {
        return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
    }

    /// @brief Sliding-window record of which recent peer sequences were received.
    ///
    /// Tracks the highest received sequence and a 32-bit bitfield of the 32
    /// sequences before it (bit i == RemoteSequence - (i + 1)). This is exactly the
    /// ack + ack-bitfield a sent packet header carries back to the peer.
    struct AckState
    {
        /// @brief Highest received sequence so far.
        u16 RemoteSequence = 0;
        /// @brief Bitfield of the 32 sequences preceding RemoteSequence.
        u32 AckBits = 0;
        /// @brief False until the first sequence is recorded.
        bool HasRemote = false;

        /// @brief Folds a received sequence into the window.
        /// @param seq  The received packet sequence.
        void Receive(u16 seq)
        {
            if (!HasRemote)
            {
                HasRemote = true;
                RemoteSequence = seq;
                AckBits = 0;
                return;
            }
            if (SequenceGreaterThan(seq, RemoteSequence))
            {
                const u16 shift = static_cast<u16>(seq - RemoteSequence);
                if (shift >= 32)
                {
                    // The old window falls entirely out of range; only seq remains.
                    AckBits = 0;
                }
                else
                {
                    AckBits = (AckBits << shift) | (1u << (shift - 1));
                }
                RemoteSequence = seq;
            }
            else if (seq != RemoteSequence)
            {
                const u16 diff = static_cast<u16>(RemoteSequence - seq);
                if (diff >= 1 && diff <= 32)
                {
                    AckBits |= (1u << (diff - 1));
                }
            }
        }

        /// @brief True if `seq` sits inside the window and is marked received.
        /// @param seq  The sequence to test.
        [[nodiscard]] bool IsAcked(u16 seq) const
        {
            if (!HasRemote)
            {
                return false;
            }
            if (seq == RemoteSequence)
            {
                return true;
            }
            const u16 diff = static_cast<u16>(RemoteSequence - seq);
            if (diff >= 1 && diff <= 32)
            {
                return (AckBits & (1u << (diff - 1))) != 0;
            }
            return false;
        }
    };

    /// @brief The fixed-size header prefixing every datagram.
    struct PacketHeader
    {
        /// @brief Protocol magic; a mismatch rejects the datagram.
        u32 Magic = ProtocolMagic;
        /// @brief Channel this datagram belongs to (a Channel value).
        u8 Channel = 0;
        /// @brief This datagram's sequence on its channel.
        u16 Sequence = 0;
        /// @brief Highest sequence received from the peer on this channel.
        u16 Ack = 0;
        /// @brief Ack bitfield for the 32 sequences before Ack.
        u32 AckBits = 0;
    };

    /// @brief Appends a little-endian u16 to a byte buffer.
    inline void WriteU16LE(vector<u8>& out, u16 value)
    {
        out.push_back(static_cast<u8>(value & 0xFFu));
        out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    }

    /// @brief Appends a little-endian u32 to a byte buffer.
    inline void WriteU32LE(vector<u8>& out, u32 value)
    {
        out.push_back(static_cast<u8>(value & 0xFFu));
        out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
        out.push_back(static_cast<u8>((value >> 16) & 0xFFu));
        out.push_back(static_cast<u8>((value >> 24) & 0xFFu));
    }

    /// @brief Appends a little-endian u64 to a byte buffer.
    inline void WriteU64LE(vector<u8>& out, u64 value)
    {
        for (u32 i = 0; i < 8; ++i)
        {
            out.push_back(static_cast<u8>((value >> (i * 8)) & 0xFFu));
        }
    }

    /// @brief Reads a little-endian u16 from a byte view at an offset.
    [[nodiscard]] inline u16 ReadU16LE(std::span<const u8> bytes, usize offset)
    {
        return static_cast<u16>(static_cast<u16>(bytes[offset]) |
                                (static_cast<u16>(bytes[offset + 1]) << 8));
    }

    /// @brief Reads a little-endian u32 from a byte view at an offset.
    [[nodiscard]] inline u32 ReadU32LE(std::span<const u8> bytes, usize offset)
    {
        return static_cast<u32>(bytes[offset]) | (static_cast<u32>(bytes[offset + 1]) << 8) |
               (static_cast<u32>(bytes[offset + 2]) << 16) |
               (static_cast<u32>(bytes[offset + 3]) << 24);
    }

    /// @brief Reads a little-endian u64 from a byte view at an offset.
    [[nodiscard]] inline u64 ReadU64LE(std::span<const u8> bytes, usize offset)
    {
        u64 value = 0;
        for (u32 i = 0; i < 8; ++i)
        {
            value |= static_cast<u64>(bytes[offset + i]) << (i * 8);
        }
        return value;
    }

    /// @brief Serializes a packet header field-by-field to a byte buffer.
    /// @param out     Destination buffer; the header is appended.
    /// @param header  The header to write.
    inline void WritePacketHeader(vector<u8>& out, const PacketHeader& header)
    {
        WriteU32LE(out, header.Magic);
        out.push_back(header.Channel);
        WriteU16LE(out, header.Sequence);
        WriteU16LE(out, header.Ack);
        WriteU32LE(out, header.AckBits);
    }

    /// @brief Parses a packet header from a datagram.
    /// @param bytes  The full datagram bytes.
    /// @return The parsed header, or nullopt if the datagram is too short to hold one.
    [[nodiscard]] inline optional<PacketHeader> ReadPacketHeader(std::span<const u8> bytes)
    {
        if (bytes.size() < PacketHeaderSize)
        {
            return {};
        }
        return PacketHeader{
            .Magic = ReadU32LE(bytes, 0),
            .Channel = bytes[4],
            .Sequence = ReadU16LE(bytes, 5),
            .Ack = ReadU16LE(bytes, 7),
            .AckBits = ReadU32LE(bytes, 9),
        };
    }
}
