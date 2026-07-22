#pragma once

#include <Veng/Veng.h>
#include <Veng/Diagnostics/TraceSink.h>

#include <atomic>
#include <chrono>

// Veng/Diagnostics/Profiler.h — the engine's CPU instrumentation surface.
//
// Call sites use only the VE_PROFILE_* macros below; everything else here is the
// subsystem they drive. Under VE_PROFILE=OFF every macro body expands to nothing
// and no event-recording or buffer code is compiled — the Profiler lifecycle
// surface remains as documented no-ops so consumers and tools build unchanged.
//
// The profiler owns per-thread buffers written with no mutex and no allocation on
// the hot path; it never touches Veng/Log.h (whose sink is single-threaded), and
// it timestamps events with a steady raw-tick counter (NowTicks) rather than
// Veng::time_point, which is high_resolution_clock and is not guaranteed steady.
// Steadiness is a correctness requirement for a trace. Raw ticks are stored on the
// hot path and converted to nanoseconds off it, so the per-scope path carries no
// timebase division.

namespace Veng::Diagnostics
{
    /// @brief Identifies a track a span can be emitted onto; 0 means the caller's own thread track.
    ///
    /// Recording is otherwise thread-local. A virtual track (from CreateTrack) lets
    /// one thread emit back-dated spans onto a logical track it does not run on — the
    /// GPU bridge is its first tenant.
    using TrackId = u32;

    /// @brief The kind of work a track carries, so a viewer can group and colour it.
    enum class TrackRole : u8
    {
        /// @brief A CPU thread's own spans.
        Cpu,
        /// @brief GPU work, emitted back-dated onto a virtual track from the main thread.
        Gpu,
        /// @brief Anything else a consumer wants a separate lane for.
        Custom,
    };

    /// @brief The profiler's operating mode. Off records nothing; Ring retains the last N seconds.
    ///
    /// This is the whole domain the diagnostics core ships: a null sink and an
    /// in-memory test sink have nothing to capture into beyond a live ring. The
    /// on-disk capture modes are added where the file sink lands.
    enum class ProfilerMode : u8
    {
        /// @brief No buffer writes; aggregates still accumulate under VE_PROFILE=ON.
        Off,
        /// @brief Retain the most recent chunks per thread, discarding whole chunks on wrap.
        Ring,
    };

    /// @brief Construction parameters for a Profiler; every field is a tuning knob with a memory cost.
    ///
    /// Per-thread buffer memory is ChunkBytes * ChunksPerThread, reserved lazily as
    /// each thread first records. The whole-buffer ceiling is that times MaxThreads.
    struct ProfilerConfig
    {
        /// @brief Bytes in one buffer chunk. A chunk is the granularity handed to the sink and the
        /// unit discarded on ring wrap. Cost: this many bytes per live chunk.
        u32 ChunkBytes = 64 * 1024;

        /// @brief Chunks retained per thread. The per-thread ring depth. Cost: ChunkBytes * this per
        /// recording thread.
        u32 ChunksPerThread = 4;

        /// @brief Seconds of history the ring aims to retain; a buffer-sizing input honoured only to
        /// within one chunk, since wrapping discards a whole chunk at a time. Consumed by capture
        /// policy, not by this core.
        f64 RingDurationSeconds = 5.0;

        /// @brief Maximum threads that may register concurrently. A further registration is accounted
        /// as an overflow rather than growing the registry. Cost: bounds the buffer ceiling above.
        u32 MaxThreads = 64;

        /// @brief Strings reserved in the interning table up front, to keep steady-state interning
        /// allocation-free. Cost: roughly this many string slots.
        u32 StringTableCapacity = 4096;

        /// @brief Ceiling on retained captures, so captures cannot accumulate without bound. Consumed
        /// by capture policy; the core carries it as a documented limit.
        u32 RetainedCaptureCap = 16;

        /// @brief The mode the profiler starts in.
        ProfilerMode InitialMode = ProfilerMode::Off;
    };

