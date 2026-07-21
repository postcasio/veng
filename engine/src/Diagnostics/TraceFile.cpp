#include "TraceFile.h"

#include "TraceFormat.h"

#include <cstring>

namespace Veng::Diagnostics::TraceFileFormat
{
    void AppendVarint(vector<u8>& out, u64 value)
    {
        while (value >= 0x80)
        {
            out.push_back(static_cast<u8>(value) | 0x80);
            value >>= 7;
        }
        out.push_back(static_cast<u8>(value));
    }

    bool ReadVarint(std::span<const u8> data, usize& offset, u64& out)
    {
        u64 result = 0;
        u32 shift = 0;
        usize cursor = offset;
        // LEB128 stops at the first byte with the high bit clear; a u64 needs at most ten 7-bit
        // groups, so a stream that never clears the bit inside ten bytes is malformed.
        for (u32 i = 0; i < 10; ++i)
        {
            if (cursor >= data.size())
            {
                return false;
            }
            const u8 byte = data[cursor++];
            result |= static_cast<u64>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0)
            {
                offset = cursor;
                out = result;
                return true;
            }
            shift += 7;
        }
        return false;
    }

    i64 DecodeZigzag(u64 encoded)
    {
        return static_cast<i64>(encoded >> 1) ^ -static_cast<i64>(encoded & 1);
    }

    CounterValueTag SelectCounterTag(f64 value) noexcept
    {
        // "Exact" means bit-for-bit: the reconstructed f64 must equal the original *bits*, not merely
        // compare equal. That distinction is what routes −0.0 (which == +0.0) to RawF64, and it makes
        // NaN — never == anything — fall through cleanly.
        u64 originalBits;
        std::memcpy(&originalBits, &value, sizeof(originalBits));
        const auto roundTrips = [originalBits](f64 reconstructed)
        {
            u64 bits;
            std::memcpy(&bits, &reconstructed, sizeof(bits));
            return bits == originalBits;
        };

        // A non-negative integer exact as u64: the common counter (queue depth, draw calls) and the
        // narrowest form. The upper bound is strict so a value at or past 2^64 never reaches the cast.
        if (value >= 0.0 && value < 18446744073709551616.0)
        {
            if (roundTrips(static_cast<f64>(static_cast<u64>(value))))
            {
                return CounterValueTag::VarU64;
            }
        }
        // A signed integer exact as i64: a negative or otherwise non-u64 integer.
        if (value >= -9223372036854775808.0 && value < 9223372036854775808.0)
        {
            if (roundTrips(static_cast<f64>(static_cast<i64>(value))))
            {
                return CounterValueTag::ZigzagI64;
            }
        }
        // Anything else — fractional, out of integer range, NaN, ±inf, −0 — is kept bit-for-bit.
        return CounterValueTag::RawF64;
    }

    TraceWriter::TraceWriter(CaptureMode mode, BuildConfig config, bool profileEnabled,
                             u64 tickFrequency, u64 tickBase)
    {
        m_Bytes.insert(m_Bytes.end(), std::begin(Magic), std::end(Magic));
        PutU32(FormatVersion);
        PutU32(PreambleSize);
        PutU64(tickFrequency);
        PutU64(tickBase);
        PutU8(static_cast<u8>(mode));
        PutU8(static_cast<u8>(config));
        PutU8(profileEnabled ? 1 : 0);
        // Five reserved bytes pad the preamble to its fixed size; readers ignore them.
        for (u32 i = 0; i < 5; ++i)
        {
            PutU8(0);
        }
    }

    void TraceWriter::WriteMetadata(string_view engineVersion, string_view executableBasename,
                                    std::span<const Provenance> provenance)
    {
        const usize length = BeginSection(SectionType::Metadata);
        PutString(engineVersion);
        PutString(executableBasename);
        PutU16(static_cast<u16>(provenance.size()));
        for (const Provenance& entry : provenance)
        {
            PutString(entry.Name);
            PutString(entry.ShortSha);
            PutU8(entry.Dirty ? 1 : 0);
        }
        EndSection(length);
    }

    void TraceWriter::WriteStringTable(NameId firstId, std::span<const string_view> strings,
                                       bool isFull)
    {
        const usize length = BeginSection(SectionType::StringTable);
        PutU8(isFull ? 1 : 0);
        PutVarint(firstId);
        PutVarint(strings.size());
        for (const string_view text : strings)
        {
            PutString(text);
        }
        EndSection(length);
    }

    void TraceWriter::WriteTrack(const TrackDescriptor& track)
    {
        const usize length = BeginSection(SectionType::Track);
        PutU8(static_cast<u8>(track.Kind));
        PutVarint(track.Id);
        PutU8(static_cast<u8>(track.Role));
        PutString(track.Name);
        EndSection(length);
    }

    void TraceWriter::WriteChunk(const ChunkData& chunk)
    {
        const usize length = BeginSection(SectionType::Chunk);
        PutVarint(chunk.Thread);
        PutVarint(chunk.SequenceNumber);
        PutU64(chunk.TimestampBase);
        // The base frame the per-record frame deltas are relative to: the first record's frame, so a
        // same-frame run encodes as zeros and a back-dated GPU span encodes as a small negative.
        const u64 baseFrame = chunk.Records.empty() ? 0 : chunk.Records.front().Frame;
        PutVarint(baseFrame);
        PutVarint(chunk.Records.size());
        for (const EventRecord& record : chunk.Records)
        {
            const bool hasTrackOverride = record.Track != 0;
            u8 tag = record.Type & 0x03;
            if (hasTrackOverride)
            {
                tag |= 0x04;
            }
            PutU8(tag);
            if (hasTrackOverride)
            {
                PutVarint(record.Track);
            }
            PutZigzag(static_cast<i64>(record.Frame) - static_cast<i64>(baseFrame));
            const u64 beginDelta = record.BeginTicks >= chunk.TimestampBase
                                       ? record.BeginTicks - chunk.TimestampBase
                                       : 0;
            PutVarint(beginDelta);

            PutVarint(record.Name);
            if (record.Type == static_cast<u8>(TraceFormat::RecordType::ScopeComplete))
            {
                const u64 duration =
                    record.EndTicks >= record.BeginTicks ? record.EndTicks - record.BeginTicks : 0;
                PutVarint(duration);
            }
            else if (record.Type == static_cast<u8>(TraceFormat::RecordType::Counter))
            {
                const CounterValueTag valueTag = SelectCounterTag(record.Value);
                PutU8(static_cast<u8>(valueTag));
                switch (valueTag)
                {
                case CounterValueTag::VarU64:
                    PutVarint(static_cast<u64>(record.Value));
                    break;
                case CounterValueTag::ZigzagI64:
                    PutZigzag(static_cast<i64>(record.Value));
                    break;
                case CounterValueTag::RawF64:
                    u64 bits;
                    std::memcpy(&bits, &record.Value, sizeof(bits));
                    PutU64(bits);
                    break;
                }
            }
        }
        EndSection(length);
    }

    void TraceWriter::WriteAccounting(u64 droppedEvents, u64 droppedThreads)
    {
        const usize length = BeginSection(SectionType::Accounting);
        PutVarint(droppedEvents);
        PutVarint(droppedThreads);
        EndSection(length);
    }

    void TraceWriter::WriteTrailer()
    {
        const usize length = BeginSection(SectionType::Trailer);
        EndSection(length);
    }

    void TraceWriter::PutU8(u8 value)
    {
        m_Bytes.push_back(value);
    }

    void TraceWriter::PutU16(u16 value)
    {
        m_Bytes.push_back(static_cast<u8>(value));
        m_Bytes.push_back(static_cast<u8>(value >> 8));
    }

    void TraceWriter::PutU32(u32 value)
    {
        for (u32 i = 0; i < 4; ++i)
        {
            m_Bytes.push_back(static_cast<u8>(value >> (i * 8)));
        }
    }

    void TraceWriter::PutU64(u64 value)
    {
        for (u32 i = 0; i < 8; ++i)
        {
            m_Bytes.push_back(static_cast<u8>(value >> (i * 8)));
        }
    }

    void TraceWriter::PutVarint(u64 value)
    {
        AppendVarint(m_Bytes, value);
    }

    void TraceWriter::PutZigzag(i64 value)
    {
        AppendVarint(m_Bytes, (static_cast<u64>(value) << 1) ^ static_cast<u64>(value >> 63));
    }

    void TraceWriter::PutString(string_view text)
    {
        PutVarint(text.size());
        m_Bytes.insert(m_Bytes.end(), text.begin(), text.end());
    }

    usize TraceWriter::BeginSection(SectionType type)
    {
        PutU32(static_cast<u32>(type));
        const usize lengthOffset = m_Bytes.size();
        PutU32(0); // payload length, back-patched by EndSection
        return lengthOffset;
    }

    void TraceWriter::EndSection(usize lengthOffset)
    {
        const usize payloadStart = lengthOffset + sizeof(u32);
        const u32 payloadBytes = static_cast<u32>(m_Bytes.size() - payloadStart);
        for (u32 i = 0; i < 4; ++i)
        {
            m_Bytes[lengthOffset + i] = static_cast<u8>(payloadBytes >> (i * 8));
        }
    }
}
