# Plan 03 — Context-owned in-memory cache

> **Stream B (pipeline cache), plan 1 of 2.** Independent of streams A (01–02) and C
> (05–06); see the README's *Dependencies & dispatching* section.

**Goal:** give `Context` a `vk::PipelineCache` and thread it into both pipeline-creation
sites, with **no disk involvement** — an in-memory cache shared across every pipeline
built in a run. This is the isolated mechanism change; persistence (plan 04) layers file
I/O on top without touching the creation sites again.

## Why this is its own plan

The cache object + the two creation-site edits are verifiable entirely on their own: the
rendered scene and the smoke PPM are unchanged (a cache changes *how fast* a pipeline
compiles, never *what* it produces), so landing it first isolates the Vulkan-object
plumbing from the file-I/O and `ApplicationInfo` surface that plan 04 adds.

## Impl — `Context::Native` (`engine/include/Veng/Renderer/Backend/ContextNative.h`) + `engine/src/Renderer/Backend/Context.cpp`

The `Context::Native` struct is defined in `ContextNative.h` (not in `Context.cpp`), so
the field is added **there**, beside the other `vk::` handles: `vk::PipelineCache
PipelineCache;`. The create/destroy calls live in `Context.cpp`. Right after the device is
created (the `m_Native->Device = m_Native->CreateDevice();` step), create an **empty**
cache:

```cpp
vk::PipelineCacheCreateInfo cacheInfo{};
m_Native->PipelineCache = m_Native->Device.createPipelineCache(cacheInfo).value;
```

Destroy it in `Context::Dispose()` alongside the other device-level objects
(`m_Native->Device.destroyPipelineCache(m_Native->PipelineCache)`), **before**
`m_Native->Device.destroy()` — after the worker join / retire-bin drain that `Dispose`
already does, so the cache outlives nothing that uses it.

## Public accessor — `engine/include/Veng/Renderer/Native.h`

Add a raw-handle accessor beside the existing `GetVkX` free functions — `Native.h` is the
one public header allowed to expose raw handles. Match the existing pattern exactly:
`inline`, returning the `vk::` type, defined in the header:

```cpp
[[nodiscard]] inline vk::PipelineCache GetVkPipelineCache(const Context& context) { return context.GetNative().PipelineCache; }
```

No other public header changes; `include_hygiene` is unaffected.

## Threading it into the factories

The two sites currently pass `nullptr` for the cache:

- `engine/src/Renderer/Backend/GraphicsPipeline.cpp:183`
  ```cpp
  m_Native->Pipeline = GetVkDevice(m_Context)
      .createGraphicsPipeline(GetVkPipelineCache(m_Context), pipelineInfo).value;
  ```
- `engine/src/Renderer/Backend/ComputePipeline.cpp:37`
  ```cpp
  .createComputePipeline(GetVkPipelineCache(m_Context), pipelineCreateInfo)
  ```

Both factories already reach the context for the device handle, so the cache handle comes
from the same `m_Context` back-reference — no signature or ownership change.

`VkPipelineCache` access must be externally synchronized; both factories run on the single
render thread, so one shared cache is safe with no lock (a present-tense constraint, noted
in `CLAUDE.md` by plan 07).

## Acceptance

- Clean build; `ctest` green; `include_hygiene` green (only `Native.h` gained a raw
  accessor).
- The sample renders the same scene; `HT_SMOKE` writes a correct-sized PPM (1280×720 RGB
  ≈ 2,764,816 bytes) and exits 0 — pixels are unchanged by the cache.
- `ctest --test-dir build-debug -L validation` green; a pipeline cache is an ordinary
  Vulkan object — the allowlist stays empty.