    /// @brief The last completed frame's rolled-up cost for one scope name.
    ///
    /// A scope that did not run in the reported frame still appears, with CallCount
    /// zero and LastActiveFrame naming the frame it last ran in — an intermittent
    /// scope is distinguishable from one that ran and cost nothing, which a naive
    /// last-frame aggregate misreports as free.
    struct ScopeAggregate
    {
        /// @brief The scope's interned name id.
        NameId Name = 0;
        /// @brief Times the scope was entered in the reported frame; 0 if it did not run.
        u64 CallCount = 0;
        /// @brief Total wall time between enter and exit, including children, in nanoseconds.
        u64 InclusiveNanos = 0;
        /// @brief Inclusive time minus time attributed to nested child scopes, in nanoseconds.
        u64 SelfNanos = 0;
        /// @brief Index of the most recent frame in which this scope actually ran.
        u64 LastActiveFrame = 0;
    };

    class Profiler;

#if defined(VE_PROFILE) && VE_PROFILE
    namespace Detail
    {
        /// @brief Opaque per-profiler recording state; defined in the implementation.
        struct ProfilerState;
    }
#endif

    /// @brief RAII handle unlinking a thread from a profiler's registry on destruction.
    ///
    /// RegisterThread hands one back; its destructor flushes the thread's remaining
    /// bytes and removes it from the registry. This is not optional bookkeeping —
    /// a transient worker thread whose thread_local buffer is destroyed at thread
    /// exit would otherwise leave the registry walking a dangling pointer. Move-only.
    class ProfilerThreadRegistration
    {
    public:
        /// @brief Constructs an inert registration referencing no thread.
        ProfilerThreadRegistration() = default;
        /// @brief Unlinks the thread from its profiler, if this registration owns one.
        ~ProfilerThreadRegistration();

        ProfilerThreadRegistration(const ProfilerThreadRegistration&) = delete;
        ProfilerThreadRegistration& operator=(const ProfilerThreadRegistration&) = delete;

        /// @brief Takes ownership of another registration's thread link.
        ProfilerThreadRegistration(ProfilerThreadRegistration&& other) noexcept;
        /// @brief Takes ownership of another registration's thread link, releasing this one first.
        ProfilerThreadRegistration& operator=(ProfilerThreadRegistration&& other) noexcept;

        /// @brief Returns the track id assigned to the registered thread, or 0 when inert.
        [[nodiscard]] ThreadId GetThreadId() const { return m_Thread; }

    private:
        friend class Profiler;
        ProfilerThreadRegistration(Profiler* owner, ThreadId thread)
            : m_Owner(owner), m_Thread(thread)
        {
        }

        /// @brief The owning profiler, or null when inert.
        Profiler* m_Owner = nullptr;
        /// @brief The registered thread's track id, or 0 when inert.
        ThreadId m_Thread = 0;
    };

    /// @brief The CPU profiler subsystem: lifecycle, sink management, tracks, and live aggregates.
    ///
    /// One instance is owned by the application and threaded in explicitly — there is
    /// no singleton. Constructing one installs it as the process's active profiler,
    /// which is how the argument-free scope macros reach recording state; a second
    /// concurrent instance is not supported. Under VE_PROFILE=OFF the class is a shell
    /// of documented no-ops holding no recording storage.
    class Profiler
    {
    public:
        /// @brief Constructs the profiler, registers the calling (main) thread, and installs it active.
        /// @param config  Buffer, threading, and mode tuning.
        explicit Profiler(const ProfilerConfig& config = {});

        /// @brief Flushes outstanding chunks, detaches every thread, and uninstalls the active profiler.
        ~Profiler();

        Profiler(const Profiler&) = delete;
        Profiler& operator=(const Profiler&) = delete;

        /// @brief Sets the sink completed chunks and string deltas are handed to.
        ///
        /// A null sink is the default and the no-recording resting state. Passing
        /// nullptr restores that.
        /// @param sink  The sink, or nullptr for the null sink.
        void SetSink(TraceSink* sink);

        /// @brief Returns the current sink, or nullptr when the null sink is active.
        [[nodiscard]] TraceSink* GetSink() const;

        /// @brief Sets the operating mode (Off or Ring).
        /// @param mode  The mode to switch to.
        void SetMode(ProfilerMode mode);

        /// @brief Returns the current operating mode.
        [[nodiscard]] ProfilerMode GetMode() const;

