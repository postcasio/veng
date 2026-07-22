// Plan-02 spine instrumentation unit cases: the per-system simulation scope and its
// stable interned name, the task pool's counters and non-asserting worker query, the
// track-descriptor seam (thread and virtual track names/roles reaching the sink), and the
// GPU bridge's frame-index back-dating over a fake timing source. Pure CPU — no Context,
// no Vulkan symbol. Recording cases are gated on VE_PROFILE so the OFF build still compiles
// this file and exercises the macros' zero-expansion.

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Diagnostics/TraceSink.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Task/TaskSystem.h>

#include "Diagnostics/TraceFormat.h"

using namespace Veng;
using namespace Veng::Diagnostics;

namespace
{
    // Two Sim systems and one View system, each with a distinct registered name, so the
    // per-system scope records can be matched to their systems by interned name.
    struct AlphaSystem final : SceneSystem
    {
        void OnUpdate(Scene&, f32, const SystemContext&) override {}
    };
    struct BetaSystem final : SceneSystem
    {
        void OnUpdate(Scene&, f32, const SystemContext&) override {}
    };
    struct GammaViewSystem final : SceneSystem
    {
        Phase GetPhase() const override { return Phase::View; }
        void OnUpdate(Scene&, f32, const SystemContext&) override {}
    };
}

namespace Veng
{
    template <>
    struct VengSystem<AlphaSystem>
    {
        static constexpr SystemId Id = 0x51020000000000A1ULL;
        static string Name() { return "AlphaSystem"; }
    };
    template <>
    struct VengSystem<BetaSystem>
    {
        static constexpr SystemId Id = 0x51020000000000B2ULL;
        static string Name() { return "BetaSystem"; }
    };
    template <>
    struct VengSystem<GammaViewSystem>
    {
        static constexpr SystemId Id = 0x51020000000000C3ULL;
        static string Name() { return "GammaViewSystem"; }
    };
}

#if defined(VE_PROFILE) && VE_PROFILE

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
    };

    vector<DecodedRecord> DecodeAll(const CapturingTestSink& sink)
    {
        vector<DecodedRecord> out;
        for (const auto& chunk : sink.GetChunks())
        {
            REQUIRE(chunk.Bytes.size() >= sizeof(ChunkHeader));
            ChunkHeader header;
            std::memcpy(&header, chunk.Bytes.data(), sizeof(header));
            usize offset = header.FirstRecordOffset;
            const usize end = static_cast<usize>(header.FirstRecordOffset) + header.RecordBytes;
            while (offset + sizeof(EventRecord) <= end)
            {
                EventRecord record;
                std::memcpy(&record, chunk.Bytes.data() + offset, sizeof(record));
                offset += sizeof(EventRecord);
                DecodedRecord decoded;
                decoded.Type = static_cast<RecordType>(record.Type);
                decoded.Thread = chunk.Thread;
                decoded.Track = record.Track;
                decoded.Name = record.Name;
                decoded.Frame = record.Frame;
                decoded.BeginAbs = header.TimestampBase + record.BeginDelta;
                if (decoded.Type == RecordType::ScopeComplete)
                {
                    decoded.EndAbs = header.TimestampBase + record.EndOrValue;
                }
                out.push_back(decoded);
            }
        }
        return out;
    }

    // A SystemContext the driver forwards but no test system dereferences.
    struct ContextStorage
    {
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
            };
        }
    };
}

TEST_CASE("Per-system scope records the registered name, stable across frames")
{
    TypeRegistry types;
    SystemRegistry registry;
    registry.Register<AlphaSystem>();
    registry.Register<BetaSystem>();
    registry.Register<GammaViewSystem>();

    CapturingTestSink sink;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        // The simulation interns its systems' names at construction against the active profiler.
        SceneSimulation sim(registry);
        const Unique<Scene> scene = Scene::Create(types);
        ContextStorage ctx;

        for (int frame = 0; frame < 3; ++frame)
        {
            profiler.BeginFrame();
            sim.UpdatePhase(*scene, SceneSystem::Phase::Sim, 0.016f, ctx.Make());
        }
        profiler.BeginFrame();
    } // flush

    const vector<DecodedRecord> records = DecodeAll(sink);

    // The Sim phase runs Alpha then Beta each frame; Gamma is a View system and must not appear.
    NameId alphaId = 0;
    NameId betaId = 0;
    int alphaCount = 0;
    int betaCount = 0;
    bool sawGamma = false;
    for (const DecodedRecord& record : records)
    {
        if (record.Type != RecordType::ScopeComplete)
        {
            continue;
        }
        const string_view name = sink.GetString(record.Name);
        if (name == "AlphaSystem")
        {
            ++alphaCount;
            if (alphaId == 0)
            {
                alphaId = record.Name;
            }
            // Stable interned id: every frame's Alpha scope carries the same name id.
            CHECK(record.Name == alphaId);
        }
        else if (name == "BetaSystem")
        {
            ++betaCount;
            if (betaId == 0)
            {
                betaId = record.Name;
            }
            CHECK(record.Name == betaId);
        }
        else if (name == "GammaViewSystem")
        {
            sawGamma = true;
        }
    }

    CHECK(alphaId != 0);
    CHECK(betaId != 0);
    CHECK(alphaId != betaId);
    CHECK(alphaCount == 3); // one per system per Sim step, three steps
    CHECK(betaCount == 3);
    CHECK_FALSE(sawGamma); // a View system is not run by the Sim phase
}

