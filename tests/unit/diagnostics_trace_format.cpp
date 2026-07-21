// Trace-format round-trips: the on-disk stream FileTraceSink produces, decoded back against the
// specification in docs/trace-format.md. These cases are the format's own conformance — the writer
// (TraceWriter, driven by FileTraceSink) and an independent reference decoder written here to the
// spec, exercised over every section, every record type, all three counter encodings, chunk framing
// with a non-zero first sequence, string-table deltas and a full ring-dump table, truncation, an
// unknown section, and an unknown version. The committed reference fixture is round-tripped here and
// is the shared conformance input plan 04's converter and planset-78's JS decoder are tested against.
//
// The whole file is independent of VE_PROFILE: the format has no recording state, and the sink is
// driven with synthetic chunks. One case that runs a live profiler is gated on the macro.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>

#include <Veng/Diagnostics/FileTraceSink.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Diagnostics/TraceSink.h>

#include "Diagnostics/TraceFile.h"
#include "Diagnostics/TraceFormat.h"
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Diagnostics;
using namespace Veng::Diagnostics::TraceFileFormat;

namespace
{
    // ----- The reference decoder: reads the on-disk stream strictly per docs/trace-format.md. -----

    struct DecodedProvenance
    {
        string Name;
        string ShortSha;
        bool Dirty = false;
    };

    struct DecodedMetadata
    {
        string EngineVersion;
        string ExecutableBasename;
        vector<DecodedProvenance> Submodules;
    };

    struct DecodedStringTable
    {
        bool IsFull = false;
        NameId FirstId = 0;
        vector<string> Strings;
    };

    struct DecodedTrack
    {
        TrackKind Kind = TrackKind::Thread;
        u32 Id = 0;
        TrackRole Role = TrackRole::Cpu;
        string Name;
    };

    struct DecodedEvent
    {
        u8 Type = 0;
        u32 Track = 0;
        NameId Name = 0;
        u64 Frame = 0;
        u64 BeginTicks = 0;
        u64 EndTicks = 0;
        f64 Value = 0.0;
        CounterValueTag ValueTag = CounterValueTag::VarU64;
    };

    struct DecodedChunk
    {
        ThreadId Thread = 0;
        u64 SequenceNumber = 0;
        u64 TimestampBase = 0;
        vector<DecodedEvent> Events;
    };

    struct DecodedTrace
    {
        bool VersionRejected = false;
        u32 FormatVersion = 0;
        u64 TickFrequency = 0;
        u64 TickBase = 0;
        CaptureMode Mode = CaptureMode::Triggered;
        BuildConfig Config = BuildConfig::Debug;
        bool ProfileEnabled = false;

        optional<DecodedMetadata> Metadata;
        vector<DecodedStringTable> StringTables;
        vector<DecodedTrack> Tracks;
        vector<DecodedChunk> Chunks;
        optional<std::pair<u64, u64>> Accounting; // {droppedEvents, droppedThreads}
        vector<u32> UnknownSections;

        bool Complete = false;  // a Trailer was reached
        bool Truncated = false; // the stream ended before a Trailer

        // The accumulated string table, applied across every StringTable section (id 0 = no name).
        vector<string> Strings;
        [[nodiscard]] string_view Resolve(NameId id) const
        {
            return (id != 0 && id <= Strings.size()) ? string_view(Strings[id - 1]) : string_view();
        }
    };

    u16 ReadU16(std::span<const u8> d, usize& o)
    {
        const u16 v = static_cast<u16>(d[o]) | (static_cast<u16>(d[o + 1]) << 8);
        o += 2;
        return v;
    }

    u32 ReadU32(std::span<const u8> d, usize& o)
    {
        u32 v = 0;
        for (u32 i = 0; i < 4; ++i)
        {
            v |= static_cast<u32>(d[o + i]) << (i * 8);
        }
        o += 4;
        return v;
    }

    u64 ReadU64(std::span<const u8> d, usize& o)
    {
        u64 v = 0;
        for (u32 i = 0; i < 8; ++i)
        {
            v |= static_cast<u64>(d[o + i]) << (i * 8);
        }
        o += 8;
        return v;
    }

    u64 ReadVar(std::span<const u8> d, usize& o)
    {
        u64 v = 0;
        REQUIRE(ReadVarint(d, o, v));
        return v;
    }

    string ReadStr(std::span<const u8> d, usize& o)
    {
        const u64 len = ReadVar(d, o);
        REQUIRE(o + len <= d.size());
        string s(reinterpret_cast<const char*>(d.data() + o), len);
        o += len;
        return s;
    }

    DecodedMetadata DecodeMetadata(std::span<const u8> p)
    {
        usize o = 0;
        DecodedMetadata m;
        m.EngineVersion = ReadStr(p, o);
        m.ExecutableBasename = ReadStr(p, o);
        const u16 count = ReadU16(p, o);
        for (u16 i = 0; i < count; ++i)
        {
            DecodedProvenance entry;
            entry.Name = ReadStr(p, o);
            entry.ShortSha = ReadStr(p, o);
            entry.Dirty = p[o++] != 0;
            m.Submodules.push_back(std::move(entry));
        }
        return m;
    }

