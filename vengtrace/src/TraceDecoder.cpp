#include "TraceDecoder.h"

#include <cstring>

namespace Veng::VengTrace
{
    namespace
    {
        constexpr u8 Magic[8] = {'V', 'E', 'N', 'G', 'T', 'R', 'A', 'C'};
        constexpr u32 KnownFormatVersion = 1;
        constexpr usize PreambleFixedSize = 40;

        enum class SectionType : u32
        {
            Metadata = 1,
            StringTable = 2,
            Track = 3,
            Chunk = 4,
            Accounting = 5,
            Trailer = 6,
        };

        // A bounds-checked forward cursor over a byte span. A read past the end sets Ok false and
        // returns zero; every subsequent read is a no-op, so a caller can decode a section body and
        // check Ok once at the end rather than after every field.
        struct Cursor
        {
            std::span<const u8> Data;
            usize Offset = 0;
            bool Ok = true;

            [[nodiscard]] bool Remaining(usize count) const
            {
                return Offset + count <= Data.size();
            }

            u8 ReadU8()
            {
                if (!Ok || !Remaining(1))
                {
                    Ok = false;
                    return 0;
                }
                return Data[Offset++];
            }

            u16 ReadU16()
            {
                if (!Ok || !Remaining(2))
                {
                    Ok = false;
                    return 0;
                }
                const u16 value =
                    static_cast<u16>(Data[Offset]) | static_cast<u16>(Data[Offset + 1] << 8);
                Offset += 2;
                return value;
            }

            u32 ReadU32()
            {
                if (!Ok || !Remaining(4))
                {
                    Ok = false;
                    return 0;
                }
                u32 value = 0;
                for (u32 i = 0; i < 4; ++i)
                {
                    value |= static_cast<u32>(Data[Offset + i]) << (i * 8);
                }
                Offset += 4;
                return value;
            }

            u64 ReadU64()
            {
                if (!Ok || !Remaining(8))
                {
                    Ok = false;
                    return 0;
                }
                u64 value = 0;
                for (u32 i = 0; i < 8; ++i)
                {
                    value |= static_cast<u64>(Data[Offset + i]) << (i * 8);
                }
                Offset += 8;
                return value;
            }

            u64 ReadVarint()
            {
                u64 value = 0;
                u32 shift = 0;
                for (;;)
                {
                    if (!Ok || !Remaining(1) || shift >= 64)
                    {
                        Ok = false;
                        return 0;
                    }
                    const u8 byte = Data[Offset++];
                    value |= static_cast<u64>(byte & 0x7F) << shift;
                    if ((byte & 0x80) == 0)
                    {
                        break;
                    }
                    shift += 7;
                }
                return value;
            }

            i64 ReadZigzag()
            {
                const u64 encoded = ReadVarint();
                return static_cast<i64>(encoded >> 1) ^ -static_cast<i64>(encoded & 1);
            }

            string ReadString()
            {
                const u64 length = ReadVarint();
                if (!Ok || !Remaining(length))
                {
                    Ok = false;
                    return {};
                }
                string text(reinterpret_cast<const char*>(Data.data() + Offset),
                            static_cast<usize>(length));
                Offset += static_cast<usize>(length);
                return text;
            }
        };

        void DecodeMetadata(std::span<const u8> payload, DecodedTrace& trace)
        {
            Cursor cursor{.Data = payload};
            Metadata meta;
            meta.EngineVersion = cursor.ReadString();
            meta.ExecutableBasename = cursor.ReadString();
            const u16 count = cursor.ReadU16();
            for (u16 i = 0; i < count && cursor.Ok; ++i)
            {
                Provenance entry;
                entry.Name = cursor.ReadString();
                entry.ShortSha = cursor.ReadString();
                entry.Dirty = cursor.ReadU8() != 0;
                meta.Submodules.push_back(std::move(entry));
            }
            if (cursor.Ok)
            {
                trace.Meta = std::move(meta);
            }
        }

        void DecodeStringTable(std::span<const u8> payload, DecodedTrace& trace)
        {
            Cursor cursor{.Data = payload};
            (void)cursor.ReadU8(); // IsFull: the accumulated table is the same whichever way a
            const u32 firstId = static_cast<u32>(cursor.ReadVarint()); // section applies (full or
            const u64 count = cursor.ReadVarint();                     // delta) at ascending ids.
            for (u64 i = 0; i < count && cursor.Ok; ++i)
            {
                const usize id = static_cast<usize>(firstId) + static_cast<usize>(i);
                string value = cursor.ReadString();
                if (id >= 1)
                {
                    if (trace.Strings.size() < id)
                    {
                        trace.Strings.resize(id);
                    }
                    trace.Strings[id - 1] = std::move(value);
                }
            }
        }

        void DecodeTrack(std::span<const u8> payload, DecodedTrace& trace)
        {
            Cursor cursor{.Data = payload};
            Track track;
            track.Kind = static_cast<TrackKind>(cursor.ReadU8());
            track.Id = static_cast<u32>(cursor.ReadVarint());
            track.Role = static_cast<TrackRole>(cursor.ReadU8());
            track.Name = cursor.ReadString();
            if (cursor.Ok)
            {
                trace.Tracks.push_back(std::move(track));
            }
        }

