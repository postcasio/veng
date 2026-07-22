#include <Veng/Diagnostics/Profiler.h>

#include <chrono>

// The build tree's capture directory, baked at configure time (`<build-dir>/captures/`). Scoped to
// this TU so it never reaches the public surface; an absent define degrades to a relative default so
// the profiler compiles in a tree that did not set it.
#ifndef VENG_CAPTURE_DIR
#define VENG_CAPTURE_DIR "captures"
#endif

namespace Veng::Diagnostics
{
    path CaptureDirectory()
    {
        return path(VENG_CAPTURE_DIR);
    }

    path ResolveCapturePath(string_view name)
    {
        // Only the final component of the requested name is taken, so a name carrying separators
        // cannot escape the capture directory.
        path leaf = path(string(name)).filename();
        if (leaf.empty())
        {
            leaf = "capture";
        }
        leaf += ".vtrace";
        return CaptureDirectory() / leaf;
    }

    // The trace clock's tick frequency and its tick-to-nanosecond conversion. Defined
    // unconditionally (they carry no recording state) so a decoder or a hand-built span can
    // convert NowTicks() values regardless of the VE_PROFILE gate.
    u64 TraceTickFrequency() noexcept
    {
#if defined(__aarch64__)
        // The architected timer frequency (CNTFRQ_EL0): 24 MHz on Apple Silicon.
        u64 frequency;
        __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
        return frequency;
#else
        using Period = std::chrono::steady_clock::period;
        return static_cast<u64>(Period::den) / static_cast<u64>(Period::num);
#endif
    }

    u64 TraceTicksToNanos(u64 ticks) noexcept
    {
        // Cached once: the frequency is fixed for the process lifetime, and this runs off the
        // per-scope path (at the frame fold and at decode). Split whole seconds from the remainder
        // so the nanosecond multiply stays exact for absolute tick counts without overflowing u64.
        static const u64 frequency = TraceTickFrequency();
        constexpr u64 NanosPerSecond = 1'000'000'000ULL;
        return (ticks / frequency) * NanosPerSecond +
               (ticks % frequency) * NanosPerSecond / frequency;
    }
}

#if defined(VE_PROFILE) && VE_PROFILE

#include <Veng/Diagnostics/FileTraceSink.h>

