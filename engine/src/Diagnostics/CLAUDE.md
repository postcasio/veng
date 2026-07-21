# Diagnostics — the CPU instrumentation subsystem

`Veng/Diagnostics/` is the engine's profiler: a scope/counter/instant vocabulary, per-thread trace
buffers that allocate nothing and lock nothing on the hot path, a `TraceSink` seam an emitter plugs
into, and the `VE_PROFILE` compile gate that removes all of it. The namespace is `Veng::Diagnostics`.

Call sites use only the `VE_PROFILE_*` macros; everything else is the subsystem they drive.

<!-- planset-75 plan 00 -->
## The core (plan 00)

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
  plan 02's call sites resolve those once at construction. New strings are published to the sink as
  `StringTableDelta`s.

### The trace clock

`NowTicks()` is the single timestamp source. Every stored timestamp — chunk `TimestampBase`, record
deltas, the values `EmitScope` takes — is in this **raw tick domain**; nanoseconds appear only after
`TraceTicksToNanos`, which the file format (plan 01) and any decoder apply with the recorded frequency.

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
plan 01). A byte ring would overwrite the base its surviving deltas are relative to and leave
variable-width records torn at an unlocatable boundary; a chunk ring cannot. The cost, documented
rather than hidden, is that the ring's configured duration is honoured only **to within one chunk**.

Two drain behaviours, both built here; the *policy* that selects between them is plan 03:

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
`FileTraceSink` is plan 01.

### The gate

`VE_PROFILE` is a CMake option, ON under `VE_DEBUG` and OFF otherwise, and a **`PUBLIC` compile
definition on the `veng` target** — owned by the engine, propagated to every consumer, **never set by
a consumer** (a consumer whose macro expansion disagrees with the engine it links is an ABI split over
shared profiler state). Under `OFF` the `Profiler` lifecycle surface remains as documented no-ops so
consumers and tools build unchanged, but **no event-recording or buffer code compiles or links** and
the class holds no recording storage.
<!-- /planset-75 plan 00 -->

<!-- planset-75 plan 01 -->
<!-- The trace format and FileTraceSink. -->
<!-- /planset-75 plan 01 -->

<!-- planset-75 plan 02 -->
<!-- Instrumenting the spine: the GPU accessor bridge and the call sites. -->
<!-- /planset-75 plan 02 -->

<!-- planset-75 plan 03 -->
<!-- Capture control: BeginCapture/EndCapture, the ring dump, the MCP tools. -->
<!-- /planset-75 plan 03 -->

<!-- planset-75 plan 07 -->
<!-- Verification, overhead numbers, the separation sweep. -->
<!-- /planset-75 plan 07 -->