        /// @brief Registers the calling thread and returns the RAII handle that unlinks it.
        ///
        /// A thread that records without registering is attached lazily; registering
        /// explicitly names its track and bounds its lifetime to the returned handle.
        /// @param name  The track name for the calling thread.
        /// @return An owning registration; drop it to unlink the thread.
        [[nodiscard]] ProfilerThreadRegistration RegisterThread(string_view name);

        /// @brief Creates a virtual track spans can be emitted onto with EmitScope.
        /// @param name  The track's display name.
        /// @param role  The kind of work the track carries.
        /// @return The new track's id (always non-zero).
        [[nodiscard]] TrackId CreateTrack(string_view name, TrackRole role);

        /// @brief Interns a name and returns its stable id, for callers building records by hand.
        /// @param name  The name to intern.
        /// @return The interned id.
        [[nodiscard]] NameId InternName(string_view name);

        /// @brief Emits a back-dated span onto a track, with explicit begin/end timestamps.
        ///
        /// Unlike a scope, this does not bracket the caller's block: the timestamps
        /// are supplied, so a span measured elsewhere (the GPU) lands on its own track.
        /// The event is stamped with the current frame index; a bridge whose work
        /// retired in an earlier frame must pass that frame through the overload below.
        /// @param track       The track to emit onto; 0 emits onto the caller's thread track.
        /// @param name        The span's interned name id.
        /// @param beginTicks  Span start, in the NowTicks() trace-clock domain.
        /// @param endTicks    Span end, in the same domain.
        void EmitScope(TrackId track, NameId name, u64 beginTicks, u64 endTicks);

        /// @brief Emits a back-dated span stamped with an explicit frame index.
        ///
        /// The frame-indexing form the GPU bridge needs: a per-pass timing read back N
        /// frames after it executed is stamped with the frame that ran it, not the
        /// current one. The trace format encodes a frame earlier than a chunk's base as
        /// a small negative delta, so a back-dated span reads on the frame it belongs to.
        /// @param track       The track to emit onto; 0 emits onto the caller's thread track.
        /// @param name        The span's interned name id.
        /// @param beginTicks  Span start, in the NowTicks() trace-clock domain.
        /// @param endTicks    Span end, in the same domain.
        /// @param frameIndex  The frame index the span is stamped with.
        void EmitScope(TrackId track, NameId name, u64 beginTicks, u64 endTicks, u64 frameIndex);

        /// @brief Advances the frame index and folds the completed frame's aggregates.
        ///
        /// Called by VE_PROFILE_FRAME. Aggregation accumulates whether or not a capture
        /// is running; this fold is what publishes the last completed frame to the HUD.
        void BeginFrame();

        /// @brief Returns the current frame index (incremented by BeginFrame).
        [[nodiscard]] u64 GetFrameIndex() const;

        /// @brief Returns true when buffer writes and sink hand-off are active.
        ///
        /// This gates recording only. It deliberately does not gate aggregation, which
        /// accumulates whenever the profiler is compiled in, so the HUD is populated in
        /// the default null-sink configuration.
        [[nodiscard]] bool IsRecording() const;

        /// @brief Returns the last completed frame's aggregate for a scope, if the scope is known.
        /// @param name  The scope's interned name id.
        /// @return The aggregate, or nullopt when the name has never been seen.
        [[nodiscard]] optional<ScopeAggregate> GetScopeAggregate(NameId name) const;

        /// @brief Returns the last completed frame's aggregates for every known scope.
        [[nodiscard]] vector<ScopeAggregate> GetFrameAggregates() const;

        /// @brief Resolves an interned name id to its text, or empty when unknown.
        /// @param name  The id to resolve.
        [[nodiscard]] string_view GetName(NameId name) const;

        /// @brief Returns how many registrations were refused for exceeding MaxThreads.
        [[nodiscard]] u64 GetRegistrationOverflowCount() const;

        /// @brief Returns how many events were dropped for any reason (buffer full in a non-ring mode, etc.).
        [[nodiscard]] u64 GetDroppedEventCount() const;

    private:
        friend class ProfilerThreadRegistration;

#if defined(VE_PROFILE) && VE_PROFILE
        /// @brief Opaque recording state (buffers, registry, string table, aggregates); absent under VE_PROFILE=OFF.
        Unique<Detail::ProfilerState> m_State;
#endif
    };