    DecodedStringTable DecodeStringTable(std::span<const u8> p)
    {
        usize o = 0;
        DecodedStringTable t;
        t.IsFull = p[o++] != 0;
        t.FirstId = static_cast<NameId>(ReadVar(p, o));
        const u64 count = ReadVar(p, o);
        for (u64 i = 0; i < count; ++i)
        {
            t.Strings.push_back(ReadStr(p, o));
        }
        return t;
    }

    DecodedTrack DecodeTrack(std::span<const u8> p)
    {
        usize o = 0;
        DecodedTrack t;
        t.Kind = static_cast<TrackKind>(p[o++]);
        t.Id = static_cast<u32>(ReadVar(p, o));
        t.Role = static_cast<TrackRole>(p[o++]);
        t.Name = ReadStr(p, o);
        return t;
    }

    DecodedChunk DecodeChunk(std::span<const u8> p)
    {
        usize o = 0;
        DecodedChunk c;
        c.Thread = static_cast<ThreadId>(ReadVar(p, o));
        c.SequenceNumber = ReadVar(p, o);
        c.TimestampBase = ReadU64(p, o);
        const u64 baseFrame = ReadVar(p, o);
        const u64 recordCount = ReadVar(p, o);
        for (u64 i = 0; i < recordCount; ++i)
        {
            const u8 tag = p[o++];
            DecodedEvent e;
            e.Type = tag & 0x03;
            if ((tag & 0x04) != 0)
            {
                e.Track = static_cast<u32>(ReadVar(p, o));
            }
            e.Frame = static_cast<u64>(static_cast<i64>(baseFrame) + DecodeZigzag(ReadVar(p, o)));
            e.BeginTicks = c.TimestampBase + ReadVar(p, o);
            e.Name = static_cast<NameId>(ReadVar(p, o));
            if (e.Type == static_cast<u8>(TraceFormat::RecordType::ScopeComplete))
            {
                e.EndTicks = e.BeginTicks + ReadVar(p, o);
            }
            else if (e.Type == static_cast<u8>(TraceFormat::RecordType::Counter))
            {
                e.ValueTag = static_cast<CounterValueTag>(p[o++]);
                switch (e.ValueTag)
                {
                case CounterValueTag::VarU64:
                    e.Value = static_cast<f64>(ReadVar(p, o));
                    break;
                case CounterValueTag::ZigzagI64:
                    e.Value = static_cast<f64>(DecodeZigzag(ReadVar(p, o)));
                    break;
                case CounterValueTag::RawF64:
                {
                    const u64 bits = ReadU64(p, o);
                    std::memcpy(&e.Value, &bits, sizeof(e.Value));
                    break;
                }
                }
                e.EndTicks = e.BeginTicks;
            }
            else
            {
                e.EndTicks = e.BeginTicks;
            }
            c.Events.push_back(e);
        }
        return c;
    }

    // Parses a whole stream, tolerating truncation exactly as the spec prescribes: read sections
    // until a Trailer (clean) or until a section header or payload cannot be fully read (truncated),
    // keeping every fully-parsed section.
    DecodedTrace Decode(std::span<const u8> data)
    {
        DecodedTrace out;
        if (data.size() < PreambleSize || std::memcmp(data.data(), Magic, sizeof(Magic)) != 0)
        {
            out.Truncated = true;
            return out;
        }
        usize o = sizeof(Magic);
        out.FormatVersion = ReadU32(data, o);
        if (out.FormatVersion != FormatVersion)
        {
            out.VersionRejected = true;
            return out;
        }
        const u32 preambleSize = ReadU32(data, o);
        out.TickFrequency = ReadU64(data, o);
        out.TickBase = ReadU64(data, o);
        out.Mode = static_cast<CaptureMode>(data[o++]);
        out.Config = static_cast<BuildConfig>(data[o++]);
        out.ProfileEnabled = data[o++] != 0;
        o = preambleSize;

        for (;;)
        {
            if (o + 8 > data.size())
            {
                out.Truncated = true;
                break;
            }
            const u32 type = ReadU32(data, o);
            const u32 payloadBytes = ReadU32(data, o);
            if (o + payloadBytes > data.size())
            {
                out.Truncated = true;
                break;
            }
            const std::span<const u8> payload = data.subspan(o, payloadBytes);
            o += payloadBytes;

            switch (static_cast<SectionType>(type))
            {
            case SectionType::Metadata:
                out.Metadata = DecodeMetadata(payload);
                break;
            case SectionType::StringTable:
            {
                DecodedStringTable table = DecodeStringTable(payload);
                if (!table.Strings.empty())
                {
                    const usize maxId =
                        static_cast<usize>(table.FirstId) + table.Strings.size() - 1;
                    if (out.Strings.size() < maxId)
                    {
                        out.Strings.resize(maxId);
                    }
                    for (usize i = 0; i < table.Strings.size(); ++i)
                    {
                        out.Strings[table.FirstId + i - 1] = table.Strings[i];
                    }
                }
                out.StringTables.push_back(std::move(table));
                break;
            }
            case SectionType::Track:
                out.Tracks.push_back(DecodeTrack(payload));
                break;
            case SectionType::Chunk:
                out.Chunks.push_back(DecodeChunk(payload));
                break;
            case SectionType::Accounting:
            {
                usize po = 0;
                const u64 events = ReadVar(payload, po);
                const u64 threads = ReadVar(payload, po);
                out.Accounting = std::pair<u64, u64>(events, threads);
                break;
            }
            case SectionType::Trailer:
                out.Complete = true;
                return out;
            default:
                out.UnknownSections.push_back(type);
                break;
            }
        }
        return out;
    }

