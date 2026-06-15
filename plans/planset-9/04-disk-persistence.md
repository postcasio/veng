# Plan 04 — Disk persistence (opt-in)

> **Stream B (pipeline cache), plan 2 of 2.** Depends on plan 03. Its sample edit shares
> `examples/hello-triangle` with stream A — land it **after** plan 02's module migration
> (see the README's *Dependencies & dispatching* section).

**Goal:** let an app persist the pipeline cache across runs by naming a file. Add
`ApplicationInfo::PipelineCachePath` (`optional<path>`); when set, seed the cache from the
file at device init and write it back at shutdown. `nullopt` keeps plan 03's in-memory
behaviour exactly. The sample opts in and the headless smoke proves the round-trip.

## Why this is its own plan

Plan 03's cache is verifiable with no file system in play; persistence is a separable
layer — file read/write plus the `ApplicationInfo` → `Context` plumbing to carry the path.
Splitting it keeps the Vulkan-object change (03) clean and contains the I/O + opt-in
surface here, where the round-trip is the thing under test.

## Public surface — `engine/include/Veng/Application.h`

```cpp
struct ApplicationInfo
{
    // ... existing fields ...

    // When set, the render context seeds its pipeline cache from this file at startup
    // (if it exists) and writes the cache back here at shutdown. nullopt (default) keeps
    // the cache in-memory only — no file is read or written. The app owns the path; veng
    // does not choose a cache directory.
    optional<path> PipelineCachePath = nullopt;
};
```

The path threads through the existing construction chain — there is **no** field for it
today, so each hop is a concrete edit:

- `ApplicationInfo::PipelineCachePath` (above).
- **`ContextInfo` gains `optional<path> PipelineCachePath;`** (`engine/include/Veng/Renderer/Context.h`, beside `ApplicationName`/`InternalRenderExtent`/…).
- **`Application::Initialize` passes it through** — `m_RenderContext.Initialize({ …, .PipelineCachePath = m_Info.PipelineCachePath }, m_Window.get())` (`engine/src/Application.cpp`, where the `ContextInfo` is built today).
- **`Context::Native` gains `optional<path> PipelineCachePath;`** (in `engine/include/Veng/Renderer/Backend/ContextNative.h`, where the struct is defined — *not* `Context.cpp`, beside the `vk::PipelineCache` field plan 03 added). `Context::Initialize` stores it (`m_Native->PipelineCachePath = info.PipelineCachePath;`).

No renderer resource API changes; `ContextInfo` is a setup struct, not a public resource surface.

## Impl — `engine/src/Renderer/Backend/Context.cpp`

At cache creation (plan 03's empty-cache step), branch on the path:

```cpp
vector<u8> initial;   // empty unless a readable file exists — read inline (binary),
                      // the same std::ifstream pattern Archive.cpp already uses; veng has
                      // no ReadFileBytes helper, so don't reference one.
if (m_Native->PipelineCachePath)
{
    std::ifstream file(*m_Native->PipelineCachePath, std::ios::binary | std::ios::ate);
    if (file)
    {
        const auto size = static_cast<usize>(file.tellg());
        initial.resize(size);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(initial.data()), static_cast<std::streamsize>(size));
    }
}

vk::PipelineCacheCreateInfo cacheInfo{
    .initialDataSize = initial.size(),
    .pInitialData    = initial.empty() ? nullptr : initial.data(),
};
m_Native->PipelineCache = m_Native->Device.createPipelineCache(cacheInfo).value;
```

A missing file is **not** an error — it is the first run; `initial` stays empty. A stale,
foreign, or truncated blob is **safe**: the driver validates the cache header
(`vendorID`/`deviceID`/`pipelineCacheUUID`) and silently ignores non-matching data,
starting cold (decision 3). veng neither parses nor validates the bytes.

At teardown, **before** destroying the cache (and the device), write it if a path is set:

```cpp
if (m_Native->PipelineCachePath)
{
    auto data = m_Native->Device.getPipelineCacheData(m_Native->PipelineCache).value;
    std::ofstream file(*m_Native->PipelineCachePath, std::ios::binary | std::ios::trunc);
    if (file)
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    else
        Log::Warn("pipeline cache write failed: {}", m_Native->PipelineCachePath->string());
}
```

The two recoverability levels are deliberate and follow veng's error policy. The
**Vulkan call** (`getPipelineCacheData`) is a Vulkan call like any other: under
`VULKAN_HPP_ASSERT_ON_RESULT` a non-success result is **fatal** (the `.value` unwrap
asserts) — a failed Vulkan call is unrecoverable per `CLAUDE.md`, not something to limp
past. The **file write** is the recoverable part: a warm cache is an optimization, not
correctness, so an unopenable/failed write logs at `WARN` and teardown continues. Do not
turn the write failure into an assert; do not pretend the Vulkan fetch is recoverable.

The cache is written **only at shutdown** (decision 4): a crash during a long first run
discards the cache it had built — the run where warming costs the most. This is an
accepted limitation of the no-periodic-flush policy, not a defect; a periodic or
after-N-new-pipelines flush is a future option, not part of this plan.

## Sample migration — `examples/hello-triangle/`

Set `PipelineCachePath` in the app's `ApplicationInfo` to
`Veng::ExecutableDirectory() / "pipeline_cache.bin"` — beside the launcher, the same
executable-relative resolution plan 02 uses for the pack, so the cache stays with the
relocatable trio. A windowed or headless run reads-then-writes it; the smoke path picks
the same file up, exercising the disk round-trip on hardware-free CI machines (it is
device-level, no swapchain needed). After stream A's plan 02 migration the sample's
`ApplicationInfo` is built inside `VengModuleRegister`, so set the path there (and
`ExecutableDirectory()` from plan 02 is available); if this plan lands before plan 02 it
goes in the old `main()` and plan 02 carries it across — hence the soft ordering (A's
sample migration first).

## Verification

- Clean build; `ctest` green.
- **Round-trip — and the cache must demonstrably hold pipelines, not just exist.** The
  sample resolves `PipelineCachePath` to `ExecutableDirectory() / "pipeline_cache.bin"`
  (beside the launcher, i.e. `build/examples/hello-triangle/pipeline_cache.bin`). Delete
  it, then run the headless smoke (the launcher) twice:
  ```sh
  PC=build/examples/hello-triangle/pipeline_cache.bin
  rm -f "$PC"
  HT_SMOKE=/tmp/ht1.ppm build/examples/hello-triangle/hello_triangle-launcher   # run 1: writes $PC
  # A populated cache is far larger than the bare 32-byte VkPipelineCacheHeaderVersionOne;
  # > 32 proves pipeline data was actually stored, not an empty-but-valid cache.
  test "$(wc -c < "$PC")" -gt 32
  S1=$(wc -c < "$PC")
  HT_SMOKE=/tmp/ht2.ppm build/examples/hello-triangle/hello_triangle-launcher   # run 2: seeds from $PC
  # Run 2 seeded from a warm cache must not start cold: the rewritten cache is at least as
  # large as run 1's (it never re-shrinks below what it already held).
  test "$(wc -c < "$PC")" -ge "$S1"
  ```
  Both runs exit 0 and write correct-sized PPMs. The size assertions are what make this a
  real round-trip test rather than a "wrote and re-read an empty file" tautology — without
  them an empty-but-valid cache passes. The PPM is non-deterministic — verify size + exit
  0, never golden-compare. (No env-var wiring: the path is baked into the sample's
  `ApplicationInfo`.)
- A run with `PipelineCachePath = nullopt` touches no file (plan 03 behaviour).
- `ctest --test-dir build-debug -L validation` green; the allowlist stays empty.

## Acceptance

- `ApplicationInfo::PipelineCachePath` opt-in works: set → seed + write; `nullopt` → no
  file I/O.
- A missing/stale/foreign cache file is handled safely (cold start, no error); a write
  failure logs and does not break teardown.
- The sample demonstrates the cross-run round-trip headless, with the written cache shown
  to hold pipeline data (size > the bare header) and to survive reseeding (run 2 ≥ run 1);
  `ctest` and the validation gate are green.
