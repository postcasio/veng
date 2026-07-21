#include <Veng/Diagnostics/Profiler.h>

#if defined(VE_PROFILE) && VE_PROFILE

#include "TraceFormat.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

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
            /// @brief One entry on a thread's open-scope stack, holding time attributed to children.
            struct ScopeStackEntry
            {
                u64 ChildNanos = 0;
            };

            /// @brief A thread's running per-frame roll-up for one scope name.
            struct ScopeAccum
            {
                u64 CallCount = 0;
                u64 InclusiveNanos = 0;
                u64 SelfNanos = 0;
                u64 LastActiveFrame = 0;
            };

            /// @brief One fixed-size buffer chunk. Non-movable: the published offset is an atomic.
            struct Chunk
            {
                Unique<u8[]> Data;
                u32 Capacity = 0;
                std::atomic<u32> WriteOffset{sizeof(ChunkHeader)};
                u64 TimestampBase = 0;
                u64 SequenceNumber = 0;

                /// @brief Re-arms the chunk for reuse with a fresh base and sequence, writing its header.
                void Arm(u64 base, u64 sequence)
                {
                    TimestampBase = base;
                    SequenceNumber = sequence;
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

            std::unordered_map<const void*, NameId> LiteralCache;
            vector<ScopeStackEntry> Stack;

            std::mutex AggMutex;
            std::unordered_map<NameId, ScopeAccum> Agg;
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

            ThreadState* AttachThread(string_view name);
            void DetachThread(ThreadState* state);
            NameId Intern(string_view text);
            void HandChunkToSink(ThreadState& state, Chunk& chunk);
            void AdvanceFrame();
        };

        namespace
        {
            /// @brief The active profiler's state, installed by the owning Profiler. The macros' anchor.
            std::atomic<ProfilerState*> g_Active{nullptr};

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
                           u64 beginAbs, u64 endAbs, u64 valueBits)
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
                        chunk->Arm(NowNanos(), state.NextSequence++);
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
                        target->Arm(NowNanos(), state.NextSequence++);
                        state.CurrentChunk = next;
                        chunk = target;
                    }
                    offset = chunk->WriteOffset.load(std::memory_order_relaxed);
                }

                EventRecord record;
                record.Type = static_cast<u8>(type);
                record.Track = track;
                record.Name = name;
                record.Frame =
                    static_cast<u32>(profiler.FrameIndex.load(std::memory_order_relaxed));
                record.BeginDelta =
                    beginAbs >= chunk->TimestampBase ? beginAbs - chunk->TimestampBase : 0;
                if (type == RecordType::ScopeComplete)
                {
                    record.EndOrValue =
                        endAbs >= chunk->TimestampBase ? endAbs - chunk->TimestampBase : 0;
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
                chunk->Arm(NowNanos(), state->NextSequence++);
                state->Chunks.push_back(std::move(chunk));
            }
            state->LiteralCache.reserve(256);
            state->Agg.reserve(256);
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
            std::unordered_map<NameId, ScopeAggregate> folded;
            {
                const std::scoped_lock registry(RegistryMutex);
                for (auto& thread : Threads)
                {
                    const std::scoped_lock agg(thread->AggMutex);
                    for (auto& [name, accum] : thread->Agg)
                    {
                        ScopeAggregate& out = folded[name];
                        out.Name = name;
                        out.CallCount += accum.CallCount;
                        out.InclusiveNanos += accum.InclusiveNanos;
                        out.SelfNanos += accum.SelfNanos;
                        out.LastActiveFrame = std::max(out.LastActiveFrame, accum.LastActiveFrame);
                        accum.CallCount = 0;
                        accum.InclusiveNanos = 0;
                        accum.SelfNanos = 0;
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

        NameId InternLiteral(ThreadState* state, const char* literal) noexcept
        {
            const auto it = state->LiteralCache.find(literal);
            if (it != state->LiteralCache.end())
            {
                return it->second;
            }
            const NameId id = state->Owner->Intern(literal);
            state->LiteralCache.emplace(static_cast<const void*>(literal), id);
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

        void CommitScope(ThreadState* state, NameId name, u64 beginNanos, u64 endNanos) noexcept
        {
            const u64 inclusive = endNanos >= beginNanos ? endNanos - beginNanos : 0;

            // Self-time: this scope's inclusive time, less the child time accumulated while it was
            // open. The RAII scopes nest, so the innermost commits first; fold this scope's inclusive
            // into its parent's child total.
            u64 childNanos = 0;
            if (!state->Stack.empty())
            {
                childNanos = state->Stack.back().ChildNanos;
                state->Stack.pop_back();
            }
            if (!state->Stack.empty())
            {
                state->Stack.back().ChildNanos += inclusive;
            }
            const u64 self = inclusive >= childNanos ? inclusive - childNanos : 0;

            // Aggregation is always live under VE_PROFILE=ON, independent of recording.
            {
                const std::scoped_lock lock(state->AggMutex);
                ScopeAccum& accum = state->Agg[name];
                ++accum.CallCount;
                accum.InclusiveNanos += inclusive;
                accum.SelfNanos += self;
                accum.LastActiveFrame = state->Owner->FrameIndex.load(std::memory_order_relaxed);
            }

            if (state->Owner->Mode != ProfilerMode::Off)
            {
                EmitEvent(*state, RecordType::ScopeComplete, 0, name, beginNanos, endNanos, 0);
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
            EmitEvent(*state, RecordType::Counter, 0, name, NowNanos(), 0, bits);
        }

        void CommitInstant(ThreadState* state, NameId name) noexcept
        {
            if (state->Owner->Mode == ProfilerMode::Off)
            {
                return;
            }
            EmitEvent(*state, RecordType::Instant, 0, name, NowNanos(), 0, 0);
        }

        void NameCurrentThread(const char* name) noexcept
        {
            ThreadState* state = CurrentThreadState();
            if (state)
            {
                state->Name = name;
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

        /// @brief Emits a back-dated span on an explicit track; the virtual-track primitive.
        void CommitEmitScope(ThreadState* state, TrackId track, NameId name, u64 beginNanos,
                             u64 endNanos) noexcept
        {
            EmitEvent(*state, RecordType::ScopeComplete, track, name, beginNanos, endNanos, 0);
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

        Detail::g_Active.store(nullptr, std::memory_order_release);
        Detail::g_Generation.fetch_add(1, std::memory_order_acq_rel);
        Detail::t_State = nullptr;
        Detail::t_Owner = nullptr;
    }

    void Profiler::SetSink(TraceSink* sink)
    {
        m_State->Sink = sink;
        if (sink)
        {
            // Bring a freshly attached sink current with every string interned so far.
            const std::scoped_lock lock(m_State->StringMutex);
            if (!m_State->IdToString.empty())
            {
                StringTableDelta delta;
                delta.FirstId = 1;
                delta.Strings.reserve(m_State->IdToString.size());
                for (const string& text : m_State->IdToString)
                {
                    delta.Strings.emplace_back(text);
                }
                sink->OnStrings(delta);
            }
        }
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
        return ProfilerThreadRegistration(this, state->Id);
    }

    TrackId Profiler::CreateTrack(string_view name, TrackRole role)
    {
        const std::scoped_lock lock(m_State->TrackMutex);
        const TrackId id = m_State->NextTrackId++;
        m_State->Tracks.push_back(
            Detail::ProfilerState::TrackInfo{.Name = string(name), .Role = role});
        return id;
    }

    NameId Profiler::InternName(string_view name)
    {
        return m_State->Intern(name);
    }

    void Profiler::EmitScope(TrackId track, NameId name, u64 beginNanos, u64 endNanos)
    {
        if (m_State->Mode == ProfilerMode::Off)
        {
            return;
        }
        Detail::ThreadState* state = Detail::CurrentThreadState();
        if (state)
        {
            Detail::CommitEmitScope(state, track, name, beginNanos, endNanos);
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
    void Profiler::EmitScope(TrackId /*track*/, NameId /*name*/, u64 /*beginNanos*/,
                             u64 /*endNanos*/)
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

    ProfilerThreadRegistration::~ProfilerThreadRegistration() = default;
    ProfilerThreadRegistration::ProfilerThreadRegistration(ProfilerThreadRegistration&&) noexcept =
        default;
    ProfilerThreadRegistration&
    ProfilerThreadRegistration::operator=(ProfilerThreadRegistration&&) noexcept = default;
}

#endif