#include "TraceFormat.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Veng::Diagnostics
{
    using TraceFormat::ChunkHeader;
    using TraceFormat::EventRecord;
    using TraceFormat::RecordStride;
    using TraceFormat::RecordType;

    namespace Detail
    {
        namespace
        {
            /// @brief One entry on a thread's open-scope stack, holding child ticks for self-time.
            struct ScopeStackEntry
            {
                u64 ChildTicks = 0;
            };

            /// @brief A thread's monotonic running totals for one scope name, drained by the frame fold.
            ///
            /// The owner thread only ever adds to the *Total fields (relaxed stores; it is the sole
            /// writer, so no read-modify-write is needed) and never resets them. The fold, on the main
            /// thread, reads them and remembers what it last folded in the *Folded shadow, so each
            /// frame's contribution is the difference — a single-writer/single-reader relationship over
            /// each counter word, with no lock and no reset race. The shadow is folder-only and never
            /// touched on the hot path.
            struct ScopeAccum
            {
                /// @brief Total times entered since attach; owner-written, folder-read.
                std::atomic<u64> CallTotal{0};
                /// @brief Total inclusive ticks since attach; owner-written, folder-read.
                std::atomic<u64> InclusiveTicks{0};
                /// @brief Total self ticks since attach; owner-written, folder-read.
                std::atomic<u64> SelfTicks{0};
                /// @brief Call total the fold last observed.
                u64 CallFolded = 0;
                /// @brief Inclusive-tick total the fold last observed.
                u64 InclusiveFolded = 0;
                /// @brief Self-tick total the fold last observed.
                u64 SelfFolded = 0;
            };

            /// @brief A thread's per-scope accumulators, indexed by NameId, grown without moving.
            ///
            /// The owner thread appends entries (an entry per interned name it records) and the fold
            /// reads them from another thread, so growth must never move a live entry. Accumulators
            /// live in fixed-size, heap-allocated blocks whose pointers never change once placed; the
            /// outer block array is reserved up front so appending a block never reallocates it. The
            /// published Count is released by the owner after a block is in place and acquire-loaded by
            /// the fold, so the fold only ever reads entries the owner has finished publishing.
            struct ScopeAggStore
            {
                /// @brief Accumulators per block; sized so a typical program needs one or two.
                static constexpr u32 BlockSize = 128;

                /// @brief Reserves the block array for up to @p maxIds distinct names.
                void Reserve(u32 maxIds)
                {
                    m_BlockCapacity = (maxIds + BlockSize - 1) / BlockSize + 2;
                    m_Blocks.reserve(m_BlockCapacity);
                }

                /// @brief Returns the accumulator for a name id, placing its block on first touch.
                ///
                /// Owner thread only. Returns nullptr past the reserved ceiling (an accounted drop
                /// rather than a reallocation that would race the fold).
                [[nodiscard]] ScopeAccum* Touch(NameId id)
                {
                    if (id == 0)
                    {
                        return nullptr;
                    }
                    const u32 index = id - 1;
                    const u32 block = index / BlockSize;
                    if (block >= m_BlockCapacity)
                    {
                        return nullptr;
                    }
                    while (block >= m_Blocks.size())
                    {
                        m_Blocks.emplace_back(CreateUnique<ScopeAccum[]>(BlockSize));
                    }
                    if (id > m_LocalCount)
                    {
                        m_LocalCount = id;
                        m_Count.store(id, std::memory_order_release);
                    }
                    return &m_Blocks[block][index % BlockSize];
                }

                /// @brief The number of ids published for the fold to read (acquire).
                [[nodiscard]] u32 Published() const
                {
                    return m_Count.load(std::memory_order_acquire);
                }

                /// @brief Returns the accumulator at a zero-based index below the published Count.
                ///
                /// The caller must have acquire-read Count first and pass an index below it; that
                /// index's block was placed (and its outer slot written) before Count was released, so
                /// the reserved, non-moving block array resolves it without reading the owner-mutated
                /// vector size.
                [[nodiscard]] ScopeAccum* At(u32 index)
                {
                    const u32 block = index / BlockSize;
                    if (block >= m_BlockCapacity)
                    {
                        return nullptr;
                    }
                    return &m_Blocks[block][index % BlockSize];
                }

            private:
                /// @brief Fixed-size accumulator blocks; reserved so appends never move a live entry.
                vector<Unique<ScopeAccum[]>> m_Blocks;
                /// @brief Published id count, released by the owner and acquire-read by the fold.
                std::atomic<u32> m_Count{0};
                /// @brief Owner-side view of the highest id touched.
                u32 m_LocalCount = 0;
                /// @brief Reserved block-array capacity; the hard ceiling Touch will not exceed.
                u32 m_BlockCapacity = 0;
            };

            /// @brief One fixed-size buffer chunk. Non-movable: the published offset is an atomic.
            struct Chunk
            {
                Unique<u8[]> Data;
                u32 Capacity = 0;
                std::atomic<u32> WriteOffset{sizeof(ChunkHeader)};
                // A collector (EndCapture, DumpRing) reads these off the owning thread while it is
                // still recording, so they are atomic like the write offset. Relaxed suffices: Arm's
                // release store to WriteOffset and the collector's acquire load of it order the pair.
                std::atomic<u64> TimestampBase{0};
                std::atomic<u64> SequenceNumber{0};

                /// @brief Re-arms the chunk for reuse with a fresh base and sequence, writing its header.
                void Arm(u64 base, u64 sequence)
                {
                    TimestampBase.store(base, std::memory_order_relaxed);
                    SequenceNumber.store(sequence, std::memory_order_relaxed);
                    ChunkHeader header;
                    header.TimestampBase = base;
                    header.SequenceNumber = sequence;
                    header.FirstRecordOffset = sizeof(ChunkHeader);
                    header.RecordBytes = 0;
                    std::memcpy(Data.get(), &header, sizeof(header));
                    WriteOffset.store(sizeof(ChunkHeader), std::memory_order_release);
                }

                /// @brief Writes the sealed record byte count into the header before hand-off.
                void SealHeader()
                {
                    const u32 offset = WriteOffset.load(std::memory_order_relaxed);
                    const u32 recordBytes = offset - sizeof(ChunkHeader);
                    std::memcpy(Data.get() + offsetof(ChunkHeader, RecordBytes), &recordBytes,
                                sizeof(recordBytes));
                }

                /// @brief Records held in the chunk right now.
                [[nodiscard]] u32 RecordCount() const
                {
                    return (WriteOffset.load(std::memory_order_relaxed) - sizeof(ChunkHeader)) /
                           RecordStride;
                }

                /// @brief True when no record has been written since the last arm.
                [[nodiscard]] bool IsEmpty() const
                {
                    return WriteOffset.load(std::memory_order_relaxed) == sizeof(ChunkHeader);
                }
            };
        }

        /// @brief Per-thread recording state; owned by the profiler, reached by the thread_local pointer.
        struct ThreadState
        {
            ProfilerState* Owner = nullptr;
            ThreadId Id = 0;
            string Name;

            vector<Unique<Chunk>> Chunks;
            usize CurrentChunk = 0;
            u64 NextSequence = 0;

            vector<ScopeStackEntry> Stack;

            // Per-thread aggregation, lock-free: the owner thread accumulates here and the frame fold
            // drains it. No mutex — the previous per-scope lock was the largest hot-path cost.
            ScopeAggStore Agg;
        };

        /// @brief Per-profiler recording state: the whole subsystem behind the Profiler shell.
        struct ProfilerState
        {
            ProfilerConfig Config;
            ProfilerMode Mode = ProfilerMode::Off;
            TraceSink* Sink = nullptr;
            // Read on the hot path (which frame a record is stamped with, and the aggregate's
            // last-active frame) while BeginFrame advances it, so it is atomic. Frame indexing is
            // approximate to within a frame at the hot path, which is all a record needs.
            std::atomic<u64> FrameIndex{0};

            std::mutex RegistryMutex;
            vector<Unique<ThreadState>> Threads;
            ThreadId NextThreadId = 1;
            u64 RegistrationOverflow = 0;
            std::atomic<u64> DroppedEvents{0};

            std::mutex StringMutex;
            std::unordered_map<string, NameId> StringToId;
            vector<string> IdToString;

            std::mutex TrackMutex;
            struct TrackInfo
            {
                string Name;
                TrackRole Role = TrackRole::Custom;
            };
            vector<TrackInfo> Tracks;
            TrackId NextTrackId = 1;

            std::mutex SnapshotMutex;
            std::unordered_map<NameId, ScopeAggregate> Snapshot;

            // Capture control. The active capture's sink (streaming to a file) and, once ended, the
            // sink still flushing to disk off-thread. RingEnabled is the standing policy a capture
            // temporarily overrides; the active Mode/Sink is re-derived from it (ApplyPolicy) when the
            // capture ends. All touched only from the control thread (the BeginFrame self-terminate
            // runs there too), so no lock of their own.
            Unique<FileTraceSink> CaptureSink;
            Unique<FileTraceSink> DrainingSink;
            path CaptureFilePath;
            bool RingEnabled = false;
            u64 CaptureFrameBudget = 0;
            u64 CaptureStartFrame = 0;

            ThreadState* AttachThread(string_view name);
            void DetachThread(ThreadState* state);
            void EmitThreadTrack(ThreadState& state);
            NameId Intern(string_view text);
            void HandChunkToSink(ThreadState& state, Chunk& chunk);
            void AdvanceFrame();

            static VoidResult EnsureDirectory(const path& file);
            void AttachSink(TraceSink* sink);
            void DeliverStateToSink(TraceSink& sink);
            void ApplyPolicy();
            void ReapDrainingSink();
            void EnforceRetention(const path& directory);
            VoidResult BeginCapture(const path& file, u64 frameCount);
            Result<path> EndCapture();
            Result<path> DumpRing(const path& file);
            CaptureState GetState();
        };

        namespace
        {
            /// @brief The active profiler's state, installed by the owning Profiler. The macros' anchor.
            std::atomic<ProfilerState*> g_Active{nullptr};

            /// @brief The active Profiler instance, for GetActiveProfiler(); the object behind g_Active.
            std::atomic<Profiler*> g_ActiveProfiler{nullptr};

            /// @brief Bumped on every Profiler construction and destruction, to invalidate thread caches.
            std::atomic<u64> g_Generation{0};

            /// @brief The calling thread's state under the active profiler, or null.
            thread_local ThreadState* t_State = nullptr;
            /// @brief The profiler that t_State belongs to.
            thread_local ProfilerState* t_Owner = nullptr;
            /// @brief The generation t_State was attached in.
            thread_local u64 t_Generation = 0;

            /// @brief Detaches the calling thread from its profiler when the thread exits.
            ///
            /// Only touches the owner while it is provably still alive (same pointer active, same
            /// generation), so a thread that outlives a destroyed profiler does nothing. The contract
            /// is that a profiler outlives every thread that registered with it except the main thread,
            /// which it detaches itself.
            struct ThreadExitGuard
            {
                ~ThreadExitGuard()
                {
                    ProfilerState* owner = t_Owner;
                    if (t_State && owner && g_Active.load(std::memory_order_acquire) == owner &&
                        t_Generation == g_Generation.load(std::memory_order_acquire))
                    {
                        owner->DetachThread(t_State);
                    }
                    t_State = nullptr;
                    t_Owner = nullptr;
                }
            };
            thread_local ThreadExitGuard t_ExitGuard;

            /// @brief odr-uses the exit guard so its thread-exit destructor is armed for this thread.
            void ArmExitGuard()
            {
                (void)&t_ExitGuard;
            }

            /// @brief Appends one built record, streaming or ring-wrapping when the current chunk fills.
            ///
            /// The record's timestamp deltas are computed against the chunk it actually lands in, so a
            /// commit that straddles a chunk boundary stays base-relative to its own chunk.
            void EmitEvent(ThreadState& state, RecordType type, u32 track, NameId name,
                           u64 beginAbs, u64 endAbs, u64 valueBits, u64 frame)
            {
                ProfilerState& profiler = *state.Owner;
                Chunk* chunk = state.Chunks[state.CurrentChunk].get();
                u32 offset = chunk->WriteOffset.load(std::memory_order_relaxed);

                if (offset + RecordStride > chunk->Capacity)
                {
                    if (profiler.Sink)
                    {
                        // Streaming drain: seal and hand the full chunk over, then reuse it in place.
                        profiler.HandChunkToSink(state, *chunk);
                        chunk->Arm(NowTicks(), state.NextSequence++);
                    }
                    else
                    {
                        // Ring drain: advance to the next chunk, discarding it whole if it still holds
                        // un-drained records (the ring wrapping onto live data).
                        const usize next = (state.CurrentChunk + 1) % state.Chunks.size();
                        Chunk* target = state.Chunks[next].get();
                        if (!target->IsEmpty())
                        {
                            profiler.DroppedEvents.fetch_add(target->RecordCount(),
                                                             std::memory_order_relaxed);
                        }
                        target->Arm(NowTicks(), state.NextSequence++);
                        state.CurrentChunk = next;
                        chunk = target;
                    }
                    offset = chunk->WriteOffset.load(std::memory_order_relaxed);
                }

                EventRecord record;
                record.Type = static_cast<u8>(type);
                record.Track = track;
                record.Name = name;
                record.Frame = static_cast<u32>(frame);
                const u64 base = chunk->TimestampBase.load(std::memory_order_relaxed);
                record.BeginDelta = beginAbs >= base ? beginAbs - base : 0;
                if (type == RecordType::ScopeComplete)
                {
                    record.EndOrValue = endAbs >= base ? endAbs - base : 0;
                }
                else if (type == RecordType::Counter)
                {
                    record.EndOrValue = valueBits;
                }

                std::memcpy(chunk->Data.get() + offset, &record, sizeof(record));
                // Publish the new extent with release so a collector that acquire-loads the offset
                // sees the fully written record below it.
                chunk->WriteOffset.store(offset + RecordStride, std::memory_order_release);
            }
        }

        ThreadState* ProfilerState::AttachThread(string_view name)
        {
            const std::scoped_lock lock(RegistryMutex);
            if (Threads.size() >= Config.MaxThreads)
            {
                ++RegistrationOverflow;
                return nullptr;
            }

            auto state = CreateUnique<ThreadState>();
            state->Owner = this;
            state->Id = NextThreadId++;
            state->Name = string(name);
            state->Chunks.reserve(Config.ChunksPerThread);
            for (u32 i = 0; i < Config.ChunksPerThread; ++i)
            {
                auto chunk = CreateUnique<Chunk>();
                chunk->Capacity = Config.ChunkBytes;
                chunk->Data = Unique<u8[]>(new u8[Config.ChunkBytes]);
                chunk->Arm(NowTicks(), state->NextSequence++);
                state->Chunks.push_back(std::move(chunk));
            }
            state->Agg.Reserve(Config.StringTableCapacity);
            state->Stack.reserve(64);

            ThreadState* raw = state.get();
            Threads.push_back(std::move(state));
            return raw;
        }

        void ProfilerState::HandChunkToSink(ThreadState& state, Chunk& chunk)
        {
            if (!Sink || chunk.IsEmpty())
            {
                return;
            }
            chunk.SealHeader();
            const u32 bytes = chunk.WriteOffset.load(std::memory_order_relaxed);
            Sink->OnChunk(state.Id, chunk.Data.get(), bytes);
        }

        void ProfilerState::DetachThread(ThreadState* state)
        {
            const std::scoped_lock lock(RegistryMutex);
            for (auto it = Threads.begin(); it != Threads.end(); ++it)
            {
                if (it->get() != state)
                {
                    continue;
                }
                // Hand back the thread's outstanding records so a detach loses nothing.
                for (auto& chunk : state->Chunks)
                {
                    HandChunkToSink(*state, *chunk);
                }
                Threads.erase(it);
                return;
            }
        }

        void ProfilerState::EmitThreadTrack(ThreadState& state)
        {
            // A track descriptor enriches the id→name/role mapping a decoder would otherwise fall
            // back to the bare id for. Emitted only while a sink is attached; a capture that begins
            // later replays every known thread through SetSink.
            if (Sink)
            {
                Sink->OnTrack(TrackDescriptor{.Id = state.Id,
                                              .IsVirtual = false,
                                              .Role = TrackRole::Cpu,
                                              .Name = state.Name});
            }
        }

        NameId ProfilerState::Intern(string_view text)
        {
            const std::scoped_lock lock(StringMutex);
            const auto it = StringToId.find(string(text));
            if (it != StringToId.end())
            {
                return it->second;
            }
            const NameId id = static_cast<NameId>(IdToString.size()) + 1;
            IdToString.emplace_back(text);
            StringToId.emplace(string(text), id);
            if (Sink)
            {
                StringTableDelta delta;
                delta.FirstId = id;
                delta.Strings.push_back(IdToString[id - 1]);
                Sink->OnStrings(delta);
            }
            return id;
        }

        void ProfilerState::AdvanceFrame()
        {
            // The frame now completing; a scope that ran this frame is stamped active in it.
            const u64 completingFrame = FrameIndex.load(std::memory_order_relaxed);

            std::unordered_map<NameId, ScopeAggregate> folded;
            {
                const std::scoped_lock registry(RegistryMutex);
                for (auto& thread : Threads)
                {
                    // Acquire the owner's published id count, then read only accumulators below it;
                    // the release/acquire pair means every block those ids live in is in place.
                    const u32 published = thread->Agg.Published();
                    for (u32 index = 0; index < published; ++index)
                    {
                        ScopeAccum* accum = thread->Agg.At(index);
                        if (accum == nullptr)
                        {
                            continue;
                        }
                        const u64 callTotal = accum->CallTotal.load(std::memory_order_relaxed);
                        if (callTotal == accum->CallFolded)
                        {
                            continue; // no calls since the last fold: idle this frame
                        }
                        const u64 inclusiveTotal =
                            accum->InclusiveTicks.load(std::memory_order_relaxed);
                        const u64 selfTotal = accum->SelfTicks.load(std::memory_order_relaxed);
                        const u64 callDelta = callTotal - accum->CallFolded;
                        const u64 inclusiveDelta = inclusiveTotal - accum->InclusiveFolded;
                        const u64 selfDelta = selfTotal - accum->SelfFolded;
                        accum->CallFolded = callTotal;
                        accum->InclusiveFolded = inclusiveTotal;
                        accum->SelfFolded = selfTotal;

                        const NameId name = index + 1;
                        ScopeAggregate& out = folded[name];
                        out.Name = name;
                        out.CallCount += callDelta;
                        out.InclusiveNanos += TraceTicksToNanos(inclusiveDelta);
                        out.SelfNanos += TraceTicksToNanos(selfDelta);
                        out.LastActiveFrame = completingFrame;
                    }
                }
            }

            const std::scoped_lock snapshot(SnapshotMutex);
            // Carry forward names that did not run this frame, so an intermittent scope stays visible
            // with a zero call count and its last-active frame preserved.
            for (auto& [name, prior] : Snapshot)
            {
                if (folded.find(name) == folded.end())
                {
                    ScopeAggregate carried = prior;
                    carried.CallCount = 0;
                    carried.InclusiveNanos = 0;
                    carried.SelfNanos = 0;
                    folded.emplace(name, carried);
                }
            }
            Snapshot = std::move(folded);
            FrameIndex.fetch_add(1, std::memory_order_relaxed);

            // A self-terminating capture ends at this frame boundary — never mid-frame, so a decoder
            // never sees a truncated final frame. Runs after every lock above is released; EndCapture
            // takes its own.
            if (CaptureSink && CaptureFrameBudget != 0)
            {
                const u64 elapsed = FrameIndex.load(std::memory_order_relaxed) - CaptureStartFrame;
                if (elapsed >= CaptureFrameBudget)
                {
                    (void)EndCapture();
                }
            }
        }

        void ProfilerState::DeliverStateToSink(TraceSink& sink)
        {
            // Bring a sink current with every string interned so far, as one full table — a triggered
            // capture reads it as its opening delta, and a ring dump needs the whole table since its
            // early chunks (where those ids were interned) may already be gone.
            {
                const std::scoped_lock lock(StringMutex);
                if (!IdToString.empty())
                {
                    StringTableDelta delta;
                    delta.FirstId = 1;
                    delta.Strings.reserve(IdToString.size());
                    for (const string& text : IdToString)
                    {
                        delta.Strings.emplace_back(text);
                    }
                    sink.OnStrings(delta);
                }
            }

            // Replay every known track's descriptor so a thread named before the sink attached (Main,
            // the workers) and every virtual track carries its real name/role rather than a bare id.
            {
                const std::scoped_lock lock(RegistryMutex);
                for (const auto& thread : Threads)
                {
                    sink.OnTrack(TrackDescriptor{.Id = thread->Id,
                                                 .IsVirtual = false,
                                                 .Role = TrackRole::Cpu,
                                                 .Name = thread->Name});
                }
            }
            {
                const std::scoped_lock lock(TrackMutex);
                for (usize i = 0; i < Tracks.size(); ++i)
                {
                    sink.OnTrack(TrackDescriptor{.Id = static_cast<TrackId>(i + 1),
                                                 .IsVirtual = true,
                                                 .Role = Tracks[i].Role,
                                                 .Name = Tracks[i].Name});
                }
            }
        }

        VoidResult ProfilerState::EnsureDirectory(const path& file)
        {
            if (!file.has_parent_path())
            {
                return {};
            }
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            if (ec)
            {
                return std::unexpected(fmt::format("cannot create capture directory '{}': {}",
                                                   file.parent_path().string(), ec.message()));
            }
            return {};
        }

        void ProfilerState::AttachSink(TraceSink* sink)
        {
            Sink = sink;
            if (sink)
            {
                DeliverStateToSink(*sink);
            }
        }

        void ProfilerState::ApplyPolicy()
        {
            // A running capture owns Mode and Sink directly; when none runs, the standing ring policy
            // decides whether the buffers record into a wrapping ring or stay off.
            if (CaptureSink)
            {
                return;
            }
            Sink = nullptr;
            Mode = RingEnabled ? ProfilerMode::Ring : ProfilerMode::Off;
        }

        void ProfilerState::ReapDrainingSink()
        {
            if (DrainingSink && DrainingSink->HasFinishedWriting())
            {
                const path directory = DrainingSink->GetPath().parent_path();
                DrainingSink.reset();
                EnforceRetention(directory);
            }
        }

        void ProfilerState::EnforceRetention(const path& directory)
        {
            const u32 cap = Config.RetainedCaptureCap;
            if (cap == 0 || directory.empty())
            {
                return;
            }
            std::error_code ec;
            if (!std::filesystem::is_directory(directory, ec))
            {
                return;
            }

            struct Entry
            {
                path File;
                std::filesystem::file_time_type Time;
            };
            std::vector<Entry> entries;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                if (ec)
                {
                    return;
                }
                if (!entry.is_regular_file() || entry.path().extension() != ".vtrace")
                {
                    continue;
                }
                std::error_code timeError;
                const auto time = entry.last_write_time(timeError);
                if (timeError)
                {
                    continue;
                }
                entries.push_back(Entry{.File = entry.path(), .Time = time});
            }

            if (entries.size() <= cap)
            {
                return;
            }
            // Oldest first, so the cap deletes the least-recent captures until the directory is under it.
            std::ranges::sort(entries,
                              [](const Entry& a, const Entry& b) { return a.Time < b.Time; });
            const usize toDelete = entries.size() - cap;
            for (usize i = 0; i < toDelete; ++i)
            {
                std::error_code removeError;
                std::filesystem::remove(entries[i].File, removeError);
            }
        }

        VoidResult ProfilerState::BeginCapture(const path& file, u64 frameCount)
        {
            ReapDrainingSink();
            if (CaptureSink)
            {
                return std::unexpected(
                    fmt::format("a capture is already in flight ('{}'); end it first",
                                CaptureFilePath.string()));
            }
            if (const VoidResult ready = EnsureDirectory(file); !ready)
            {
                return std::unexpected(ready.error());
            }

            Unique<FileTraceSink> sink = FileTraceSink::Create(file, /*ringDump=*/false);
            if (!sink)
            {
                return std::unexpected(
                    fmt::format("failed to create a capture sink at '{}'", file.string()));
            }

            CaptureSink = std::move(sink);
            CaptureFilePath = file;
            CaptureFrameBudget = frameCount;
            CaptureStartFrame = FrameIndex.load(std::memory_order_relaxed);
            // Recording on (Mode != Off), streaming to the file (a non-null sink selects the
            // hand-off-when-full drain). The standing RingEnabled is untouched — a capture overrides
            // the active policy but does not change it.
            Mode = ProfilerMode::Ring;
            AttachSink(CaptureSink.get());
            return {};
        }

        Result<path> ProfilerState::EndCapture()
        {
            if (!CaptureSink)
            {
                return std::unexpected(string("no capture is running"));
            }

            u64 droppedThreads = 0;
            {
                const std::scoped_lock lock(RegistryMutex);
                droppedThreads = RegistrationOverflow;
                // Flush every thread's outstanding chunk up to its published write offset.
                for (auto& thread : Threads)
                {
                    for (auto& chunk : thread->Chunks)
                    {
                        HandChunkToSink(*thread, *chunk);
                    }
                }
            }
            CaptureSink->SetAccounting(DroppedEvents.load(std::memory_order_relaxed),
                                       droppedThreads);
            CaptureSink->BeginClose();

            const path written = CaptureFilePath;
            DrainingSink = std::move(CaptureSink);
            CaptureFilePath.clear();
            CaptureFrameBudget = 0;
            CaptureStartFrame = 0;
            // Restore the standing policy the capture overrode.
            ApplyPolicy();
            return written;
        }

        Result<path> ProfilerState::DumpRing(const path& file)
        {
            ReapDrainingSink();
            if (CaptureSink)
            {
                return std::unexpected(fmt::format(
                    "a capture is in flight ('{}'); the ring is unavailable until it ends",
                    CaptureFilePath.string()));
            }
            if (const VoidResult ready = EnsureDirectory(file); !ready)
            {
                return std::unexpected(ready.error());
            }

            Unique<FileTraceSink> sink = FileTraceSink::Create(file, /*ringDump=*/true);
            if (!sink)
            {
                return std::unexpected(
                    fmt::format("failed to create a dump sink at '{}'", file.string()));
            }

            // The full string table and every track's descriptor, so a dump whose earliest chunks
            // (carrying the deltas that first interned those ids) were discarded still resolves them.
            DeliverStateToSink(*sink);

            // Copy a live ring chunk up to its acquire-loaded write offset without disturbing it — the
            // ring keeps recording — and seal the record byte count into the copy's header.
            const auto collect = [&sink](ThreadId thread, Chunk& chunk)
            {
                const u32 offset = chunk.WriteOffset.load(std::memory_order_acquire);
                if (offset <= sizeof(ChunkHeader))
                {
                    return;
                }
                std::vector<u8> bytes(chunk.Data.get(), chunk.Data.get() + offset);
                const u32 recordBytes = offset - static_cast<u32>(sizeof(ChunkHeader));
                std::memcpy(bytes.data() + offsetof(ChunkHeader, RecordBytes), &recordBytes,
                            sizeof(recordBytes));
                sink->OnChunk(thread, bytes.data(), offset);
            };

            u64 droppedThreads = 0;
            {
                const std::scoped_lock lock(RegistryMutex);
                droppedThreads = RegistrationOverflow;
                for (auto& thread : Threads)
                {
                    // Each thread's live chunks in sequence order, oldest first — the discarded span
                    // between them reads as a sequence gap, not silence.
                    std::vector<Chunk*> live;
                    for (auto& chunk : thread->Chunks)
                    {
                        if (!chunk->IsEmpty())
                        {
                            live.push_back(chunk.get());
                        }
                    }
                    std::ranges::sort(live,
                                      [](const Chunk* a, const Chunk* b)
                                      {
                                          return a->SequenceNumber.load(std::memory_order_relaxed) <
                                                 b->SequenceNumber.load(std::memory_order_relaxed);
                                      });
                    for (Chunk* chunk : live)
                    {
                        collect(thread->Id, *chunk);
                    }
                }
            }

            sink->SetAccounting(DroppedEvents.load(std::memory_order_relaxed), droppedThreads);
            sink->BeginClose();

            const path written = file;
            DrainingSink = std::move(sink);
            return written;
        }

        CaptureState ProfilerState::GetState()
        {
            ReapDrainingSink();
            CaptureState state;
            if (CaptureSink)
            {
                state.Status = CaptureStatus::Capturing;
                state.FrameBudget = CaptureFrameBudget;
                state.FramesElapsed =
                    FrameIndex.load(std::memory_order_relaxed) - CaptureStartFrame;
                state.Path = CaptureFilePath;
            }
            else if (RingEnabled)
            {
                state.Status = CaptureStatus::Ring;
            }
            else
            {
                state.Status = CaptureStatus::Off;
            }
            state.WriterDraining = DrainingSink != nullptr;
            return state;
        }

        ThreadState* CurrentThreadState() noexcept
        {
            ArmExitGuard();
            ProfilerState* active = g_Active.load(std::memory_order_acquire);
            if (!active)
            {
                return nullptr;
            }
            const u64 generation = g_Generation.load(std::memory_order_acquire);
            if (t_State && t_Owner == active && t_Generation == generation)
            {
                return t_State;
            }
            t_State = active->AttachThread(string_view());
            t_Owner = active;
            t_Generation = generation;
            return t_State;
        }

        NameId ResolveLiteralName(ThreadState* state, ScopeName& name) noexcept
        {
            const u64 generation = g_Generation.load(std::memory_order_acquire);
            if (name.Generation.load(std::memory_order_acquire) == generation)
            {
                return name.Id.load(std::memory_order_relaxed);
            }
            // First execution under this profiler (or a new one after a teardown): resolve the id
            // through the string table once and cache it at the call site. Two threads may resolve
            // concurrently; Intern is idempotent, so they agree on the id.
            const NameId id = state->Owner->Intern(name.Literal);
            name.Id.store(id, std::memory_order_relaxed);
            name.Generation.store(generation, std::memory_order_release);
            return id;
        }

        NameId InternDynamic(ThreadState* state, string_view name) noexcept
        {
            return state->Owner->Intern(name);
        }

        void EnterScope(ThreadState* state) noexcept
        {
            state->Stack.push_back(ScopeStackEntry{});
        }

        void CommitScope(ThreadState* state, NameId name, u64 beginTicks, u64 endTicks) noexcept
        {
            const u64 inclusive = endTicks >= beginTicks ? endTicks - beginTicks : 0;

            // Self-time: this scope's inclusive time, less the child time accumulated while it was
            // open. The RAII scopes nest, so the innermost commits first; fold this scope's inclusive
            // into its parent's child total. All in raw ticks; nanosecond conversion is the fold's job.
            u64 childTicks = 0;
            if (!state->Stack.empty())
            {
                childTicks = state->Stack.back().ChildTicks;
                state->Stack.pop_back();
            }
            if (!state->Stack.empty())
            {
                state->Stack.back().ChildTicks += inclusive;
            }
            const u64 self = inclusive >= childTicks ? inclusive - childTicks : 0;

            // Aggregation is always live under VE_PROFILE=ON, independent of recording. Lock-free:
            // this thread is the sole writer of its own accumulators, so a monotonic add (a relaxed
            // load and store, no read-modify-write) suffices; the frame fold reads and diffs them.
            ScopeAccum* accum = state->Agg.Touch(name);
            if (accum != nullptr)
            {
                accum->CallTotal.store(accum->CallTotal.load(std::memory_order_relaxed) + 1,
                                       std::memory_order_relaxed);
                accum->InclusiveTicks.store(accum->InclusiveTicks.load(std::memory_order_relaxed) +
                                                inclusive,
                                            std::memory_order_relaxed);
                accum->SelfTicks.store(accum->SelfTicks.load(std::memory_order_relaxed) + self,
                                       std::memory_order_relaxed);
            }

            if (state->Owner->Mode != ProfilerMode::Off)
            {
                const u64 frame = state->Owner->FrameIndex.load(std::memory_order_relaxed);
                EmitEvent(*state, RecordType::ScopeComplete, 0, name, beginTicks, endTicks, 0,
                          frame);
            }
        }

        void CommitCounter(ThreadState* state, NameId name, f64 value) noexcept
        {
            if (state->Owner->Mode == ProfilerMode::Off)
            {
                return;
            }
            u64 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            const u64 frame = state->Owner->FrameIndex.load(std::memory_order_relaxed);
            EmitEvent(*state, RecordType::Counter, 0, name, NowTicks(), 0, bits, frame);
        }

        void CommitInstant(ThreadState* state, NameId name) noexcept
        {
            if (state->Owner->Mode == ProfilerMode::Off)
            {
                return;
            }
            const u64 frame = state->Owner->FrameIndex.load(std::memory_order_relaxed);
            EmitEvent(*state, RecordType::Instant, 0, name, NowTicks(), 0, 0, frame);
        }

        void NameCurrentThread(const char* name) noexcept
        {
            ThreadState* state = CurrentThreadState();
            if (state)
            {
                state->Name = name;
                state->Owner->EmitThreadTrack(*state);
            }
        }

        void MarkFrame() noexcept
        {
            ProfilerState* active = g_Active.load(std::memory_order_acquire);
            if (active)
            {
                active->AdvanceFrame();
            }
        }

        /// @brief Emits a back-dated span on an explicit track and frame; the virtual-track primitive.
        void CommitEmitScope(ThreadState* state, TrackId track, NameId name, u64 beginTicks,
                             u64 endTicks, u64 frame) noexcept
        {
            EmitEvent(*state, RecordType::ScopeComplete, track, name, beginTicks, endTicks, 0,
                      frame);
        }
    }
}