    // ----- The committed reference fixture, built deterministically from clean synthetic values. ---
    //
    // No absolute path, no host-specific field, no game-derived string: the fixture is a public
    // binary and plan 07 byte-scans it, so every string it carries is generic and hand-chosen here.

    struct Fixture
    {
        vector<u8> Complete;
        usize AfterFirstChunk = 0; // byte offset just past the first chunk section
    };

    Fixture BuildReferenceFixture()
    {
        constexpr u64 TickFrequency = 24'000'000;
        constexpr u64 TickBase = 1'000'000;

        TraceWriter writer(CaptureMode::RingDump, BuildConfig::Debug, /*profileEnabled=*/true,
                           TickFrequency, TickBase);

        const TraceWriter::Provenance provenance[] = {
            {.Name = "engine", .ShortSha = "0000000", .Dirty = false},
            {.Name = "consumer", .ShortSha = "1111111", .Dirty = true},
        };
        writer.WriteMetadata("0.0.0-fixture", "trace-fixture", std::span(provenance));

        const string strings[] = {"Main",        "Frame",    "Update",      "draw.calls",
                                  "queue.depth", "gpu.ms",   "temperature", "GpuPass",
                                  "marker",      "Worker 0", "GPU"};
        vector<string_view> views;
        for (const string& s : strings)
        {
            views.emplace_back(s);
        }
        // A ring dump writes the full table (ids 1..N), so ids interned in discarded chunks resolve.
        writer.WriteStringTable(1, views, /*isFull=*/true);

        writer.WriteTrack(
            {.Kind = TrackKind::Thread, .Id = 1, .Role = TrackRole::Cpu, .Name = "Main"});
        writer.WriteTrack(
            {.Kind = TrackKind::Thread, .Id = 2, .Role = TrackRole::Cpu, .Name = "Worker 0"});
        writer.WriteTrack(
            {.Kind = TrackKind::Virtual, .Id = 1, .Role = TrackRole::Gpu, .Name = "GPU"});

        auto scope = [](NameId name, u64 frame, u64 begin, u64 end, u32 track = 0)
        {
            return EventRecord{.Type = static_cast<u8>(TraceFormat::RecordType::ScopeComplete),
                               .Track = track,
                               .Name = name,
                               .Frame = frame,
                               .BeginTicks = begin,
                               .EndTicks = end};
        };
        auto counter = [](NameId name, u64 frame, u64 begin, f64 value)
        {
            return EventRecord{.Type = static_cast<u8>(TraceFormat::RecordType::Counter),
                               .Name = name,
                               .Frame = frame,
                               .BeginTicks = begin,
                               .EndTicks = begin,
                               .Value = value};
        };
        auto instant = [](NameId name, u64 frame, u64 begin)
        {
            return EventRecord{.Type = static_cast<u8>(TraceFormat::RecordType::Instant),
                               .Name = name,
                               .Frame = frame,
                               .BeginTicks = begin,
                               .EndTicks = begin};
        };

        // First chunk: sequence 3, not 0 — a ring dump begins mid-sequence. All three counter
        // encodings, a scope, an instant, and a scope with a large begin delta (a multi-byte varint).
        ChunkData chunkA;
        chunkA.Thread = 1;
        chunkA.SequenceNumber = 3;
        chunkA.TimestampBase = TickBase;
        chunkA.Records = {
            scope(2, 10, TickBase, TickBase + 500),
            scope(3, 10, TickBase + 50, TickBase + 200),
            counter(4, 10, TickBase + 100, 1234.0), // VarU64
            counter(5, 10, TickBase + 110, -7.0),   // ZigzagI64
            counter(6, 10, TickBase + 120, 3.5),    // RawF64
            instant(9, 10, TickBase + 150),
            scope(2, 11, TickBase + 200'000, TickBase + 200'100), // wide begin delta, next frame
        };
        writer.WriteChunk(chunkA);

        Fixture fixture;
        fixture.AfterFirstChunk = writer.GetBytes().size();

        // Second chunk: a different thread, sequence 0, and a back-dated GPU span on the virtual
        // track — its frame (9) precedes the chunk's base frame (12), so its frame delta is negative.
        ChunkData chunkB;
        chunkB.Thread = 2;
        chunkB.SequenceNumber = 0;
        chunkB.TimestampBase = 1'005'000;
        chunkB.Records = {
            scope(2, 12, 1'005'000, 1'005'300),
            scope(8, 9, 1'005'100, 1'005'900,
                  /*track=*/1), // back-dated GPU pass on virtual track 1
        };
        writer.WriteChunk(chunkB);

        writer.WriteAccounting(/*droppedEvents=*/42, /*droppedThreads=*/1);
        writer.WriteTrailer();

        fixture.Complete = writer.TakeBytes();
        return fixture;
    }

