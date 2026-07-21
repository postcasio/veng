// Diagnostics profiler unit cases: the scope/counter/instant vocabulary, per-thread chunk rings and
// their drain behaviours, string interning, virtual tracks, RAII thread registration, and the
// always-on per-frame aggregates. A pure-CPU subsystem — no GPU, no Context. The whole file compiles
// under VE_PROFILE=OFF too, where it asserts the lifecycle surface is inert; the recording cases are
// gated on VE_PROFILE so the macros' zero-expansion is itself exercised by the OFF build.

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <thread>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Diagnostics/TraceSink.h>

#include "Diagnostics/TraceFormat.h"

using namespace Veng;
using namespace Veng::Diagnostics;

#if defined(VE_PROFILE) && VE_PROFILE
#define VE_PROFILE_TEST_ON 1
#else
#define VE_PROFILE_TEST_ON 0
#endif

#if VE_PROFILE_TEST_ON

namespace
{
    using TraceFormat::ChunkHeader;
    using TraceFormat::EventRecord;
    using TraceFormat::RecordType;

    struct DecodedRecord
    {
        RecordType Type = RecordType::Instant;
        ThreadId Thread = 0;
        u32 Track = 0;
        NameId Name = 0;
        u32 Frame = 0;
        u64 BeginAbs = 0;
        u64 EndAbs = 0;
        f64 Value = 0.0;
        u64 Base = 0;
    };

    // Decodes one captured chunk from its own header, reconstructing absolute timestamps from the
    // chunk's base — the "self-decodable from its own base" property in the flesh.
    vector<DecodedRecord> DecodeChunk(const CapturingTestSink::CapturedChunk& chunk)
    {
        vector<DecodedRecord> out;
        REQUIRE(chunk.Bytes.size() >= sizeof(ChunkHeader));
        ChunkHeader header;
        std::memcpy(&header, chunk.Bytes.data(), sizeof(header));
        CHECK(header.FirstRecordOffset == sizeof(ChunkHeader));

        usize offset = header.FirstRecordOffset;
        const usize end = static_cast<usize>(header.FirstRecordOffset) + header.RecordBytes;
        REQUIRE(end <= chunk.Bytes.size());
        while (offset + sizeof(EventRecord) <= end)
        {
            EventRecord record;
            std::memcpy(&record, chunk.Bytes.data() + offset, sizeof(record));
            DecodedRecord decoded;
            decoded.Type = static_cast<RecordType>(record.Type);
            decoded.Thread = chunk.Thread;
            decoded.Track = record.Track;
            decoded.Name = record.Name;
            decoded.Frame = record.Frame;
            decoded.Base = header.TimestampBase;
            decoded.BeginAbs = header.TimestampBase + record.BeginDelta;
            if (decoded.Type == RecordType::ScopeComplete)
            {
                decoded.EndAbs = header.TimestampBase + record.EndOrValue;
            }
            else if (decoded.Type == RecordType::Counter)
            {
                std::memcpy(&decoded.Value, &record.EndOrValue, sizeof(decoded.Value));
            }
            out.push_back(decoded);
            offset += sizeof(EventRecord);
        }
        return out;
    }

    vector<DecodedRecord> DecodeAll(const CapturingTestSink& sink)
    {
        vector<DecodedRecord> out;
        for (const auto& chunk : sink.GetChunks())
        {
            const vector<DecodedRecord> records = DecodeChunk(chunk);
            out.insert(out.end(), records.begin(), records.end());
        }
        return out;
    }

    // A little busywork so a scope has a non-trivial, non-zero duration to record.
    u64 Spin(u32 iterations)
    {
        u64 acc = 0;
        for (u32 i = 0; i < iterations; ++i)
        {
            acc += i * 2654435761u;
        }
        return acc;
    }
}

