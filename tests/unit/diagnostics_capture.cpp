// Capture control: the policy layer over the profiler's buffers — triggered captures, the
// self-terminating frame budget, the continuous ring and its dump, the overlap matrix, the retention
// cap, and shutdown closing an open capture. Each capture is driven to a real file and decoded back
// with a compact reference reader written to docs/trace-format.md, so a round trip is proved end to
// end, not merely that a request returned.
//
// The behavioural cases run a live Profiler and so are gated on VE_PROFILE; the path-resolution case
// is defined unconditionally (ResolveCapturePath is path math, present under either gate).

#include <doctest/doctest.h>

#include <Veng/Diagnostics/Profiler.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "Diagnostics/TraceFile.h"
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Diagnostics;

namespace
{
    std::vector<u8> ReadFile(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<u8>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    }

    // A compact reference reader: the sections capture control's tests assert on, decoded strictly
    // per docs/trace-format.md (chunk records themselves are skipped via the section length prefix).
    struct Decoded
    {
        bool Complete = false;
        bool Truncated = false;
        TraceFileFormat::CaptureMode Mode = TraceFileFormat::CaptureMode::Triggered;
        std::vector<std::string> Strings; // id 1 at index 0
        bool AnyFullTable = false;
        std::vector<std::pair<ThreadId, u64>> Chunks; // (thread, sequence) in file order
        std::optional<std::pair<u64, u64>> Accounting;

        [[nodiscard]] bool HasString(std::string_view text) const
        {
            for (const std::string& s : Strings)
            {
                if (s == text)
                {
                    return true;
                }
            }
            return false;
        }
    };

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

    Decoded Decode(std::span<const u8> data)
    {
        using namespace TraceFileFormat;
        Decoded out;
        if (data.size() < PreambleSize || std::memcmp(data.data(), Magic, sizeof(Magic)) != 0)
        {
            out.Truncated = true;
            return out;
        }
        usize o = sizeof(Magic);
        const u32 version = ReadU32(data, o);
        if (version != FormatVersion)
        {
            return out;
        }
        const u32 preambleSize = ReadU32(data, o);
        o += 16; // tick frequency + base
        out.Mode = static_cast<CaptureMode>(data[o]);
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
            case SectionType::StringTable:
            {
                usize po = 0;
                const bool isFull = payload[po++] != 0;
                out.AnyFullTable = out.AnyFullTable || isFull;
                u64 firstId = 0;
                REQUIRE(ReadVarint(payload, po, firstId));
                u64 count = 0;
                REQUIRE(ReadVarint(payload, po, count));
                for (u64 i = 0; i < count; ++i)
                {
                    u64 len = 0;
                    REQUIRE(ReadVarint(payload, po, len));
                    const auto id = static_cast<usize>(firstId + i);
                    if (out.Strings.size() < id)
                    {
                        out.Strings.resize(id);
                    }
                    out.Strings[id - 1] =
                        std::string(reinterpret_cast<const char*>(payload.data() + po), len);
                    po += len;
                }
                break;
            }
            case SectionType::Chunk:
            {
                usize po = 0;
                u64 thread = 0;
                u64 sequence = 0;
                REQUIRE(ReadVarint(payload, po, thread));
                REQUIRE(ReadVarint(payload, po, sequence));
                out.Chunks.emplace_back(static_cast<ThreadId>(thread), sequence);
                break;
            }
            case SectionType::Accounting:
            {
                usize po = 0;
                u64 events = 0;
                u64 threads = 0;
                REQUIRE(ReadVarint(payload, po, events));
                REQUIRE(ReadVarint(payload, po, threads));
                out.Accounting = std::pair<u64, u64>(events, threads);
                break;
            }
            case SectionType::Trailer:
                out.Complete = true;
                return out;
            default:
                break;
            }
        }
        return out;
    }