// ---------------------------------------------------------------------------
// Profiler and ProfilerThreadRegistration — the enabled implementation.
// ---------------------------------------------------------------------------

namespace Veng::Diagnostics
{
    Profiler::Profiler(const ProfilerConfig& config)
        : m_State(CreateUnique<Detail::ProfilerState>())
    {
        m_State->Config = config;
        m_State->Mode = config.InitialMode;
        m_State->IdToString.reserve(config.StringTableCapacity);
        m_State->StringToId.reserve(config.StringTableCapacity);

        Detail::g_Generation.fetch_add(1, std::memory_order_acq_rel);
        Detail::g_Active.store(m_State.get(), std::memory_order_release);
        Detail::g_ActiveProfiler.store(this, std::memory_order_release);

        // Register the calling (main) thread against this profiler.
        Detail::t_State = m_State->AttachThread(string_view("Main"));
        Detail::t_Owner = m_State.get();
        Detail::t_Generation = Detail::g_Generation.load(std::memory_order_acquire);
    }

    Profiler::~Profiler()
    {
        if (m_State->Sink)
        {
            const std::scoped_lock lock(m_State->RegistryMutex);
            for (auto& thread : m_State->Threads)
            {
                for (auto& chunk : thread->Chunks)
                {
                    m_State->HandChunkToSink(*thread, *chunk);
                }
            }
            m_State->Sink->OnFlush();
            m_State->Sink->OnClose();
        }

        Detail::g_ActiveProfiler.store(nullptr, std::memory_order_release);
        Detail::g_Active.store(nullptr, std::memory_order_release);
        Detail::g_Generation.fetch_add(1, std::memory_order_acq_rel);
        Detail::t_State = nullptr;
        Detail::t_Owner = nullptr;
    }