TEST_CASE("Scope nesting balances and self-time subtracts children")
{
    Profiler profiler; // Off mode, null sink: aggregation still accumulates.

    volatile u64 sink = 0;
    {
        VE_PROFILE_SCOPE("Outer");
        sink += Spin(2000);
        {
            VE_PROFILE_SCOPE("Inner");
            sink += Spin(4000);
        }
    }
    profiler.BeginFrame(); // fold frame 0

    const NameId outer = profiler.InternName("Outer");
    const NameId inner = profiler.InternName("Inner");
    const optional<ScopeAggregate> outerAgg = profiler.GetScopeAggregate(outer);
    const optional<ScopeAggregate> innerAgg = profiler.GetScopeAggregate(inner);

    REQUIRE(outerAgg.has_value());
    REQUIRE(innerAgg.has_value());
    CHECK(outerAgg->CallCount == 1);
    CHECK(innerAgg->CallCount == 1);
    // Inclusive nests: the outer contains the inner.
    CHECK(outerAgg->InclusiveNanos >= innerAgg->InclusiveNanos);
    // Self never exceeds inclusive, and the outer's self excludes the inner's inclusive.
    CHECK(outerAgg->SelfNanos <= outerAgg->InclusiveNanos);
    CHECK(outerAgg->SelfNanos + innerAgg->InclusiveNanos <= outerAgg->InclusiveNanos + 1);
}

TEST_CASE("String interning: stable id for one literal, distinct ids for distinct contents")
{
    Profiler profiler;

    const NameId a1 = profiler.InternName("Foo");
    const NameId a2 = profiler.InternName("Foo");
    const NameId b = profiler.InternName("Bar");

    CHECK(a1 != 0);
    CHECK(a1 == a2);
    CHECK(a1 != b);
    CHECK(profiler.GetName(a1) == "Foo");
    CHECK(profiler.GetName(b) == "Bar");
}

TEST_CASE("Counter and instant events round-trip through the test sink")
{
    CapturingTestSink sink;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        VE_PROFILE_COUNTER("QueueDepth", 42.0);
        VE_PROFILE_INSTANT("Hitch");
    } // destructor flushes the buffered chunk and closes the sink

    const vector<DecodedRecord> records = DecodeAll(sink);
    bool sawCounter = false;
    bool sawInstant = false;
    for (const DecodedRecord& record : records)
    {
        if (record.Type == RecordType::Counter && sink.GetString(record.Name) == "QueueDepth")
        {
            sawCounter = true;
            CHECK(record.Value == doctest::Approx(42.0));
        }
        if (record.Type == RecordType::Instant && sink.GetString(record.Name) == "Hitch")
        {
            sawInstant = true;
        }
    }
    CHECK(sawCounter);
    CHECK(sawInstant);
    CHECK(sink.GetCloseCount() == 1);
}

TEST_CASE("Nested scopes round-trip as ordered complete records")
{
    CapturingTestSink sink;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        VE_PROFILE_SCOPE("Outer");
        Spin(1000);
        {
            VE_PROFILE_SCOPE("Inner");
            Spin(1000);
        }
    }

    const vector<DecodedRecord> records = DecodeAll(sink);
    const DecodedRecord* outer = nullptr;
    const DecodedRecord* inner = nullptr;
    for (const DecodedRecord& record : records)
    {
        if (sink.GetString(record.Name) == "Outer")
        {
            outer = &record;
        }
        if (sink.GetString(record.Name) == "Inner")
        {
            inner = &record;
        }
    }
    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);
    // Inner is bracketed by outer.
    CHECK(inner->BeginAbs >= outer->BeginAbs);
    CHECK(inner->EndAbs <= outer->EndAbs);
    CHECK(outer->Track == 0);
    CHECK(inner->Track == 0);
}

TEST_CASE("Emit-to-track lands a span on a virtual track, not the caller's thread track")
{
    CapturingTestSink sink;
    TrackId gpu = 0;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        gpu = profiler.CreateTrack("GPU", TrackRole::Gpu);
        CHECK(gpu != 0);
        const NameId pass = profiler.InternName("ShadowPass");
        const u64 begin = NowNanos();
        profiler.EmitScope(gpu, pass, begin, begin + 1000);
    }

    const vector<DecodedRecord> records = DecodeAll(sink);
    bool sawTrackSpan = false;
    for (const DecodedRecord& record : records)
    {
        if (record.Type == RecordType::ScopeComplete && sink.GetString(record.Name) == "ShadowPass")
        {
            sawTrackSpan = true;
            CHECK(record.Track == gpu);
            CHECK(record.Track != 0);
        }
    }
    CHECK(sawTrackSpan);
}