#if defined(VE_PROFILE) && VE_PROFILE
    // A fresh, uncontended scratch directory per case, so the retention scan sees only this case's
    // captures.
    std::filesystem::path FreshDir(std::string_view name)
    {
        static std::atomic<u32> counter{0};
        const std::filesystem::path dir =
            Veng::TestSupport::TempDir() / "capture" /
            (std::string(name) + std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // EndCapture/DumpRing finish the file off the calling thread, so a reader waits on the state's
    // WriterDraining flag rather than on a frame.
    void WaitDrain(Profiler& profiler)
    {
        for (int i = 0; i < 500; ++i)
        {
            if (!profiler.GetState().WriterDraining)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
#endif
}

TEST_CASE(
    "capture directory: ResolveCapturePath places a named capture under the capture directory")
{
    const path dir = CaptureDirectory();
    CHECK_FALSE(dir.empty());

    const path resolved = ResolveCapturePath("myrun");
    CHECK(resolved.parent_path() == dir);
    CHECK(resolved.extension() == ".vtrace");
    CHECK(resolved.stem() == "myrun");

    // A name carrying separators cannot escape the capture directory: only its final component is taken.
    const path escaped = ResolveCapturePath("../../etc/passwd");
    CHECK(escaped.parent_path() == dir);
    CHECK(escaped.filename() == "passwd.vtrace");
}

#if defined(VE_PROFILE) && VE_PROFILE

TEST_CASE("capture: BeginCapture then EndCapture round-trips through the reader")
{
    const std::filesystem::path dir = FreshDir("basic");
    const path out = dir / "basic.vtrace";

    Profiler profiler;
    REQUIRE(profiler.BeginCapture(out));
    CHECK(profiler.GetState().Status == CaptureStatus::Capturing);
    CHECK(profiler.GetState().Path == out);

    for (int i = 0; i < 3; ++i)
    {
        profiler.BeginFrame();
        {
            VE_PROFILE_SCOPE("RoundTrip");
        }
    }

    const Result<path> ended = profiler.EndCapture();
    REQUIRE(ended);
    CHECK(ended.value() == out);
    // With no ring standing, the profiler falls back to Off once the capture ends.
    CHECK(profiler.GetState().Status == CaptureStatus::Off);

    WaitDrain(profiler);
    CHECK_FALSE(profiler.GetState().WriterDraining);

    const std::vector<u8> bytes = ReadFile(out);
    REQUIRE_FALSE(bytes.empty());
    const Decoded d = Decode(bytes);
    CHECK(d.Complete);
    CHECK_FALSE(d.Truncated);
    CHECK(d.Mode == TraceFileFormat::CaptureMode::Triggered);
    // The scope name interned during the live capture resolves in the written string table.
    CHECK(d.HasString("RoundTrip"));
}

TEST_CASE("capture: a frame-count capture self-terminates at exactly N frames on a boundary")
{
    const std::filesystem::path dir = FreshDir("selfterm");
    const path out = dir / "selfterm.vtrace";

    Profiler profiler;
    REQUIRE(profiler.BeginCapture(out, /*frameCount=*/3));
    CHECK(profiler.GetState().FrameBudget == 3);

    // Two frames in: still capturing.
    for (int i = 0; i < 2; ++i)
    {
        {
            VE_PROFILE_SCOPE("SelfTerm");
        }
        profiler.BeginFrame();
    }
    CHECK(profiler.GetState().Status == CaptureStatus::Capturing);

    // The third frame boundary self-terminates the capture — not mid-frame, and no later.
    {
        VE_PROFILE_SCOPE("SelfTerm");
    }
    profiler.BeginFrame();
    CHECK(profiler.GetState().Status != CaptureStatus::Capturing);

    WaitDrain(profiler);
    const std::vector<u8> bytes = ReadFile(out);
    REQUIRE_FALSE(bytes.empty());
    const Decoded d = Decode(bytes);
    CHECK(d.Complete);
    CHECK(d.HasString("SelfTerm"));
}

TEST_CASE("capture: the overlap matrix each returns a located error and leaves state unchanged")
{
    const std::filesystem::path dir = FreshDir("overlap");

    // EndCapture with none running is an error, not an assert.
    {
        Profiler profiler;
        const Result<path> ended = profiler.EndCapture();
        CHECK_FALSE(ended);
        CHECK(profiler.GetState().Status == CaptureStatus::Off);
    }

    // BeginCapture during a capture, and DumpRing during a capture, each fail without disturbing it.
    {
        Profiler profiler;
        const path first = dir / "first.vtrace";
        REQUIRE(profiler.BeginCapture(first));

        const Result<void> begun = profiler.BeginCapture(dir / "second.vtrace");
        CHECK_FALSE(begun);
        CHECK(profiler.GetState().Status == CaptureStatus::Capturing);
        CHECK(profiler.GetState().Path == first);

        const Result<path> dumped = profiler.DumpRing(dir / "dump.vtrace");
        CHECK_FALSE(dumped);
        CHECK(profiler.GetState().Status == CaptureStatus::Capturing);
        CHECK(profiler.GetState().Path == first);

        REQUIRE(profiler.EndCapture());
        WaitDrain(profiler);
        // The second and dump files were never created.
        CHECK_FALSE(std::filesystem::exists(dir / "second.vtrace"));
        CHECK_FALSE(std::filesystem::exists(dir / "dump.vtrace"));
    }
}

TEST_CASE("capture: the standing ring policy is restored after a capture ends")
{
    const std::filesystem::path dir = FreshDir("ringrestore");

    Profiler profiler;
    profiler.SetRingEnabled(true);
    CHECK(profiler.GetState().Status == CaptureStatus::Ring);

    REQUIRE(profiler.BeginCapture(dir / "over-ring.vtrace"));
    CHECK(profiler.GetState().Status == CaptureStatus::Capturing);

    REQUIRE(profiler.EndCapture());
    // A capture is a temporary override: the ring resumes once it ends.
    CHECK(profiler.GetState().Status == CaptureStatus::Ring);
    WaitDrain(profiler);
}

TEST_CASE(
    "capture: a ring dump carries whole chunks in sequence order with the drop count and table")
{
    const std::filesystem::path dir = FreshDir("ringdump");

    ProfilerConfig config;
    config.ChunkBytes = 256;    // ~7 records per chunk
    config.ChunksPerThread = 2; // a shallow ring, so wrapping discards early
    Profiler profiler(config);
    profiler.SetRingEnabled(true);

    // Emit well past the ring's depth, so it wraps many times and discards whole chunks.
    for (int f = 0; f < 50; ++f)
    {
        profiler.BeginFrame();
        for (int i = 0; i < 10; ++i)
        {
            VE_PROFILE_SCOPE("RingScope");
        }
    }
    CHECK(profiler.GetDroppedEventCount() > 0);

    const path out = dir / "ring.vtrace";
    const Result<path> dumped = profiler.DumpRing(out);
    REQUIRE(dumped);
    CHECK(dumped.value() == out);
    // The ring resumes after the dump — it was frozen, not stopped.
    CHECK(profiler.GetState().Status == CaptureStatus::Ring);

    WaitDrain(profiler);
    const std::vector<u8> bytes = ReadFile(out);
    REQUIRE_FALSE(bytes.empty());
    const Decoded d = Decode(bytes);
    CHECK(d.Complete);
    CHECK(d.Mode == TraceFileFormat::CaptureMode::RingDump);
    CHECK(d.AnyFullTable);           // a dump writes the full table, not a delta
    CHECK(d.HasString("RingScope")); // interned in a since-discarded chunk, still resolved
    REQUIRE_FALSE(d.Chunks.empty());

    // Whole chunks in sequence order from the oldest live one: the first live chunk is not sequence 0
    // (older chunks were discarded), and each thread's chunks ascend.
    u64 minSequence = ~0ull;
    for (const auto& [thread, sequence] : d.Chunks)
    {
        minSequence = std::min(minSequence, sequence);
    }
    CHECK(minSequence > 0);
    // Per producing thread, sequences are strictly ascending.
    for (usize i = 1; i < d.Chunks.size(); ++i)
    {
        if (d.Chunks[i].first == d.Chunks[i - 1].first)
        {
            CHECK(d.Chunks[i].second > d.Chunks[i - 1].second);
        }
    }

    REQUIRE(d.Accounting.has_value());
    CHECK(d.Accounting->first > 0); // the discarded-event count travels in the file
}

TEST_CASE("capture: the retention cap deletes oldest-first once the directory exceeds it")
{
    const std::filesystem::path dir = FreshDir("retention");

    ProfilerConfig config;
    config.RetainedCaptureCap = 1;
    Profiler profiler(config);

    const path a = dir / "a.vtrace";
    REQUIRE(profiler.BeginCapture(a));
    {
        VE_PROFILE_SCOPE("Retain");
    }
    REQUIRE(profiler.EndCapture());
    WaitDrain(profiler);
    REQUIRE(std::filesystem::exists(a));
    // Pin a's mtime firmly in the past so it is unambiguously the oldest regardless of FS resolution.
    std::error_code ec;
    std::filesystem::last_write_time(
        a, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1), ec);

    const path b = dir / "b.vtrace";
    REQUIRE(profiler.BeginCapture(b));
    {
        VE_PROFILE_SCOPE("Retain");
    }
    REQUIRE(profiler.EndCapture());
    WaitDrain(profiler); // reaps the drained sink, which enforces the cap

    // Two files, cap of one: the oldest is deleted, the newest kept.
    CHECK_FALSE(std::filesystem::exists(a));
    CHECK(std::filesystem::exists(b));
}

TEST_CASE("capture: destruction closes an open capture into a trailered file")
{
    const std::filesystem::path dir = FreshDir("destruct");
    const path out = dir / "destruct.vtrace";

    {
        Profiler profiler;
        REQUIRE(profiler.BeginCapture(out));
        {
            VE_PROFILE_SCOPE("Destruct");
        }
        profiler.BeginFrame();
        // No EndCapture: the destructor flushes the outstanding chunks and closes the sink cleanly.
    }

    const std::vector<u8> bytes = ReadFile(out);
    REQUIRE_FALSE(bytes.empty());
    const Decoded d = Decode(bytes);
    CHECK(d.Complete); // trailered, not a truncation a reader must special-case
    CHECK(d.HasString("Destruct"));
}

TEST_CASE("capture: an unwritable path fails as a Result rather than aborting")
{
    const std::filesystem::path dir = FreshDir("unwritable");
    // A regular file where a directory component is expected makes the capture directory uncreatable.
    const path blocker = dir / "blocker";
    {
        std::ofstream(blocker) << "x";
    }

    Profiler profiler;
    const Result<void> begun = profiler.BeginCapture(blocker / "nested" / "x.vtrace");
    CHECK_FALSE(begun);
    CHECK(profiler.GetState().Status == CaptureStatus::Off);
}

#endif // VE_PROFILE