TEST_CASE("Task pool counters move as jobs queue and drain; worker query is non-asserting")
{
    // The non-asserting worker query returns the sentinel off a worker rather than aborting.
    CHECK(TaskSystem::TryGetCurrentWorkerIndex() == TaskSystem::NotAWorker);

    Profiler profiler; // active before the pool starts, so workers register named tracks

    CapturingTestSink sink;
    profiler.SetSink(&sink);
    profiler.SetMode(ProfilerMode::Ring);

    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 2});

    std::atomic<bool> release{false};
    std::atomic<u32> started{0};

    // Two jobs that block until released, so the counters are observable mid-flight.
    Task<void> a = tasks.Submit(
        [&]
        {
            started.fetch_add(1);
            while (!release.load())
            {
                std::this_thread::yield();
            }
        },
        "BlockingJob");
    Task<void> b = tasks.Submit(
        [&]
        {
            started.fetch_add(1);
            while (!release.load())
            {
                std::this_thread::yield();
            }
        },
        "BlockingJob");

    // Both jobs are active (queued or running) until released.
    CHECK(tasks.GetActiveJobCount() >= 1);

    release.store(true);
    (void)a.Get();
    (void)b.Get();
    tasks.WaitForAll();

    CHECK(tasks.GetActiveJobCount() == 0);
    CHECK(tasks.GetQueueDepth() == 0);
}

TEST_CASE("Named jobs record under their name on a worker track distinct from the main track")
{
    CapturingTestSink sink;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        // A scope on the main thread pins the main track's id.
        {
            VE_PROFILE_SCOPE("MainWork");
        }

        TaskSystem tasks(TaskSystemInfo{.WorkerCount = 1});
        Task<void> job = tasks.Submit([] {}, "DecodeJob");
        (void)job.Get();
        tasks.WaitForAll();
    } // flush

    const vector<DecodedRecord> records = DecodeAll(sink);
    ThreadId mainThread = 0;
    ThreadId jobThread = 0;
    for (const DecodedRecord& record : records)
    {
        if (record.Type != RecordType::ScopeComplete)
        {
            continue;
        }
        const string_view name = sink.GetString(record.Name);
        if (name == "MainWork")
        {
            mainThread = record.Thread;
        }
        else if (name == "DecodeJob")
        {
            jobThread = record.Thread;
        }
    }
    CHECK(mainThread != 0);
    CHECK(jobThread != 0);
    CHECK(jobThread != mainThread); // the job ran on a worker track, not the main track
}

TEST_CASE("EmitScope back-dates a span to the frame that executed it, not the current one")
{
    CapturingTestSink sink;
    u64 executedTicks = 0;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        const TrackId gpu = profiler.CreateTrack("GPU", TrackRole::Gpu);
        const NameId passName = profiler.InternName("GBuffer");

        // Advance several frames, "executing" the GPU work in frame 1 but only reading it back
        // (and emitting it) in frame 4 — the N-frames-late readback the bridge must back-date.
        profiler.BeginFrame(); // -> frame 1
        executedTicks = NowTicks();
        for (int i = 0; i < 4; ++i)
        {
            profiler.BeginFrame();
        }
        const u64 currentFrame = profiler.GetFrameIndex();
        REQUIRE(currentFrame >= 4);

        // Emit the pass back-dated to frame 1 (three nested passes by tick containment).
        profiler.EmitScope(gpu, passName, executedTicks, executedTicks + 1000, 1);
        profiler.EmitScope(gpu, profiler.InternName("Lighting"), executedTicks + 200,
                           executedTicks + 400, 1);
    } // flush

    const vector<DecodedRecord> records = DecodeAll(sink);
    bool sawBackDated = false;
    for (const DecodedRecord& record : records)
    {
        if (record.Type == RecordType::ScopeComplete && sink.GetString(record.Name) == "GBuffer")
        {
            sawBackDated = true;
            CHECK(record.Frame == 1); // the executing frame, not the current one
            CHECK(record.Track != 0); // the virtual GPU track, not the caller's thread track
            CHECK(record.EndAbs > record.BeginAbs);
        }
    }
    CHECK(sawBackDated);
}

