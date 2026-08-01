# Diagnostics — the CPU instrumentation subsystem

`Veng/Diagnostics/` is the engine's profiler: a scope/counter/instant vocabulary, per-thread trace
buffers that allocate nothing and lock nothing on the hot path, a `TraceSink` seam an emitter plugs
into, and the `VE_PROFILE` compile gate that removes all of it. The namespace is `Veng::Diagnostics`.

Call sites use only the `VE_PROFILE_*` macros; everything else is the subsystem they drive.

## The core

### The macro vocabulary

`Veng/Diagnostics/Profiler.h` defines the only surface call sites touch:

- `VE_PROFILE_SCOPE(name)` — an RAII scope over the enclosing block; `name` is a string **literal**.
  The cheapest and default form.
- `VE_PROFILE_SCOPE_DYNAMIC(name)` — the `string_view` variant, for a name known only at runtime. It
  hashes its contents on every call and is the deliberately costlier path; prefer the literal form.
- `VE_PROFILE_FUNCTION()` — `VE_PROFILE_SCOPE` over the enclosing function name.
- `VE_PROFILE_FRAME()` — marks a frame boundary; folds the completed frame's aggregates and advances
  the frame index every later event carries.
- `VE_PROFILE_COUNTER(name, value)` — a sampled `f64` series.
- `VE_PROFILE_INSTANT(name)` — a zero-duration marked point.
- `VE_PROFILE_THREAD(name)` — names the calling thread's track once, at thread start.

Under `VE_PROFILE=OFF` **every macro body expands to nothing** — no empty inline, no disabled branch
— and none of the recording types or `Detail` entry points are declared. See the gate below.

### The hot-path invariants

The design constraint is that a scope costs **≤ 40 ns and allocates nothing**, a budget that
*includes* the release store below. What holds it:

- **No `Log.h`, ever.** The log sink is single-threaded and `fflush`es each record; a worker-thread
  emitter riding it would be a data race and ruinously slow. The profiler owns its own buffers and
  never touches `Log`.