        void DecodeChunk(std::span<const u8> payload, DecodedTrace& trace)
        {
            Cursor cursor{.Data = payload};
            const u32 threadId = static_cast<u32>(cursor.ReadVarint());
            (void)cursor.ReadVarint(); // SequenceNumber: viewer JSON carries no gap concept.
            const u64 timestampBase = cursor.ReadU64();
            const u64 baseFrame = cursor.ReadVarint();
            const u64 recordCount = cursor.ReadVarint();

            for (u64 i = 0; i < recordCount && cursor.Ok; ++i)
            {
                const u8 tag = cursor.ReadU8();
                Event event;
                event.Type = static_cast<RecordType>(tag & 0x03);
                event.Thread = threadId;
                if ((tag & 0x04) != 0)
                {
                    event.HasVirtualTrack = true;
                    event.VirtualTrack = static_cast<u32>(cursor.ReadVarint());
                }
                event.Frame = static_cast<u64>(static_cast<i64>(baseFrame) + cursor.ReadZigzag());
                event.BeginTicks = timestampBase + cursor.ReadVarint();
                event.Name = static_cast<u32>(cursor.ReadVarint());

                switch (event.Type)
                {
                case RecordType::ScopeComplete:
                    event.EndTicks = event.BeginTicks + cursor.ReadVarint();
                    break;
                case RecordType::Counter:
                {
                    event.ValueTag = static_cast<CounterValueTag>(cursor.ReadU8());
                    switch (event.ValueTag)
                    {
                    case CounterValueTag::VarU64:
                        event.Value = static_cast<f64>(cursor.ReadVarint());
                        break;
                    case CounterValueTag::ZigzagI64:
                        event.Value = static_cast<f64>(cursor.ReadZigzag());
                        break;
                    case CounterValueTag::RawF64:
                    {
                        const u64 bits = cursor.ReadU64();
                        std::memcpy(&event.Value, &bits, sizeof(event.Value));
                        break;
                    }
                    }
                    event.EndTicks = event.BeginTicks;
                    break;
                }
                case RecordType::Instant:
                    event.EndTicks = event.BeginTicks;
                    break;
                }

                if (cursor.Ok)
                {
                    trace.Events.push_back(event);
                }
            }
        }
    }

    string_view DecodedTrace::Resolve(u32 id) const
    {
        return (id != 0 && id <= Strings.size()) ? string_view(Strings[id - 1]) : string_view();
    }

    DecodeResult Decode(std::span<const u8> data)
    {
        DecodeResult result;

        if (data.size() < PreambleFixedSize || std::memcmp(data.data(), Magic, sizeof(Magic)) != 0)
        {
            result.Status = DecodeStatus::NotACapture;
            return result;
        }

        Cursor cursor{.Data = data, .Offset = sizeof(Magic)};
        const u32 formatVersion = cursor.ReadU32();
        result.FormatVersion = formatVersion;
        if (formatVersion != KnownFormatVersion)
        {
            result.Status = DecodeStatus::UnknownVersion;
            return result;
        }

        DecodedTrace& trace = result.Trace;
        trace.FormatVersion = formatVersion;
        const u32 preambleSize = cursor.ReadU32();
        trace.TickFrequency = cursor.ReadU64();
        trace.TickBase = cursor.ReadU64();
        trace.Mode = static_cast<CaptureMode>(cursor.ReadU8());
        trace.Config = static_cast<BuildConfig>(cursor.ReadU8());
        trace.ProfileEnabled = cursor.ReadU8() != 0;

        // Honour the preamble's declared size so a future, larger preamble stays readable here.
        usize offset = (preambleSize >= PreambleFixedSize) ? preambleSize : PreambleFixedSize;

        for (;;)
        {
            if (offset + 8 > data.size())
            {
                trace.Truncated = true;
                break;
            }
            Cursor header{.Data = data, .Offset = offset};
            const u32 type = header.ReadU32();
            const u32 payloadBytes = header.ReadU32();
            offset = header.Offset;

            if (offset + payloadBytes > data.size())
            {
                trace.Truncated = true;
                break;
            }
            const std::span<const u8> payload = data.subspan(offset, payloadBytes);
            offset += payloadBytes;

            switch (static_cast<SectionType>(type))
            {
            case SectionType::Metadata:
                DecodeMetadata(payload, trace);
                break;
            case SectionType::StringTable:
                DecodeStringTable(payload, trace);
                break;
            case SectionType::Track:
                DecodeTrack(payload, trace);
                break;
            case SectionType::Chunk:
                DecodeChunk(payload, trace);
                break;
            case SectionType::Accounting:
            {
                Cursor account{.Data = payload};
                trace.DroppedEvents = account.ReadVarint();
                trace.DroppedThreads = account.ReadVarint();
                trace.HasAccounting = account.Ok;
                break;
            }
            case SectionType::Trailer:
                trace.Complete = true;
                return result;
            default:
                trace.UnknownSections.push_back(type);
                break;
            }
        }

        return result;
    }
}