    /// @brief Returns the process's active profiler, or nullptr when none is installed.
    ///
    /// Constructing a Profiler installs it as the active one; destroying it uninstalls it.
    /// This lets code with no Profiler reference in hand — a subsystem interning a name at
    /// construction, the GPU bridge — reach the live instance. Returns nullptr under
    /// VE_PROFILE=OFF, where no recording state exists. Not for the hot path: the
    /// argument-free scope macros resolve their thread state directly.
    [[nodiscard]] Profiler* GetActiveProfiler() noexcept;

    /// @brief Reads the steady trace clock as a raw tick count — the profiler's timestamp source.
    ///
    /// This is the one place the clock source is chosen, so a caller building a span by
    /// hand (EmitScope) stamps it from the same counter. Ticks, not nanoseconds, are what
    /// the hot path stores: the read is a single instruction on Apple Silicon, and the
    /// nanosecond conversion (TraceTicksToNanos) is kept off the per-scope path. The
    /// counter is monotonic and runs at a fixed frequency (TraceTickFrequency); its epoch
    /// is arbitrary, so only differences carry meaning.
    [[nodiscard]] inline u64 NowTicks() noexcept
    {
#if defined(__aarch64__)
        // The ARM generic-timer virtual counter (CNTVCT_EL0): a monotonic, fixed-frequency
        // counter readable from user space in one instruction — the same counter
        // mach_absolute_time() reads on Apple Silicon, without the library-call overhead.
        u64 ticks;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(ticks));
        return ticks;
#else
        // Portable fallback: the steady clock in its native tick period. steady_clock, not
        // high_resolution_clock, because steadiness is a correctness requirement for a trace.
        return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    /// @brief Ticks per second of the NowTicks() counter; its resolution is one over this.
    ///
    /// The trace file format (and any decoder) records this to convert stored ticks to
    /// wall time. On Apple Silicon it is the 24 MHz architected timer, so one tick is
    /// ~41.7 ns.
    [[nodiscard]] u64 TraceTickFrequency() noexcept;

    /// @brief Converts a NowTicks() tick count or delta to nanoseconds. Off the hot path.
    /// @param ticks  A tick count or tick delta from NowTicks().
    /// @return The equivalent nanoseconds.
    [[nodiscard]] u64 TraceTicksToNanos(u64 ticks) noexcept;
}

// ---------------------------------------------------------------------------
// The macro vocabulary and its hot-path support.
//
// Under VE_PROFILE=OFF every macro below expands to nothing, and none of the
// recording types or Detail entry points are declared. This is the property the
// gate is measured against: no recording-path symbols, no per-thread buffer
// storage.
// ---------------------------------------------------------------------------

#if defined(VE_PROFILE) && VE_PROFILE

namespace Veng::Diagnostics::Detail
{
    /// @brief Per-thread recording state; defined in the implementation, opaque here.
    struct ProfilerState;
    struct ThreadState;

    /// @brief Returns the calling thread's recording state, lazily attaching it to the active profiler.
    /// @return The thread state, or nullptr when no profiler is active.
    [[nodiscard]] ThreadState* CurrentThreadState() noexcept;

    /// @brief Per-call-site cache of a string literal's interned id, guarding it by profiler generation.
    ///
    /// One static instance sits at each VE_PROFILE_SCOPE("literal") site. The id is
    /// resolved on first execution and reused thereafter, so the steady state of the
    /// literal path is a cached read rather than a hash lookup. The generation stamp
    /// re-resolves the id if the active profiler is torn down and replaced (ids belong
    /// to a profiler's string table). Shared across threads: the id is a profiler-global
    /// value, so a resolve on any thread serves the rest. Constant-initialised, so the
    /// static carries no dynamic-init guard.
    struct ScopeName
    {
        /// @brief Constructs the cache for a string literal with static storage duration.
        constexpr explicit ScopeName(const char* literal) noexcept
            : Literal(literal), Generation(0), Id(0)
        {
        }