- **The clock is a steady raw-tick counter, not `Veng::time_point`.** `Time.h`'s alias is
  `high_resolution_clock`, which on some standard libraries aliases `system_clock` and can jump.
  Steadiness is a correctness requirement for a trace, so the source is pinned here (`NowTicks()`) —
  see [The trace clock](#the-trace-clock) for the source, resolution, and epoch. The hot path stores
  **raw ticks**; the nanosecond conversion (`TraceTicksToNanos`) runs only off it, at the frame fold
  and at decode, so a scope carries no timebase division.
- **Recording is thread-local.** Each recording thread owns a `ThreadState` obtained on first use —
  lazily on the first event, or explicitly through `RegisterThread`. Events append by pointer bump
  into the thread's current chunk: no mutex, no allocation, no contended atomic.
- **The publication protocol.** Lock-free is not synchronization-free. Each chunk carries an
  `atomic<u32>` write offset the recording thread **stores with release on each commit**, and a
  collector **loads with acquire** before reading any byte below it; the recording thread's own reads
  of its offset are relaxed. This is the sum total of the hot path's synchronization, and the ≤ 40 ns
  budget is measured with the release store in place. The frame index is atomic for the same reason —
  it is read on the hot path while `BeginFrame` advances it.
- **String interning is cached per call site.** `VE_PROFILE_SCOPE("Foo")` places a constant-initialised
  `static ScopeName` at the call site; the interned id is resolved once (on first execution) and reused
  for every later call, guarded by the profiler generation so a torn-down-and-replaced profiler
  re-resolves. The id is a profiler-global value, so one thread's resolve serves the rest — steady
  state is a cached read, not a hash lookup. The dynamic form (`VE_PROFILE_SCOPE_DYNAMIC`) hashes
  contents through the shared, mutex-guarded string table on every call and is the costlier path;
  the engine's own high-cardinality call sites resolve those once at construction. New strings are published to the sink as
  `StringTableDelta`s.

### The trace clock

`NowTicks()` is the single timestamp source. Every stored timestamp — chunk `TimestampBase`, record
deltas, the values `EmitScope` takes — is in this **raw tick domain**; nanoseconds appear only after
`TraceTicksToNanos`, which the file format and any decoder apply with the recorded frequency.

- **Source.** On `__aarch64__` (the primary Apple-Silicon target) it reads the ARM generic-timer
  virtual counter `CNTVCT_EL0` in one instruction (`mrs`) — the same counter `mach_absolute_time()`
  reads, without the library-call cost (~1.2 ns vs ~6 ns vs ~17 ns for `steady_clock::now()`). Every
  other platform falls back to `std::chrono::steady_clock` in its native period. Both are monotonic
  and steady; `high_resolution_clock` is rejected precisely because it is not guaranteed steady.
- **Resolution.** `TraceTickFrequency()` reports ticks per second — `CNTFRQ_EL0`, **24 MHz on Apple
  Silicon**, so one tick is **~41.7 ns**. Sub-41 ns durations are therefore inherently coarse: the
  per-scope budget is validated as an **amortised** cost over many invocations, not as a single
  sub-tick measurement.
- **Epoch.** The counter's origin is arbitrary (a boot-relative count), so **only differences carry
  meaning**. A trace records the frequency and its own base; absolute wall-clock alignment is not
  implied and is not part of the contract.

### Buffers are rings of chunks, never rings of bytes

Each thread holds `ChunksPerThread` fixed-size chunks in a circular array. Every chunk is
**self-contained**: its own absolute timestamp base, the offset of its first record, and a monotonic
sequence number (`TraceFormat.h`, the internal provisional encoding — the normative on-disk format is
[docs/trace-format.md](../../../docs/trace-format.md)). A byte ring would overwrite the base its surviving deltas are relative to and leave
variable-width records torn at an unlocatable boundary; a chunk ring cannot. The cost, documented
rather than hidden, is that the ring's configured duration is honoured only **to within one chunk**.

Two drain behaviours; the *policy* that selects between them is [Capture control](#capture-control):

- **Streaming** (a non-null sink attached): when a chunk fills it is sealed, handed to the sink whole
  via `OnChunk`, and reused in place. No loss.
- **Ring** (null sink): a filled chunk is retained; when the ring wraps onto a chunk that still holds
  un-drained records, that whole chunk is **discarded** and the drop counter incremented.

### Recording, aggregation, and the two gates

`IsRecording()` is `GetMode() != ProfilerMode::Off`; it gates **buffer writes and sink hand-off, and
nothing else**. It deliberately does **not** gate aggregation: under `VE_PROFILE=ON` the per-frame
per-scope aggregates accumulate **always**, capture or not, because a null sink is the default resting
state and gating them on recording would leave the HUD blank in the default configuration.

**Aggregation is lock-free, per thread, folded once per frame.** Each recording thread accumulates its
own per-scope **monotonic tick totals** (call count, inclusive, self) into thread-local storage — no
lock, no shared mutable state on the hot path, matching the per-thread-then-publish discipline the
event buffers already use. The thread is the sole writer of its own totals, so a plain relaxed add (a
load and a store, never a read-modify-write) suffices. Storage is a per-thread array of accumulators
indexed by `NameId`, grown in fixed, pointer-stable blocks; the owner releases a published id count
the fold acquire-reads, so the fold never reads an entry mid-placement. `BeginFrame` (single-threaded)
walks every thread's published accumulators, **diffs each total against what it last folded** (a
folder-only shadow, so the fold never writes the producer's counters — no reset race), converts the
tick deltas to nanoseconds, and merges them into a double-buffered snapshot the panel reads. A
thread's un-folded remainder is dropped when it detaches, exactly as an un-drained ring chunk is.
**An intermittent scope stays visible** with a zero call count and the *last frame it ran in* carried
alongside, so a checkpoint that fires every few seconds is distinguishable from one that ran and cost
nothing. The fold's cost is a per-frame cost, separate from the per-scope budget.

### Thread registration is RAII

`RegisterThread` returns a `ProfilerThreadRegistration` whose destructor flushes the thread's
remaining bytes and unlinks it from the registry. A transient `ParallelFor` thread whose
`thread_local` buffer dies at thread exit must not leave the registry walking a dangling pointer, so a
lazily attached thread is also unlinked at thread exit. The contract: **a profiler outlives every
thread that registered with it, except the main thread**, which it registers at construction and
detaches at destruction. Registration beyond `MaxThreads` is **accounted** (a counter), never fatal;
a dropped event increments a separate counter. A truncated capture that says so beats one that looks
complete.

### Virtual tracks and the seam

Recording is thread-local, but a bridge running on one thread may need to emit onto another logical
track (the GPU bridge is the case this exists for). `CreateTrack(name, role)` mints a `TrackId`, and
`EmitScope(track, name, begin, end)` emits a back-dated span with explicit timestamps rather than
bracketing the caller's block. The `TraceSink` seam takes **completed chunks**, not events —
`OnChunk`/`OnStrings`/`OnFlush`/`OnClose` — which is what keeps the sink off the hot path. The core
ships a `NullTraceSink` (the default) and a `CapturingTestSink` (retains chunks in memory for tests);
`FileTraceSink` is the on-disk sink described below.

**`CapturingTestSink` is declared in a public header, and that placement is a known wart.**
`Veng/Diagnostics/TraceSink.h` sits behind `Profiler.h` and reaches most of the tree, so a
test-support class rides the public profiling surface. Its method bodies are out-lined into
`src/Diagnostics/TraceSink.cpp`, so it costs nothing to a TU that does not construct it — but the
*placement* question is untouched: moving it is a design change with callers to migrate rather
than a cost fix.

### The gate

`VE_PROFILE` is a CMake option, ON under `VE_DEBUG` and OFF otherwise, and a **`PUBLIC` compile
definition on the `veng` target** — owned by the engine, propagated to every consumer, **never set by
a consumer** (a consumer whose macro expansion disagrees with the engine it links is an ABI split over
shared profiler state). Under `OFF` the `Profiler` lifecycle surface remains as documented no-ops so
consumers and tools build unchanged, but **no event-recording or buffer code compiles or links** and
the class holds no recording storage.

## The trace format and `FileTraceSink`

The on-disk shape of a capture is a compact binary stream, specified **normatively** in
[docs/trace-format.md](../../../docs/trace-format.md) — the single source of truth both this
repo's `vengtrace` converter and any out-of-tree decoder are written against. The internal
`TraceFormat.h` encoding the profiler writes into its buffers is *not* that format: it is a
fixed-width provisional record the sink transcodes into the compact on-disk stream. The two are
deliberately distinct — the buffer encoding is cheap to append on the hot path, the on-disk encoding
is small to store and read.

**The stream.** A fixed 40-byte preamble (magic `VENGTRAC`, format version, the trace clock's tick
frequency and base, capture mode, build config, `VE_PROFILE` state) followed by length-prefixed
typed sections: `Metadata`, `StringTable`, `Track`, `Chunk`, `Accounting`, and a `Trailer` written
only on a clean close. A reader skips a section type it does not know (by its length prefix) and
stops at the first section it cannot fully read, so a **truncated capture reads up to its last
complete section** and a new section type is addable without a version bump. The event record layout
is what the version pins.

**Ticks, not nanoseconds.** Timestamps are raw trace-clock ticks (`NowTicks`); the preamble records
`TraceTickFrequency()` (24 MHz on Apple Silicon) and a tick base, and a decoder converts with
`TraceTicksToNanos`. Every chunk carries its own **absolute** tick base and a monotonic sequence
number, so it decodes standalone — and a ring dump's first chunk is not sequence 0, the gap being
how a discarded span reads as a gap rather than silence. Per-record timestamps are variable-width
tick deltas from the chunk base; per-record frame indices are zigzag deltas from a chunk base frame,
so a **back-dated GPU span** (an earlier frame than the chunk's) encodes as a small negative.

**Counter values are lossless and compact.** A one-byte tag selects `varint u64`, `zigzag varint
i64`, or raw `f64`, and the writer picks the narrowest form that round-trips the `f64` **bit-for-bit**
(`TraceFileFormat::SelectCounterTag`) — so −0.0 and NaN fall to the raw form rather than being
flattened by an integer encoding.

**Identity, enumerated.** The `Metadata` section carries only the engine version, the executable
**basename** (never its path), and one git-provenance entry per submodule (short SHA + dirty flag).
No absolute path, environment variable, command line, or host name is ever written — the engine's
own provenance is compiled into the `FileTraceSink` TU at configure time.

**`FileTraceSink`** (`Veng/Diagnostics/FileTraceSink.h`, `TraceFile.h`/`.cpp` the internal encoder)
is the `TraceSink` that writes this stream. `Create(path, ringDump)` returns the sink; chunks and
string deltas are **copied and queued to a dedicated writer thread**, which does the transcoding and
encoding off the recording thread, so the hot-path invariant holds through the sink. The finished
stream is committed with `assetpack`'s `WriteFileAtomic`, so a scanner never sees a half-written
capture — a ring dump is a complete file the instant it appears. Captures land in
**`<build-dir>/captures/`** (gitignored with the build tree, orderable and attributable to the build
that produced them), never `UserDataDir`. The dropped-event/dropped-thread counts the profiler owns
are stamped through `SetAccounting` before close, so a lossy capture is visibly lossy.

**The reference fixture** (`tests/fixtures/trace-fixture.vtrace`, plus a truncated sibling) is a
committed capture covering every section and record type, all three counter encodings, a first chunk
whose sequence is not 0, a full string table, and a back-dated span — the shared conformance input
both decoders are tested against. It is built from clean synthetic values (no path, no host string),
and the round-trip test regenerates it and compares byte-for-byte, so it cannot drift from the
format. Regenerate with `VENG_REGEN_TRACE_FIXTURE=1` on the unit suite.

## The instrumented spine

The call sites that make a capture worth taking, plus the seam and bridge that place GPU work on it.

### What the engine records

- **The frame spine.** `Application::Frame()` opens with `VE_PROFILE_FRAME()` (the boundary marker,
  not a scope) and wraps each phase in a stable-named scope in the order the frame runs them:
  `Frame/RequestDrain`, `TaskSystem/PumpMainThread`, `Frame/AssetFinalize`, `Frame/Input`,
  `Frame/ImGui`, `WorldRunner/Tick`, the net pumps, `Frame/Update`, `Frame/ViewPush`,
  `Frame/RenderBegin`, `Frame/Render` (per viewport, dynamic), `Frame/OnRender`, `Frame/Composite`,
  `Frame/RenderEnd`. **The names are stable strings** — the HUD and the flamegraph key on them.
- **Simulation, per world and per system.** `WorldRunner::Tick` carries an outer scope; each world's
  Sim and View phases get a dynamic scope named by the world's identity, and the Sim phase records
  a `WorldRunner/SimSteps` counter (the fixed-step catch-up count). `SceneSimulation` **retains each
  system's registered name, interned once at construction** (`SystemNameOf` returns a `string` by
  value, so resolving it per frame would allocate on the hottest instrumentation path), and scopes
  each system over that pre-interned id through `VE_PROFILE_SCOPE_ID`. This per-system loop is the
  one deliberate high-cardinality site — the systems *are* the phases there.
- **The task pool.** `TaskSystem::Submit` gains an optional job name (defaulted, so every call site
  compiles); the queue element is a `QueuedJob` struct declared in every configuration, its
  profiling fields conditional on `VE_PROFILE`. Each job's execution is scoped on its worker (named
  by the job), `PumpMainThread` is scoped, workers name their tracks, and `ParallelFor` scopes its
  region and each range and registers its transient threads. The counters the pool never had:
  **queue depth** and **main-thread queue depth** (atomics maintained under the lock the
  enqueue/dequeue already hold, read lock-free at the sample point), **active jobs** (atomic), and
  **job latency** (submit→start, from a submit timestamp taken *only while recording*).
  `TryGetCurrentWorkerIndex()` is the non-asserting worker query (returns `NotAWorker` off a
  worker), leaving the asserting `GetCurrentWorkerIndex()` untouched. `Application` samples the
  pool counters once per frame at the frame boundary (`SampleFrameCounters`).

### The GPU accessor extension and the bridge

- **`GpuPassTiming` gains placement and nesting.** Alongside `Name`/`Milliseconds` it carries
  `BeginNanos`/`EndNanos` (nanoseconds from the frame's GPU start) and `Depth` (nesting level,
  reconstructing the backend `OpenScopeStack`). The public boundary stays backend-free — the struct
  carries plain integers, resolved against `TimestampPeriodNs` the backend already knows.
  `Name`/`Milliseconds` keep their meaning, so existing readers are unchanged. This changes what the
  accessor *reports*, not how timestamps are collected: the 128-scope budget, the readback latency,
  and the `m_GpuScopeRecording` gate are untouched.
- **The bridge back-dates.** `Application::BridgeGpuTimings()` runs once per frame after
  `Context::EndFrame()`, reads the timings **through the public `Context` accessors only**, and
  emits each pass as a scope-shaped event onto a virtual GPU track (`CreateTrack` once, then the
  five-argument `EmitScope` that stamps an explicit frame index). The readback is N frames late, so
  each event is stamped with the frame that executed it — tracked per frame-in-flight slot
  (`m_GpuSlotFrame`/`m_GpuSlotAnchorTicks`), never the current frame. `EmitScope(track, name, begin,
  end, frameIndex)` is the frame-indexing primitive the format's negative-frame-delta encoding
  exists for. Timing unsupported → no track, no error; an out-of-frame graph run records nothing.
  The fast band tests the mechanism over a fake source; the live alignment result is under
  [Verification](#verification) below.

### The track-descriptor seam

`TraceSink` carries **`OnTrack(const TrackDescriptor&)`** (the track analogue of `OnStrings`), and
the `Profiler` calls it when a thread is named
(`RegisterThread`/`VE_PROFILE_THREAD`), when a virtual track is created (`CreateTrack`), and — for
tracks named before a capture began — replayed for every known track when a sink is attached
(`SetSink`). `FileTraceSink::OnTrack` records the delivered descriptor and prefers it over the
id-only fallback it synthesizes from chunk references, so the format's named/roled Track section
carries real names and roles. `GetActiveProfiler()` reaches the installed instance for code with no
profiler reference in hand (the per-system interning, the GPU bridge).

### Vulkan debug-utils command-buffer labels

`DebugMarkers` drops the three dead `vkCmdDebugMarker*` pointers (they belong to the never-requested
`VK_EXT_debug_marker` device extension) and loads `vkCmdBegin/End/InsertDebugUtilsLabelEXT` from the
enabled `VK_EXT_debug_utils` instance extension, exposing `BeginLabel`/`EndLabel`/`InsertLabel`
(each null-checking its own pointer). `Context::BeginGpuScope`/`EndGpuScope` emit a balanced label
region under `VE_DEBUG`, so `RenderGraph`'s per-pass auto-bracketing labels every pass for RenderDoc
and Xcode with no new call site and no per-pass cost in a shipping build.

## Capture control

The policy layer over the two buffer behaviours above. A capture is something you *start* — from
code, a key, or over MCP — that ends with a file on disk whose path you are handed back; the
continuous ring is the same buffers under a different policy, dumped after the fact.

### The `Profiler` capture API

- **`BeginCapture(path, frameCount = 0) -> VoidResult`** — switches to capturing mode and constructs
  a `FileTraceSink` at `path` (a triggered stream). A non-zero `frameCount` self-terminates the
  capture at the frame boundary after that many frames (in `BeginFrame`), the form a scripted or
  agent-driven capture wants; zero runs until `EndCapture`. Fails with a located error if a capture
  is already in flight (naming it), or if the destination directory cannot be created.
- **`EndCapture() -> Result<path>`** — flushes every thread's outstanding chunk up to its published
  write offset, hands the capture to the off-thread writer to trailer and commit, restores the
  standing ring policy, and returns the written path. Fails if no capture is running (a located
  error, never an assert — it is reachable from an agent and a keypress).
- **`SetRingEnabled(bool)`** — the standing continuous-ring policy (size = `RingDurationSeconds`). A
  capture temporarily overrides the active mode; the ring is re-derived from this flag when the
  capture ends, so enabling it mid-capture takes effect on `EndCapture`.
- **`DumpRing(path) -> Result<path>`** — walks each thread's live chunks in sequence order from the
  oldest, copying each up to its acquire-loaded write offset, and writes them with the **full**
  string table (a ring dump has no earlier delta to build on). **Recording is not suspended** —
  producers keep appending throughout — so the dump is honoured to within one chunk, and a wrap
  landing on a chunk mid-copy can tear that chunk (see [Verification](#verification)). Fails if a
  capture is running.
- **`GetState() -> CaptureState`** — off / ring / capturing, the frame budget and frames elapsed, the
  running capture's path, and `WriterDraining`.

**Off-thread finish.** `EndCapture`/`DumpRing` hand their chunks to the `FileTraceSink` writer
and return **immediately** — the calling thread's cost is collecting and copying chunks, never the
encode or I/O. `FileTraceSink::BeginClose()` initiates the trailer + atomic commit without joining,
and `HasFinishedWriting()` reports when the file is on disk; `GetState().WriterDraining` reflects
that. A caller that needs the file *finished* (the MCP `profile.stop`/`profile.dump_ring` tools)
waits on `WriterDraining`, not on a frame. The profiler reaps the drained sink lazily (in `GetState`
and the next capture op) and enforces the retention cap at that point.

**Retention.** On each reap the `RetainedCaptureCap` is enforced over the just-written file's
directory, deleting `*.vtrace` oldest-first (by mtime) until the count is under the cap.

**Lifecycle.** A self-terminating capture ends at a frame boundary in `AdvanceFrame`; `~Profiler`
closes any open capture into a trailered file (the existing sink flush + `OnClose`); every failure
path is a `Result`, so no capture request aborts. Accounting (`GetDroppedEventCount` /
`GetRegistrationOverflowCount`) is stamped into the sink via `SetAccounting` before close, so a
truncated capture is visibly lossy in the file.

**The capture directory.** `CaptureDirectory()` returns `<build-dir>/captures/`, baked at configure
time (`VENG_CAPTURE_DIR` on `Profiler.cpp`). `ResolveCapturePath(name)` places a named capture under
it (final path component only, so a name cannot escape). Both are defined regardless of the
`VE_PROFILE` gate — they are path math.

### The MCP tools — `profile.*`

`mcp/src/ProfileTools.cpp` registers, following the `noun.verb` convention:

- `profile.stats` — the live per-scope aggregates, state, and drop counters. Read-only, **always
  registered**.
- `profile.start {frames?, name?}` — begins a capture; returns immediately with the state and planned
  path (never blocks for the frame budget).
- `profile.stop` — ends the capture and returns the written path (waits on the writer).
- `profile.dump_ring {name?}` — dumps the ring and returns the path.

The three write verbs register only under `AllowMutations` (they write files), beside the mutation
and input families. The host seam is one optional `McpHost` member — `function<Profiler*()> Profiler`
— null (or returning null) makes the tools report the profiler unavailable, exactly as a null
`InjectInput` does for `input.send`. **No tool argument is ever a filesystem path**: a tool names a
capture, the engine resolves it under the capture directory, and the path travels outward in the
response.

### The consumer hotkey recipe

The engine exposes the verbs; a hotkey is a **consumer call site**, not an engine subsystem. Poll a
key in `OnUpdate` and call the verbs — `hello-triangle` binds F5/F6/F7 beside its F9–F12:

```cpp
Diagnostics::Profiler& profiler = GetProfiler();
if (GetInput().WasKeyPressed(Key::F5))
{
    if (profiler.GetState().Status == Diagnostics::CaptureStatus::Capturing)
    {
        if (const Result<path> written = profiler.EndCapture())
            Log::Info("Capture written to {}", written.value().string());
    }
    else
    {
        (void)profiler.BeginCapture(Diagnostics::ResolveCapturePath("hotkey"));
    }
}
if (GetInput().WasKeyPressed(Key::F6))
    (void)profiler.DumpRing(Diagnostics::ResolveCapturePath("ring"));
if (GetInput().WasKeyPressed(Key::F7))
    profiler.SetRingEnabled(profiler.GetState().Status != Diagnostics::CaptureStatus::Ring);
```

Every verb returns a `Result`, so a failure is logged, never fatal.

### Under `VE_PROFILE=OFF`

The capture verbs are documented no-ops returning a clear "disabled" error, and the MCP tools report
the profiler unavailable — both build unchanged, neither aborts.


## Verification

The subsystem's value rests on two properties no single test proves: that instrumenting the engine
does not distort what it measures, and that a GPU duration read back several frames late is
attributed to the frame that produced it. Both are measured, and the numbers are recorded here so
the next change to this code has a baseline rather than a claim.

`tests/bench/diag_bench.cpp` (the `diag_bench` ctest, built only under `VE_PROFILE`) is the
standing measurement: per-scope cost with recording on, per-scope cost with the profiler merely
compiled in, the per-frame aggregation fold, and a hard allocation-free assertion on the
steady-state hot path — the one thing it fails on, since a timing threshold in CI would be a
flake generator. **Measure in an optimized build.** The figures below are medians over seven runs
on an Apple M-series host, in a `VE_DEBUG=OFF` (Release, `-O3`) tree; the `VE_DEBUG=ON` tree is
`-O0` and is roughly an order of magnitude slower, which is a property of the build type, not of
the design.

| Measurement | Release (`-O3`) | Debug (`-O0`) | Budget |
|---|---|---|---|
| Per scope, recording (append + release store + aggregation) | **12.4 ns** | 146.7 ns | ≤ 40 ns |
| Per scope, compiled in but not recording (aggregation only) | **9.4 ns** | 113.6 ns | — |
| Per-frame aggregation fold, 8 distinct scopes | **737 ns** | 5,626 ns | — |
| Hot-path allocations under a recording load | **0** | 0 | 0 |

So **recording costs ~3 ns per scope over merely being compiled in** — the two are reported apart
because conflating them hides the one that matters. The per-frame fold is a per-frame cost the
per-scope budget does not cover at all and is likewise its own figure.

**A disabled build shows no frame-time regression.** Over a fixed twenty-frame headless drive of
the sample, medians of fifteen interleaved runs: 647.0 ms at the instrumented tree's
`VE_PROFILE=OFF` build against 647.2 ms at a pre-instrumentation build of the same sample — a
−0.03 % difference, inside the run-to-run spread. That drive is startup-dominated and resolves
about 150 µs per frame; the substantive evidence is the symbol check below, which shows the code
is not merely inert but absent.

**`VE_PROFILE=OFF` links no recording path.** The disabled build is *not* free of diagnostics
symbols and must not be checked as if it were — the `Profiler` lifecycle surface, the `profile.*`
MCP tool stubs and the performance panel's degraded branch all remain so consumers, tools, and
panels build unchanged. The checkable property is that **no event-recording code and no per-thread
buffer storage link**, and it holds: `Detail::ThreadState`, `CurrentThreadState`, `EnterScope`,
`CommitScope`/`CommitCounter`/`CommitInstant`/`CommitEmitScope`, `ResolveLiteralName`,
`InternDynamic`, `MarkFrame`, `NameCurrentThread`, and the whole `Detail::ProfilerState` are absent
from the `OFF` library, which carries no thread-local storage for the subsystem at all. One thing
outside that list does still link: `FileTraceSink` and the `TraceFileFormat` writer compile whole
under `OFF`, unreachable because every capture verb returns the disabled error. They are the
serializer, not the recording path, but they are dead weight in a disabled build.

**GPU spans land on the frame that executed them.** Verified over a capture of a deliberately
varied per-frame GPU load — 1,256 frames whose CPU duration ranged 8.2–99.2 ms (median 18.3) and
whose GPU frame time ranged 0–40.4 ms (median 7.1), stepped down five times by destroying batches
of drawn entities mid-capture. For each GPU span, its stamped frame index is checked against the
wall-clock window of that frame's CPU work: **100.0 % of spans begin inside the frame they are
stamped with.** The same check under an artificial constant frame-shift fails, which is what makes
it a check rather than a tautology — shifting the stamps by +1 / −1 frame leaves 42.0 % / 4.4 %
in-window, by +2 / −2 leaves 3.6 % / 0.6 %, and by +3 / −3 leaves 0.2 % / 0.0 %.

**The publication protocol is TSan-clean under a concurrent collector.** A recording load driving
the frame spine, task-pool jobs, and `ParallelFor`'s transient threads, with a second thread
repeatedly calling `DumpRing`, runs clean under ThreadSanitizer over ~4,600 frames and ~240 dumps.
A chunk's `TimestampBase` and `SequenceNumber` are atomic for the same reason its write offset is:
a collector sorts live chunks by sequence off the owning thread while the ring re-arms one under it.
Their accesses are relaxed — `WriteOffset`'s release/acquire pair supplies the ordering.

**A ring dump can still tear at a wrap.** `DumpRing` copies a live chunk up to its acquire-loaded
write offset while the ring keeps recording, so a wrap that re-arms *that* chunk mid-copy
overwrites the header the copy is reading. The window is narrow and costs at most one chunk of a
dump, and closing it needs a per-chunk generation the collector re-checks after copying.