    Profiler* GetActiveProfiler() noexcept
    {
        return Detail::g_ActiveProfiler.load(std::memory_order_acquire);
    }

    void Profiler::SetSink(TraceSink* sink)
    {
        m_State->AttachSink(sink);
    }

    TraceSink* Profiler::GetSink() const
    {
        return m_State->Sink;
    }

    void Profiler::SetMode(ProfilerMode mode)
    {
        m_State->Mode = mode;
    }

    ProfilerMode Profiler::GetMode() const
    {
        return m_State->Mode;
    }

    ProfilerThreadRegistration Profiler::RegisterThread(string_view name)
    {
        Detail::ThreadState* state = m_State->AttachThread(name);
        if (!state)
        {
            return ProfilerThreadRegistration();
        }
        Detail::t_State = state;
        Detail::t_Owner = m_State.get();
        Detail::t_Generation = Detail::g_Generation.load(std::memory_order_acquire);
        m_State->EmitThreadTrack(*state);
        return ProfilerThreadRegistration(this, state->Id);
    }

    TrackId Profiler::CreateTrack(string_view name, TrackRole role)
    {
        TrackId id;
        {
            const std::scoped_lock lock(m_State->TrackMutex);
            id = m_State->NextTrackId++;
            m_State->Tracks.push_back(
                Detail::ProfilerState::TrackInfo{.Name = string(name), .Role = role});
        }
        if (m_State->Sink)
        {
            m_State->Sink->OnTrack(
                TrackDescriptor{.Id = id, .IsVirtual = true, .Role = role, .Name = name});
        }
        return id;
    }