TEST_CASE("A filled buffer hands exactly one chunk to the sink with no event loss")
{
    CapturingTestSink sink;
    ProfilerConfig config;
    config.ChunkBytes = sizeof(ChunkHeader) + 4 * TraceFormat::RecordStride; // 4 records per chunk
    config.ChunksPerThread = 2;

    {
        Profiler profiler(config);
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        for (u32 i = 0; i < 5; ++i)
        {
            VE_PROFILE_SCOPE("Loop");
        }

        // The fifth scope overflowed the chunk, streaming exactly one full chunk out.
        CHECK(sink.GetChunks().size() == 1);
        CHECK(DecodeChunk(sink.GetChunks().front()).size() == 4);
    } // flush the remaining record

    // No loss: all five scopes are accounted for across the streamed and flushed chunks.
    const vector<DecodedRecord> records = DecodeAll(sink);
    usize loopRecords = 0;
    for (const DecodedRecord& record : records)
    {
        if (sink.GetString(record.Name) == "Loop")
        {
            ++loopRecords;
        }
    }
    CHECK(loopRecords == 5);
}

TEST_CASE("Ring wrap discards a whole chunk, counts the drop, and leaves survivors self-decodable")
{
    ProfilerConfig config;
    config.ChunkBytes = sizeof(ChunkHeader) + 4 * TraceFormat::RecordStride; // 4 records per chunk
    config.ChunksPerThread = 2;

    CapturingTestSink sink;
    {
        Profiler profiler(config);
        profiler.SetMode(
            ProfilerMode::Ring); // null sink: the ring retains and wraps rather than streams

        for (u32 i = 0; i < 9; ++i)
        {
            VE_PROFILE_SCOPE("Ring");
        }

        // Two chunks of 4 each filled; the ninth wrapped onto the oldest, discarding it whole.
        CHECK(profiler.GetDroppedEventCount() == 4);

        // Attaching a sink now and destroying drains the surviving ring chunks.
        profiler.SetSink(&sink);
    }

    const vector<DecodedRecord> records = DecodeAll(sink);
    usize survivors = 0;
    for (const DecodedRecord& record : records)
    {
        if (sink.GetString(record.Name) == "Ring")
        {
            ++survivors;
            // Each survivor decoded from its own chunk base, so its absolute time is at or after it.
            CHECK(record.BeginAbs >= record.Base);
        }
    }
    // Nine emitted, four discarded: five survive.
    CHECK(survivors == 5);
}

TEST_CASE("Two threads produce separable tracks")
{
    CapturingTestSink sink;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        {
            VE_PROFILE_SCOPE("MainWork");
        }

        std::thread worker(
            [&profiler]
            {
                const ProfilerThreadRegistration registration = profiler.RegisterThread("Worker");
                VE_PROFILE_SCOPE("WorkerWork");
            });
        worker.join(); // the registration's destructor flushed the worker's chunk on the way out
    }

    const vector<DecodedRecord> records = DecodeAll(sink);
    ThreadId mainThread = 0;
    ThreadId workerThread = 0;
    for (const DecodedRecord& record : records)
    {
        if (sink.GetString(record.Name) == "MainWork")
        {
            mainThread = record.Thread;
        }
        if (sink.GetString(record.Name) == "WorkerWork")
        {
            workerThread = record.Thread;
        }
    }
    CHECK(mainThread != 0);
    CHECK(workerThread != 0);
    CHECK(mainThread != workerThread);
}

TEST_CASE("A registration destroyed on a short-lived thread leaves no registry entry behind")
{
    ProfilerConfig config;
    config.MaxThreads = 2; // the main thread plus exactly one worker slot

    Profiler profiler(config);

    // Each worker takes the one worker slot and must free it on exit, or the next would overflow.
    for (u32 i = 0; i < 6; ++i)
    {
        std::thread worker(
            [&profiler]
            {
                const ProfilerThreadRegistration registration =
                    profiler.RegisterThread("Transient");
                VE_PROFILE_SCOPE("TransientWork");
            });
        worker.join();
    }

    // A lazily attached thread (no explicit registration) must also unlink on exit.
    for (u32 i = 0; i < 6; ++i)
    {
        std::thread worker([] { VE_PROFILE_SCOPE("LazyWork"); });
        worker.join();
    }

    CHECK(profiler.GetRegistrationOverflowCount() == 0);
}

TEST_CASE("Registering beyond the configured maximum is accounted, not fatal")
{
    ProfilerConfig config;
    config.MaxThreads = 1; // only the auto-registered main thread fits

    Profiler profiler(config);
    const ProfilerThreadRegistration overflow = profiler.RegisterThread("TooMany");

    CHECK(overflow.GetThreadId() == 0); // inert registration
    CHECK(profiler.GetRegistrationOverflowCount() == 1);
}