        /// @brief The literal this site names.
        const char* Literal;
        /// @brief Profiler generation the cached id was resolved in; 0 until first resolved.
        std::atomic<u64> Generation;
        /// @brief The cached interned id, valid while Generation matches the active profiler.
        std::atomic<NameId> Id;
    };

    /// @brief Resolves a call site's literal name id, hitting the per-site cache in steady state.
    /// @param state  The calling thread's state.
    /// @param name   The call site's cache.
    /// @return The interned id.
    [[nodiscard]] NameId ResolveLiteralName(ThreadState* state, ScopeName& name) noexcept;

    /// @brief Interns a runtime string by hashing its contents. The costlier path.
    /// @param state  The calling thread's state.
    /// @param name   The name, valid only for the call.
    /// @return The interned id.
    [[nodiscard]] NameId InternDynamic(ThreadState* state, string_view name) noexcept;

    /// @brief Pushes an open-scope frame for self-time accounting; paired with CommitScope.
    void EnterScope(ThreadState* state) noexcept;

    /// @brief Records a completed scope: aggregates it always, and buffers it while recording.
    void CommitScope(ThreadState* state, NameId name, u64 beginNanos, u64 endNanos) noexcept;

    /// @brief Records a sampled counter value.
    void CommitCounter(ThreadState* state, NameId name, f64 value) noexcept;

    /// @brief Records a zero-duration instant.
    void CommitInstant(ThreadState* state, NameId name) noexcept;

    /// @brief Names the calling thread's track (VE_PROFILE_THREAD).
    void NameCurrentThread(const char* name) noexcept;

    /// @brief Advances the active profiler's frame (VE_PROFILE_FRAME).
    void MarkFrame() noexcept;

    /// @brief Tag selecting the runtime-string scope constructor.
    struct DynamicTag
    {
    };

    /// @brief Tag selecting the pre-interned-id scope constructor.
    struct PreinternedTag
    {
    };

    /// @brief RAII scope timer: records begin on construction and commits the span on destruction.
    ///
    /// Holds the thread state resolved once at construction, so entry and exit touch
    /// the same buffer with no second lookup. A null state (no active profiler) makes
    /// both ends inert.
    class ScopeTimer
    {
    public:
        /// @brief Times a scope named by a call site's cached literal.
        /// @param name  The call site's ScopeName cache.
        explicit ScopeTimer(ScopeName& name) noexcept : m_State(CurrentThreadState())
        {
            if (m_State)
            {
                m_Name = ResolveLiteralName(m_State, name);
                EnterScope(m_State);
                m_Begin = NowTicks();
            }
        }

        /// @brief Times a scope named by a runtime string.
        /// @param name  The name, valid for the constructor call.
        ScopeTimer(string_view name, DynamicTag) noexcept : m_State(CurrentThreadState())
        {
            if (m_State)
            {
                m_Name = InternDynamic(m_State, name);
                EnterScope(m_State);
                m_Begin = NowTicks();
            }
        }

        /// @brief Times a scope named by a name id interned once, ahead of the hot path.
        ///
        /// The per-frame form for a name whose interning would allocate: the caller resolves
        /// the id when its work is built and reuses it every frame, so the scope costs no hash
        /// lookup. A zero id (no active profiler at interning time) records under "no name".
        /// @param name  The pre-interned name id.
        ScopeTimer(NameId name, PreinternedTag) noexcept : m_State(CurrentThreadState())
        {
            if (m_State)
            {
                m_Name = name;
                EnterScope(m_State);
                m_Begin = NowTicks();
            }
        }

        /// @brief Commits the span if a profiler was active at construction.
        ~ScopeTimer()
        {
            if (m_State)
            {
                CommitScope(m_State, m_Name, m_Begin, NowTicks());
            }
        }

        ScopeTimer(const ScopeTimer&) = delete;
        ScopeTimer& operator=(const ScopeTimer&) = delete;
        ScopeTimer(ScopeTimer&&) = delete;
        ScopeTimer& operator=(ScopeTimer&&) = delete;

    private:
        /// @brief The thread's recording state, or null when no profiler is active.
        ThreadState* m_State;
        /// @brief The scope's interned name id.
        NameId m_Name = 0;
        /// @brief The begin timestamp, in NowTicks() ticks.
        u64 m_Begin = 0;
    };
}