TEST_CASE("The GPU bridge shape: fake pass timings land on the GPU track at their executing frame")
{
    CapturingTestSink sink;
    {
        Profiler profiler;
        profiler.SetSink(&sink);
        profiler.SetMode(ProfilerMode::Ring);

        // A fake per-frame pass source, standing in for Context::GetLastGpuPassTimings().
        struct FakePass
        {
            const char* Name;
            u64 BeginNanos;
            u64 EndNanos;
            u32 Depth;
        };
        const FakePass passes[] = {
            {.Name = "GPU Frame", .BeginNanos = 0, .EndNanos = 5000, .Depth = 0},
            {.Name = "GBuffer", .BeginNanos = 100, .EndNanos = 2000, .Depth = 1},
            {.Name = "Lighting", .BeginNanos = 2000, .EndNanos = 4000, .Depth = 1},
        };

        const TrackId gpu = profiler.CreateTrack("GPU", TrackRole::Gpu);
        const u64 frequency = TraceTickFrequency();
        const u64 anchor = NowTicks();

        // Two real frames elapse before the readback of the frame executed at index 2.
        for (int i = 0; i < 4; ++i)
        {
            profiler.BeginFrame();
        }
        constexpr u64 executingFrame = 2;
        for (const FakePass& pass : passes)
        {
            const auto toTicks = [frequency](u64 nanos)
            {
                return static_cast<u64>((static_cast<f64>(nanos) * static_cast<f64>(frequency)) /
                                        1.0e9);
            };
            profiler.EmitScope(gpu, profiler.InternName(pass.Name),
                               anchor + toTicks(pass.BeginNanos), anchor + toTicks(pass.EndNanos),
                               executingFrame);
        }
    } // flush

    const vector<DecodedRecord> records = DecodeAll(sink);
    int gpuScopes = 0;
    for (const DecodedRecord& record : records)
    {
        if (record.Type == RecordType::ScopeComplete && record.Track != 0)
        {
            ++gpuScopes;
            CHECK(record.Frame == 2); // every GPU event stamped with the executing frame
        }
    }
    CHECK(gpuScopes == 3);
}

TEST_CASE("Track descriptors deliver thread and virtual track names and roles to the sink")
{
    CapturingTestSink sink;
    {
        Profiler profiler;
        // Create a virtual GPU track and name a thread before the sink attaches.
        const TrackId gpu = profiler.CreateTrack("GPU", TrackRole::Gpu);
        (void)gpu;

        // Attaching the sink replays every known track's descriptor (the main thread, the GPU track).
        profiler.SetSink(&sink);

        // A track named after the sink attached is delivered live.
        const TrackId custom = profiler.CreateTrack("Audio", TrackRole::Custom);
        (void)custom;
    }

    bool sawMainThread = false;
    bool sawGpu = false;
    bool sawAudio = false;
    for (const CapturingTestSink::CapturedTrack& track : sink.GetTracks())
    {
        if (!track.IsVirtual && track.Name == "Main")
        {
            sawMainThread = true;
            CHECK(track.Role == TrackRole::Cpu);
        }
        if (track.IsVirtual && track.Name == "GPU")
        {
            sawGpu = true;
            CHECK(track.Role == TrackRole::Gpu);
        }
        if (track.IsVirtual && track.Name == "Audio")
        {
            sawAudio = true;
            CHECK(track.Role == TrackRole::Custom);
        }
    }
    CHECK(sawMainThread);
    CHECK(sawGpu);
    CHECK(sawAudio);
}

#else // VE_PROFILE off — the additions still compile and the surface is inert.

TEST_CASE("Off build: the non-asserting worker query is the sentinel and Submit takes a name")
{
    CHECK(TaskSystem::TryGetCurrentWorkerIndex() == TaskSystem::NotAWorker);

    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 1});
    Task<void> job = tasks.Submit([] {}, "NamedJob");
    (void)job.Get();
    CHECK(tasks.GetQueueDepth() == 0);
    CHECK(tasks.GetActiveJobCount() == 0);
}

#endif