TEST_CASE("Per-frame aggregates accumulate with a null sink and no capture in progress")
{
    Profiler profiler; // Off mode, null sink — the default resting state
    REQUIRE(profiler.IsRecording() == false);

    for (u32 frame = 0; frame < 3; ++frame)
    {
        for (u32 i = 0; i < 5; ++i)
        {
            VE_PROFILE_SCOPE("Phase");
        }
        profiler.BeginFrame();
    }

    const NameId phase = profiler.InternName("Phase");
    const optional<ScopeAggregate> aggregate = profiler.GetScopeAggregate(phase);
    REQUIRE(aggregate.has_value());
    // The last completed frame ran the scope five times, even though nothing was ever captured.
    CHECK(aggregate->CallCount == 5);
}

TEST_CASE("An intermittent scope stays visible with a zero count, distinct from cost-free")
{
    Profiler profiler;

    // Frame 0 runs the scope; frame 1 does not.
    {
        VE_PROFILE_SCOPE("Checkpoint");
    }
    profiler.BeginFrame(); // fold frame 0 (the scope ran)
    const u64 ranFrame = profiler.GetFrameIndex() - 1;
    profiler.BeginFrame(); // fold frame 1 (the scope did not run)

    const NameId checkpoint = profiler.InternName("Checkpoint");
    const optional<ScopeAggregate> aggregate = profiler.GetScopeAggregate(checkpoint);
    REQUIRE(aggregate.has_value());
    CHECK(aggregate->CallCount == 0);              // did not run in the last completed frame
    CHECK(aggregate->LastActiveFrame == ranFrame); // but the frame it last ran in is carried
}

TEST_CASE("Recording and concurrent aggregate reads stay consistent under a producer thread")
{
    CapturingTestSink sink;
    constexpr u32 Count = 20000;
    {
        Profiler profiler;
        profiler.SetMode(ProfilerMode::Ring);

        std::atomic<bool> producing{true};
        std::thread producer(
            [&profiler, &sink, &producing]
            {
                const ProfilerThreadRegistration registration = profiler.RegisterThread("Producer");
                // Stream to the sink from the producer thread only, so the sink has a single writer.
                profiler.SetSink(&sink);
                for (u32 i = 0; i < Count; ++i)
                {
                    VE_PROFILE_SCOPE("Job");
                }
                producing.store(false);
            });

        // Concurrently fold frames and read aggregates on the main thread while the producer records.
        while (producing.load())
        {
            profiler.BeginFrame();
            const vector<ScopeAggregate> snapshot = profiler.GetFrameAggregates();
            (void)snapshot;
        }
        producer.join();
    }

    // Every recorded job survives streaming with no torn record (each decodes to a valid name).
    const vector<DecodedRecord> records = DecodeAll(sink);
    usize jobRecords = 0;
    for (const DecodedRecord& record : records)
    {
        if (sink.GetString(record.Name) == "Job")
        {
            ++jobRecords;
        }
    }
    CHECK(jobRecords == Count);
}

#endif // VE_PROFILE_TEST_ON

TEST_CASE("Under VE_PROFILE the lifecycle surface is present; without it, inert")
{
    Profiler profiler;
    profiler.SetMode(ProfilerMode::Ring);

    // Every macro compiles in both configurations; under VE_PROFILE=OFF each expands to nothing.
    VE_PROFILE_FRAME();
    {
        VE_PROFILE_SCOPE("Lifecycle");
        VE_PROFILE_FUNCTION();
        VE_PROFILE_SCOPE_DYNAMIC(string("Dynamic"));
        VE_PROFILE_COUNTER("Series", 1.0);
        VE_PROFILE_INSTANT("Point");
        VE_PROFILE_THREAD("SelfNamed");
    }

#if VE_PROFILE_TEST_ON
    CHECK(profiler.IsRecording() == true);
    CHECK(profiler.InternName("Lifecycle") != 0);
#else
    // The gate off: the whole surface is a documented no-op holding no recording state.
    CHECK(profiler.IsRecording() == false);
    CHECK(profiler.InternName("Lifecycle") == 0);
    CHECK(profiler.GetFrameAggregates().empty());
    CHECK(profiler.GetSink() == nullptr);
#endif
}