/// @cond
#define VE_PROFILE_DETAIL_CONCAT_INNER(a, b) a##b
#define VE_PROFILE_DETAIL_CONCAT(a, b) VE_PROFILE_DETAIL_CONCAT_INNER(a, b)
#define VE_PROFILE_DETAIL_UNIQUE(prefix) VE_PROFILE_DETAIL_CONCAT(prefix, __LINE__)
/// @endcond

/// @brief Times the enclosing block under a compile-time name. The default, cheapest scope.
#define VE_PROFILE_SCOPE(name)                                                                     \
    static ::Veng::Diagnostics::Detail::ScopeName VE_PROFILE_DETAIL_UNIQUE(veProfName_){name};     \
    const ::Veng::Diagnostics::Detail::ScopeTimer VE_PROFILE_DETAIL_UNIQUE(veProfScope_)           \
    {                                                                                              \
        VE_PROFILE_DETAIL_UNIQUE(veProfName_)                                                      \
    }

/// @brief Times the enclosing block under a runtime name; the more expensive interning path.
#define VE_PROFILE_SCOPE_DYNAMIC(name)                                                             \
    const ::Veng::Diagnostics::Detail::ScopeTimer VE_PROFILE_DETAIL_UNIQUE(veProfScope_)           \
    {                                                                                              \
        (name), ::Veng::Diagnostics::Detail::DynamicTag {}                                         \
    }

/// @brief Times the enclosing block under a name id interned once, off the hot path.
#define VE_PROFILE_SCOPE_ID(nameId)                                                                \
    const ::Veng::Diagnostics::Detail::ScopeTimer VE_PROFILE_DETAIL_UNIQUE(veProfScope_)           \
    {                                                                                              \
        (nameId), ::Veng::Diagnostics::Detail::PreinternedTag {}                                   \
    }

/// @brief Times the enclosing function under its own name.
#define VE_PROFILE_FUNCTION() VE_PROFILE_SCOPE(__func__)

/// @brief Marks a frame boundary and advances the frame index every later event carries.
#define VE_PROFILE_FRAME() ::Veng::Diagnostics::Detail::MarkFrame()

/// @brief Samples a numeric series under a compile-time name.
#define VE_PROFILE_COUNTER(name, value)                                                            \
    do                                                                                             \
    {                                                                                              \
        static ::Veng::Diagnostics::Detail::ScopeName veProfName_{name};                           \
        auto* veProfState = ::Veng::Diagnostics::Detail::CurrentThreadState();                     \
        if (veProfState)                                                                           \
        {                                                                                          \
            ::Veng::Diagnostics::Detail::CommitCounter(                                            \
                veProfState,                                                                       \
                ::Veng::Diagnostics::Detail::ResolveLiteralName(veProfState, veProfName_),         \
                (value));                                                                          \
        }                                                                                          \
    } while (false)

/// @brief Marks a zero-duration point under a compile-time name.
#define VE_PROFILE_INSTANT(name)                                                                   \
    do                                                                                             \
    {                                                                                              \
        static ::Veng::Diagnostics::Detail::ScopeName veProfName_{name};                           \
        auto* veProfState = ::Veng::Diagnostics::Detail::CurrentThreadState();                     \
        if (veProfState)                                                                           \
        {                                                                                          \
            ::Veng::Diagnostics::Detail::CommitInstant(                                            \
                veProfState,                                                                       \
                ::Veng::Diagnostics::Detail::ResolveLiteralName(veProfState, veProfName_));        \
        }                                                                                          \
    } while (false)

/// @brief Names the calling thread's track once, at thread start.
#define VE_PROFILE_THREAD(name) ::Veng::Diagnostics::Detail::NameCurrentThread(name)

#else // VE_PROFILE off — every macro body expands to nothing.

#define VE_PROFILE_SCOPE(name)
#define VE_PROFILE_SCOPE_DYNAMIC(name)
#define VE_PROFILE_SCOPE_ID(nameId)
#define VE_PROFILE_FUNCTION()
#define VE_PROFILE_FRAME()
#define VE_PROFILE_COUNTER(name, value)
#define VE_PROFILE_INSTANT(name)
#define VE_PROFILE_THREAD(name)

#endif