    vector<u8> ReadFile(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void WriteFile(const std::filesystem::path& p, std::span<const u8> bytes)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    // Builds a profiler-internal chunk buffer (the fixed-width encoding OnChunk receives) so the sink
    // can be driven without a live profiler.
    vector<u8> BuildInternalChunk(u64 base, u64 sequence,
                                  const vector<TraceFormat::EventRecord>& records)
    {
        using TraceFormat::ChunkHeader;
        using TraceFormat::EventRecord;
        vector<u8> buffer(sizeof(ChunkHeader) + records.size() * sizeof(EventRecord));
        ChunkHeader header;
        header.TimestampBase = base;
        header.SequenceNumber = sequence;
        header.FirstRecordOffset = sizeof(ChunkHeader);
        header.RecordBytes = static_cast<u32>(records.size() * sizeof(EventRecord));
        std::memcpy(buffer.data(), &header, sizeof(header));
        std::memcpy(buffer.data() + sizeof(header), records.data(),
                    records.size() * sizeof(EventRecord));
        return buffer;
    }
}

TEST_CASE("trace format: preamble and trailer round-trip")
{
    TraceWriter writer(CaptureMode::Triggered, BuildConfig::Release, /*profileEnabled=*/true,
                       24'000'000, 999);
    const TraceWriter::Provenance provenance[] = {
        {.Name = "engine", .ShortSha = "abc1234", .Dirty = true}};
    writer.WriteMetadata("1.2.3", "app", std::span(provenance));
    writer.WriteTrailer();

    const DecodedTrace t = Decode(writer.GetBytes());
    CHECK(t.FormatVersion == FormatVersion);
    CHECK(t.TickFrequency == 24'000'000);
    CHECK(t.TickBase == 999);
    CHECK(t.Mode == CaptureMode::Triggered);
    CHECK(t.Config == BuildConfig::Release);
    CHECK(t.ProfileEnabled);
    CHECK(t.Complete);
    CHECK_FALSE(t.Truncated);
    REQUIRE(t.Metadata.has_value());
    CHECK(t.Metadata->EngineVersion == "1.2.3");
    CHECK(t.Metadata->ExecutableBasename == "app");
    REQUIRE(t.Metadata->Submodules.size() == 1);
    CHECK(t.Metadata->Submodules[0].Name == "engine");
    CHECK(t.Metadata->Submodules[0].ShortSha == "abc1234");
    CHECK(t.Metadata->Submodules[0].Dirty);
}

TEST_CASE("trace format: every record type round-trips to identity")
{
    TraceWriter writer(CaptureMode::Triggered, BuildConfig::Debug, true, 24'000'000, 0);
    const string strings[] = {"scope", "counter", "instant"};
    vector<string_view> views{strings[0], strings[1], strings[2]};
    writer.WriteStringTable(1, views, false);

    ChunkData chunk;
    chunk.Thread = 1;
    chunk.SequenceNumber = 0;
    chunk.TimestampBase = 1000;
    chunk.Records = {
        EventRecord{.Type = 0, .Name = 1, .Frame = 5, .BeginTicks = 1010, .EndTicks = 1090},
        EventRecord{
            .Type = 1, .Name = 2, .Frame = 5, .BeginTicks = 1020, .EndTicks = 1020, .Value = 17.0},
        EventRecord{.Type = 2, .Name = 3, .Frame = 5, .BeginTicks = 1030, .EndTicks = 1030},
    };
    writer.WriteChunk(chunk);
    writer.WriteTrailer();

    const DecodedTrace t = Decode(writer.GetBytes());
    REQUIRE(t.Chunks.size() == 1);
    REQUIRE(t.Chunks[0].Events.size() == 3);
    const auto& e = t.Chunks[0].Events;
    CHECK(e[0].Type == 0);
    CHECK(e[0].BeginTicks == 1010);
    CHECK(e[0].EndTicks == 1090);
    CHECK(t.Resolve(e[0].Name) == "scope");
    CHECK(e[1].Type == 1);
    CHECK(e[1].Value == doctest::Approx(17.0));
    CHECK(e[1].BeginTicks == 1020);
    CHECK(t.Resolve(e[1].Name) == "counter");
    CHECK(e[2].Type == 2);
    CHECK(e[2].BeginTicks == 1030);
    CHECK(t.Resolve(e[2].Name) == "instant");
}

TEST_CASE("trace format: counter values pick the narrowest exact encoding")
{
    struct Case
    {
        f64 Value;
        CounterValueTag Tag;
    };
    const Case cases[] = {
        {.Value = 0.0, .Tag = CounterValueTag::VarU64},
        {.Value = 1234.0, .Tag = CounterValueTag::VarU64},
        {.Value = 18446744073709551615.0, .Tag = CounterValueTag::RawF64}, // 2^64-1 not f64-exact
        {.Value = 9007199254740992.0, .Tag = CounterValueTag::VarU64},     // 2^53 exact
        {.Value = -1.0, .Tag = CounterValueTag::ZigzagI64},
        {.Value = -7.0, .Tag = CounterValueTag::ZigzagI64},
        {.Value = 3.5, .Tag = CounterValueTag::RawF64},
        {.Value = 0.1, .Tag = CounterValueTag::RawF64},
        {.Value = -0.0, .Tag = CounterValueTag::RawF64}, // sign bit is lost by an integer encoding
    };

    for (const Case& c : cases)
    {
        CHECK(SelectCounterTag(c.Value) == c.Tag);

        TraceWriter writer(CaptureMode::Triggered, BuildConfig::Debug, true, 24'000'000, 0);
        ChunkData chunk;
        chunk.Thread = 1;
        chunk.TimestampBase = 0;
        chunk.Records = {EventRecord{
            .Type = 1, .Name = 0, .Frame = 0, .BeginTicks = 0, .EndTicks = 0, .Value = c.Value}};
        writer.WriteChunk(chunk);
        writer.WriteTrailer();

        const DecodedTrace t = Decode(writer.GetBytes());
        REQUIRE(t.Chunks.size() == 1);
        REQUIRE(t.Chunks[0].Events.size() == 1);
        const DecodedEvent& e = t.Chunks[0].Events[0];
        CHECK(e.ValueTag == c.Tag);
        // Bit-for-bit identity, so −0.0 and any exact value survive.
        u64 wrote;
        u64 read;
        std::memcpy(&wrote, &c.Value, sizeof(wrote));
        std::memcpy(&read, &e.Value, sizeof(read));
        CHECK(wrote == read);
    }
}

TEST_CASE("trace format: special f64 counter values survive as raw bits")
{
    const f64 specials[] = {std::numeric_limits<f64>::infinity(),
                            -std::numeric_limits<f64>::infinity(),
                            std::numeric_limits<f64>::quiet_NaN()};
    for (const f64 value : specials)
    {
        CHECK(SelectCounterTag(value) == CounterValueTag::RawF64);

        TraceWriter writer(CaptureMode::Triggered, BuildConfig::Debug, true, 24'000'000, 0);
        ChunkData chunk;
        chunk.Thread = 1;
        chunk.Records = {EventRecord{.Type = 1, .Name = 0, .Value = value}};
        writer.WriteChunk(chunk);
        writer.WriteTrailer();

        const DecodedTrace t = Decode(writer.GetBytes());
        const f64 got = t.Chunks[0].Events[0].Value;
        u64 wrote;
        u64 read;
        std::memcpy(&wrote, &value, sizeof(wrote));
        std::memcpy(&read, &got, sizeof(read));
        CHECK(wrote == read);
    }
}

TEST_CASE("trace format: timestamp deltas survive width transitions and a non-zero first sequence")
{
    TraceWriter writer(CaptureMode::RingDump, BuildConfig::Debug, true, 24'000'000, 0);
    ChunkData chunk;
    chunk.Thread = 7;
    chunk.SequenceNumber = 42; // a ring dump's first chunk is not sequence 0
    chunk.TimestampBase = 5'000'000;
    // Begin deltas spanning one, two, three, and five varint bytes.
    const u64 deltas[] = {0, 1, 127, 128, 16383, 16384, 2'000'000, 300'000'000};
    for (const u64 delta : deltas)
    {
        chunk.Records.push_back(EventRecord{.Type = 0,
                                            .Name = 1,
                                            .Frame = 100,
                                            .BeginTicks = chunk.TimestampBase + delta,
                                            .EndTicks = chunk.TimestampBase + delta + 10});
    }
    writer.WriteChunk(chunk);
    writer.WriteTrailer();

    const DecodedTrace t = Decode(writer.GetBytes());
    REQUIRE(t.Chunks.size() == 1);
    CHECK(t.Chunks[0].SequenceNumber == 42);
    REQUIRE(t.Chunks[0].Events.size() == std::size(deltas));
    for (usize i = 0; i < std::size(deltas); ++i)
    {
        CHECK(t.Chunks[0].Events[i].BeginTicks == 5'000'000 + deltas[i]);
        CHECK(t.Chunks[0].Events[i].EndTicks == 5'000'000 + deltas[i] + 10);
    }
}

TEST_CASE("trace format: a chunk decodes standalone from its own base and a sequence gap shows")
{
    TraceWriter writer(CaptureMode::RingDump, BuildConfig::Debug, true, 24'000'000, 0);
    // Two chunks on one thread whose sequence numbers jump — the discarded span is a visible gap.
    ChunkData a{.Thread = 1, .SequenceNumber = 5, .TimestampBase = 1000, .Records = {}};
    a.Records.push_back(
        EventRecord{.Type = 0, .Name = 1, .Frame = 0, .BeginTicks = 1000, .EndTicks = 1100});
    ChunkData b{.Thread = 1, .SequenceNumber = 9, .TimestampBase = 9000, .Records = {}};
    b.Records.push_back(
        EventRecord{.Type = 0, .Name = 1, .Frame = 0, .BeginTicks = 9000, .EndTicks = 9100});
    writer.WriteChunk(a);
    writer.WriteChunk(b);
    writer.WriteTrailer();

    const DecodedTrace t = Decode(writer.GetBytes());
    REQUIRE(t.Chunks.size() == 2);
    CHECK(t.Chunks[0].SequenceNumber == 5);
    CHECK(t.Chunks[1].SequenceNumber == 9);
    // The gap (5 -> 9) is more than one, so chunks were discarded between them.
    CHECK(t.Chunks[1].SequenceNumber - t.Chunks[0].SequenceNumber > 1);
    // Each chunk's records reconstruct from its own base with no cross-chunk state.
    CHECK(t.Chunks[0].Events[0].BeginTicks == 1000);
    CHECK(t.Chunks[1].Events[0].BeginTicks == 9000);
}

TEST_CASE("trace format: frame indices survive a round trip including a back-dated event")
{
    TraceWriter writer(CaptureMode::Triggered, BuildConfig::Debug, true, 24'000'000, 0);
    ChunkData chunk;
    chunk.Thread = 1;
    chunk.TimestampBase = 0;
    chunk.Records = {
        EventRecord{.Type = 0, .Name = 1, .Frame = 100, .BeginTicks = 10, .EndTicks = 20},
        // A back-dated GPU span: it measures an earlier frame than the chunk's base frame (100).
        EventRecord{
            .Type = 0, .Track = 1, .Name = 2, .Frame = 96, .BeginTicks = 30, .EndTicks = 40},
        EventRecord{.Type = 0, .Name = 1, .Frame = 101, .BeginTicks = 50, .EndTicks = 60},
    };
    writer.WriteChunk(chunk);
    writer.WriteTrailer();

    const DecodedTrace t = Decode(writer.GetBytes());
    REQUIRE(t.Chunks[0].Events.size() == 3);
    CHECK(t.Chunks[0].Events[0].Frame == 100);
    CHECK(t.Chunks[0].Events[1].Frame == 96); // negative frame delta survived
    CHECK(t.Chunks[0].Events[1].Track == 1);  // virtual-track override survived
    CHECK(t.Chunks[0].Events[2].Frame == 101);
}

TEST_CASE("trace format: string ids stay stable across deltas")
{
    TraceWriter writer(CaptureMode::Triggered, BuildConfig::Debug, true, 24'000'000, 0);
    const string first[] = {"a", "b"};
    const string second[] = {"c"};
    const string_view firstViews[] = {string_view(first[0]), string_view(first[1])};
    const string_view secondViews[] = {string_view(second[0])};
    writer.WriteStringTable(1, std::span(firstViews), false);
    writer.WriteStringTable(3, std::span(secondViews), false); // delta appends at id 3
    writer.WriteTrailer();

    const DecodedTrace t = Decode(writer.GetBytes());
    REQUIRE(t.StringTables.size() == 2);
    CHECK(t.Resolve(1) == "a");
    CHECK(t.Resolve(2) == "b");
    CHECK(t.Resolve(3) == "c");
}

TEST_CASE("trace format: an unknown section is skipped without error")
{
    // Hand-build a stream: valid preamble, an unknown section, then a Trailer.
    TraceWriter writer(CaptureMode::Triggered, BuildConfig::Debug, true, 24'000'000, 0);
    writer.WriteTrailer();
    vector<u8> bytes = writer.TakeBytes();

    // Splice an unknown section (type 9999) in front of the trailer.
    const usize trailerStart = PreambleSize;
    vector<u8> unknown;
    auto putU32 = [&unknown](u32 v)
    {
        for (u32 i = 0; i < 4; ++i)
        {
            unknown.push_back(static_cast<u8>(v >> (i * 8)));
        }
    };
    putU32(9999);
    putU32(3);
    unknown.insert(unknown.end(), {0xAA, 0xBB, 0xCC});

    vector<u8> spliced(bytes.begin(), bytes.begin() + trailerStart);
    spliced.insert(spliced.end(), unknown.begin(), unknown.end());
    spliced.insert(spliced.end(), bytes.begin() + trailerStart, bytes.end());

    const DecodedTrace t = Decode(spliced);
    CHECK(t.Complete);
    CHECK_FALSE(t.Truncated);
    REQUIRE(t.UnknownSections.size() == 1);
    CHECK(t.UnknownSections[0] == 9999);
}

TEST_CASE("trace format: an unknown version is rejected cleanly")
{
    TraceWriter writer(CaptureMode::Triggered, BuildConfig::Debug, true, 24'000'000, 0);
    writer.WriteTrailer();
    vector<u8> bytes = writer.TakeBytes();
    // Bump the format version past what this decoder knows (the field is at offset 8).
    bytes[8] = 0xFF;

    const DecodedTrace t = Decode(bytes);
    CHECK(t.VersionRejected);
    CHECK_FALSE(t.Complete);
}

TEST_CASE("trace format: a truncated stream reads up to its last complete section")
{
    const Fixture fixture = BuildReferenceFixture();
    // Cut inside the second chunk's section header, so only the sections through the first chunk are
    // complete and no trailer is present.
    vector<u8> truncated(fixture.Complete.begin(),
                         fixture.Complete.begin() + fixture.AfterFirstChunk + 3);

    const DecodedTrace t = Decode(truncated);
    CHECK(t.Truncated);
    CHECK_FALSE(t.Complete);
    // Everything before the cut survived: metadata, the full string table, the tracks, chunk one.
    REQUIRE(t.Metadata.has_value());
    CHECK(t.StringTables.size() == 1);
    CHECK(t.Tracks.size() == 3);
    REQUIRE(t.Chunks.size() == 1);
    CHECK(t.Chunks[0].SequenceNumber == 3);
}

TEST_CASE("trace format: the committed reference fixture matches the writer and round-trips")
{
    const Fixture fixture = BuildReferenceFixture();
    const std::filesystem::path dir = VENG_TEST_FIXTURE_DIR;
    const std::filesystem::path completePath = dir / "trace-fixture.vtrace";
    const std::filesystem::path truncatedPath = dir / "trace-fixture-truncated.vtrace";
    const vector<u8> truncated(fixture.Complete.begin(),
                               fixture.Complete.begin() + fixture.AfterFirstChunk + 3);

    // Self-heal the committed fixtures when missing or when a deliberate regen is requested; a normal
    // run compares the committed bytes against the writer's output, so the fixture cannot silently
    // drift from the format.
    std::error_code ec;
    const bool regen = std::getenv("VENG_REGEN_TRACE_FIXTURE") != nullptr;
    if (regen || !std::filesystem::exists(completePath, ec))
    {
        std::filesystem::create_directories(dir, ec);
        WriteFile(completePath, fixture.Complete);
        WriteFile(truncatedPath, truncated);
        MESSAGE("Wrote reference fixtures to " << dir.string());
    }

    const vector<u8> committed = ReadFile(completePath);
    REQUIRE_FALSE(committed.empty());
    CHECK(committed == fixture.Complete);

    const DecodedTrace t = Decode(committed);
    CHECK(t.Complete);
    CHECK_FALSE(t.Truncated);

    // Every section is present.
    REQUIRE(t.Metadata.has_value());
    CHECK(t.Metadata->EngineVersion == "0.0.0-fixture");
    CHECK(t.Metadata->ExecutableBasename == "trace-fixture");
    REQUIRE(t.Metadata->Submodules.size() == 2);
    CHECK(t.Metadata->Submodules[0].Name == "engine");
    CHECK(t.Metadata->Submodules[1].Dirty);
    CHECK(t.StringTables.size() == 1);
    CHECK(t.StringTables[0].IsFull);
    CHECK(t.Tracks.size() == 3);
    CHECK(t.Tracks[2].Kind == TrackKind::Virtual);
    CHECK(t.Tracks[2].Role == TrackRole::Gpu);
    REQUIRE(t.Chunks.size() == 2);
    REQUIRE(t.Accounting.has_value());
    CHECK(t.Accounting->first == 42);
    CHECK(t.Accounting->second == 1);

    // Every record type and every counter encoding.
    const auto& first = t.Chunks[0];
    CHECK(first.SequenceNumber == 3);
    REQUIRE(first.Events.size() == 7);
    CHECK(first.Events[2].ValueTag == CounterValueTag::VarU64);
    CHECK(first.Events[2].Value == doctest::Approx(1234.0));
    CHECK(first.Events[3].ValueTag == CounterValueTag::ZigzagI64);
    CHECK(first.Events[3].Value == doctest::Approx(-7.0));
    CHECK(first.Events[4].ValueTag == CounterValueTag::RawF64);
    CHECK(first.Events[4].Value == doctest::Approx(3.5));
    CHECK(first.Events[5].Type == static_cast<u8>(TraceFormat::RecordType::Instant));

    // The back-dated GPU span in the second chunk.
    const auto& second = t.Chunks[1];
    REQUIRE(second.Events.size() == 2);
    CHECK(second.Events[1].Track == 1);
    CHECK(second.Events[1].Frame == 9);
    CHECK(t.Resolve(second.Events[1].Name) == "GpuPass");
}

TEST_CASE("trace format: the committed fixture carries no path-like identity leak")
{
    const std::filesystem::path completePath =
        std::filesystem::path(VENG_TEST_FIXTURE_DIR) / "trace-fixture.vtrace";
    if (!std::filesystem::exists(completePath))
    {
        return; // created by the round-trip case; nothing to scan on a first cold run
    }
    const vector<u8> bytes = ReadFile(completePath);
    const string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // No absolute-path prefix and no build-tree marker may appear in a committed public fixture.
    for (const char* needle : {"/Users/", "/home/", "/private/", "C:\\", ".cpp", "build-debug"})
    {
        CHECK(text.find(needle) == string_view::npos);
    }
}

TEST_CASE("FileTraceSink: transcodes queued chunks and commits a decodable file")
{
    const std::filesystem::path out =
        Veng::TestSupport::TempDir() / "captures" / "sink-basic.vtrace";

    {
        Unique<FileTraceSink> sink = FileTraceSink::Create(out, /*ringDump=*/false);
        REQUIRE(sink != nullptr);

        StringTableDelta delta;
        delta.FirstId = 1;
        const string names[] = {"Frame", "counter"};
        delta.Strings = {string_view(names[0]), string_view(names[1])};
        sink->OnStrings(delta);

        f64 sample = 12.0;
        u64 sampleBits;
        std::memcpy(&sampleBits, &sample, sizeof(sampleBits));
        const vector<TraceFormat::EventRecord> records = {
            TraceFormat::EventRecord{.Type =
                                         static_cast<u8>(TraceFormat::RecordType::ScopeComplete),
                                     .Name = 1,
                                     .Frame = 3,
                                     .BeginDelta = 10,
                                     .EndOrValue = 90},
            TraceFormat::EventRecord{.Type = static_cast<u8>(TraceFormat::RecordType::Counter),
                                     .Name = 2,
                                     .Frame = 3,
                                     .BeginDelta = 20,
                                     .EndOrValue = sampleBits},
        };
        const vector<u8> chunk = BuildInternalChunk(/*base=*/2000, /*sequence=*/4, records);
        sink->OnChunk(/*thread=*/1, chunk.data(), chunk.size());

        sink->SetAccounting(/*droppedEvents=*/2, /*droppedThreads=*/0);
        sink->OnClose();
    }

    const vector<u8> bytes = ReadFile(out);
    REQUIRE_FALSE(bytes.empty());
    const DecodedTrace t = Decode(bytes);
    CHECK(t.Complete);
    CHECK_FALSE(t.Truncated);
    REQUIRE(t.Metadata.has_value());
    CHECK_FALSE(t.Metadata->EngineVersion.empty()); // stamped by the sink from the build
    REQUIRE(t.Chunks.size() == 1);
    CHECK(t.Chunks[0].Thread == 1);
    CHECK(t.Chunks[0].SequenceNumber == 4);
    REQUIRE(t.Chunks[0].Events.size() == 2);
    CHECK(t.Chunks[0].Events[0].BeginTicks == 2010);
    CHECK(t.Chunks[0].Events[0].EndTicks == 2090);
    CHECK(t.Chunks[0].Events[0].Frame == 3);
    CHECK(t.Resolve(t.Chunks[0].Events[0].Name) == "Frame");
    CHECK(t.Chunks[0].Events[1].Value == doctest::Approx(12.0));
    REQUIRE(t.Accounting.has_value());
    CHECK(t.Accounting->first == 2);
    // The sink emits a thread-track descriptor for the producing thread.
    REQUIRE_FALSE(t.Tracks.empty());
    CHECK(t.Tracks[0].Kind == TrackKind::Thread);
    CHECK(t.Tracks[0].Id == 1);
}

TEST_CASE("FileTraceSink: a sink dropped without a clean close reads as truncated")
{
    const std::filesystem::path out =
        Veng::TestSupport::TempDir() / "captures" / "sink-truncated.vtrace";
    {
        Unique<FileTraceSink> sink = FileTraceSink::Create(out);
        const vector<TraceFormat::EventRecord> records = {TraceFormat::EventRecord{
            .Type = 0, .Name = 0, .Frame = 0, .BeginDelta = 0, .EndOrValue = 5}};
        const vector<u8> chunk = BuildInternalChunk(1000, 0, records);
        sink->OnChunk(1, chunk.data(), chunk.size());
        // No OnClose: the destructor stops the writer without a trailer.
    }
    const vector<u8> bytes = ReadFile(out);
    REQUIRE_FALSE(bytes.empty());
    const DecodedTrace t = Decode(bytes);
    CHECK_FALSE(t.Complete);
    CHECK(t.Truncated);
}

#if defined(VE_PROFILE) && VE_PROFILE
TEST_CASE("FileTraceSink: a live profiler capture writes a decodable file")
{
    const std::filesystem::path out =
        Veng::TestSupport::TempDir() / "captures" / "sink-live.vtrace";
    // The sink outlives the profiler: the profiler's destructor flushes its chunks and closes the
    // sink (which commits the file), so the sink must still be alive at that point.
    const Unique<FileTraceSink> sink = FileTraceSink::Create(out);
    {
        ProfilerConfig config;
        config.InitialMode = ProfilerMode::Ring;
        Profiler profiler(config);
        profiler.SetSink(sink.get());

        for (int i = 0; i < 3; ++i)
        {
            VE_PROFILE_FRAME();
            {
                VE_PROFILE_SCOPE("LiveScope");
            }
            VE_PROFILE_COUNTER("live.counter", static_cast<f64>(i));
        }
    }
    const vector<u8> bytes = ReadFile(out);
    REQUIRE_FALSE(bytes.empty());
    const DecodedTrace t = Decode(bytes);
    CHECK(t.Complete);
    CHECK_FALSE(t.Truncated);
    // The scope name interned during the live run resolves in the written string table.
    bool sawScope = false;
    for (const DecodedChunk& chunk : t.Chunks)
    {
        for (const DecodedEvent& e : chunk.Events)
        {
            if (t.Resolve(e.Name) == "LiveScope")
            {
                sawScope = true;
            }
        }
    }
    CHECK(sawScope);
}
#endif