    NameId Profiler::InternName(string_view name)
    {
        return m_State->Intern(name);
    }

    void Profiler::EmitScope(TrackId track, NameId name, u64 beginTicks, u64 endTicks)
    {
        EmitScope(track, name, beginTicks, endTicks,
                  m_State->FrameIndex.load(std::memory_order_relaxed));
    }

    void Profiler::EmitScope(TrackId track, NameId name, u64 beginTicks, u64 endTicks,
                             u64 frameIndex)
    {
        if (m_State->Mode == ProfilerMode::Off)
        {
            return;
        }
        Detail::ThreadState* state = Detail::CurrentThreadState();
        if (state)
        {
            Detail::CommitEmitScope(state, track, name, beginTicks, endTicks, frameIndex);
        }
    }

    void Profiler::BeginFrame()
    {
        m_State->AdvanceFrame();
    }

    u64 Profiler::GetFrameIndex() const
    {
        return m_State->FrameIndex.load(std::memory_order_relaxed);
    }

    bool Profiler::IsRecording() const
    {
        return m_State->Mode != ProfilerMode::Off;
    }

    optional<ScopeAggregate> Profiler::GetScopeAggregate(NameId name) const
    {
        const std::scoped_lock lock(m_State->SnapshotMutex);
        const auto it = m_State->Snapshot.find(name);
        if (it == m_State->Snapshot.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    vector<ScopeAggregate> Profiler::GetFrameAggregates() const
    {
        const std::scoped_lock lock(m_State->SnapshotMutex);
        vector<ScopeAggregate> out;
        out.reserve(m_State->Snapshot.size());
        for (const auto& [name, aggregate] : m_State->Snapshot)
        {
            out.push_back(aggregate);
        }
        return out;
    }

    string_view Profiler::GetName(NameId name) const
    {
        const std::scoped_lock lock(m_State->StringMutex);
        if (name == 0 || name > m_State->IdToString.size())
        {
            return string_view();
        }
        return m_State->IdToString[name - 1];
    }

    u64 Profiler::GetRegistrationOverflowCount() const
    {
        const std::scoped_lock lock(m_State->RegistryMutex);
        return m_State->RegistrationOverflow;
    }

    u64 Profiler::GetDroppedEventCount() const
    {
        return m_State->DroppedEvents.load(std::memory_order_relaxed);
    }

    VoidResult Profiler::BeginCapture(const path& path, u64 frameCount)
    {
        return m_State->BeginCapture(path, frameCount);
    }

    Result<path> Profiler::EndCapture()
    {
        return m_State->EndCapture();
    }

    void Profiler::SetRingEnabled(bool enabled)
    {
        m_State->RingEnabled = enabled;
        m_State->ApplyPolicy();
    }

    Result<path> Profiler::DumpRing(const path& path)
    {
        return m_State->DumpRing(path);
    }

    CaptureState Profiler::GetState() const
    {
        return m_State->GetState();
    }

    // A registration is RAII on the thread that created it, so it detaches the calling thread. As a
    // friend of Profiler it reaches the private recording state directly.
    ProfilerThreadRegistration::~ProfilerThreadRegistration()
    {
        if (m_Owner && Detail::t_State)
        {
            m_Owner->m_State->DetachThread(Detail::t_State);
            Detail::t_State = nullptr;
            Detail::t_Owner = nullptr;
        }
    }

    ProfilerThreadRegistration::ProfilerThreadRegistration(
        ProfilerThreadRegistration&& other) noexcept
        : m_Owner(other.m_Owner), m_Thread(other.m_Thread)
    {
        other.m_Owner = nullptr;
        other.m_Thread = 0;
    }

    ProfilerThreadRegistration&
    ProfilerThreadRegistration::operator=(ProfilerThreadRegistration&& other) noexcept
    {
        if (this != &other)
        {
            if (m_Owner && Detail::t_State)
            {
                m_Owner->m_State->DetachThread(Detail::t_State);
                Detail::t_State = nullptr;
                Detail::t_Owner = nullptr;
            }
            m_Owner = other.m_Owner;
            m_Thread = other.m_Thread;
            other.m_Owner = nullptr;
            other.m_Thread = 0;
        }
        return *this;
    }
}

#else // VE_PROFILE off — the lifecycle surface as documented no-ops, no recording storage.

namespace Veng::Diagnostics
{
    Profiler::Profiler(const ProfilerConfig& /*config*/) {}
    Profiler::~Profiler() = default;
    Profiler* GetActiveProfiler() noexcept
    {
        return nullptr;
    }
    void Profiler::SetSink(TraceSink* /*sink*/) {}
    TraceSink* Profiler::GetSink() const
    {
        return nullptr;
    }
    void Profiler::SetMode(ProfilerMode /*mode*/) {}
    ProfilerMode Profiler::GetMode() const
    {
        return ProfilerMode::Off;
    }
    ProfilerThreadRegistration Profiler::RegisterThread(string_view /*name*/)
    {
        return {};
    }
    TrackId Profiler::CreateTrack(string_view /*name*/, TrackRole /*role*/)
    {
        return 0;
    }
    NameId Profiler::InternName(string_view /*name*/)
    {
        return 0;
    }
    void Profiler::EmitScope(TrackId /*track*/, NameId /*name*/, u64 /*beginTicks*/,
                             u64 /*endTicks*/)
    {
    }
    void Profiler::EmitScope(TrackId /*track*/, NameId /*name*/, u64 /*beginTicks*/,
                             u64 /*endTicks*/, u64 /*frameIndex*/)
    {
    }
    void Profiler::BeginFrame() {}
    u64 Profiler::GetFrameIndex() const
    {
        return 0;
    }
    bool Profiler::IsRecording() const
    {
        return false;
    }
    optional<ScopeAggregate> Profiler::GetScopeAggregate(NameId /*name*/) const
    {
        return std::nullopt;
    }
    vector<ScopeAggregate> Profiler::GetFrameAggregates() const
    {
        return {};
    }
    string_view Profiler::GetName(NameId /*name*/) const
    {
        return string_view();
    }
    u64 Profiler::GetRegistrationOverflowCount() const
    {
        return 0;
    }
    u64 Profiler::GetDroppedEventCount() const
    {
        return 0;
    }

    // The capture verbs are documented no-ops under VE_PROFILE=OFF: there is no recording state to
    // capture, so a request returns a clear error a caller reports rather than failing to build.
    namespace
    {
        VoidResult ProfileDisabled()
        {
            return std::unexpected(
                string("the profiler is disabled in this build (VE_PROFILE=OFF)"));
        }
    }

    VoidResult Profiler::BeginCapture(const path& /*path*/, u64 /*frameCount*/)
    {
        return ProfileDisabled();
    }

    Result<path> Profiler::EndCapture()
    {
        return std::unexpected(ProfileDisabled().error());
    }

    void Profiler::SetRingEnabled(bool /*enabled*/) {}

    Result<path> Profiler::DumpRing(const path& /*path*/)
    {
        return std::unexpected(ProfileDisabled().error());
    }

    CaptureState Profiler::GetState() const
    {
        return {};
    }

    ProfilerThreadRegistration::~ProfilerThreadRegistration() = default;
    ProfilerThreadRegistration::ProfilerThreadRegistration(ProfilerThreadRegistration&&) noexcept =
        default;
    ProfilerThreadRegistration&
    ProfilerThreadRegistration::operator=(ProfilerThreadRegistration&&) noexcept = default;
}

#endif
