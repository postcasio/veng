# Plan 04 — De-globalize context internals & delete the singleton

**Goal:** convert the last `Context::Instance()` users — the context-internal
primitives and the `DebugMarkers` helpers — to take the context explicitly, then
**delete `s_Instance` / `Instance()`**. Capstone of the de-global arc: after this
plan the global is gone and the device is threaded everywhere.

## What still reaches the global after plan 03

All *context-internal* — constructed by `Context` itself during `Initialize`, or
free helpers called from resource constructors:

- **Primitives owned by `Context`:** `CommandPool`, `DescriptorPool`, `SwapChain`,
  `Fence`, `Semaphore`, `CommandBuffer`, `SynchronizationFrame`. These run inside
  the context's own setup, where `*this` is in scope.
- **`DebugMarkers::Mark*`** free functions (`Backend/DebugMarkers.cpp`) — they call
  `Context::Instance()` to fetch the device/instance, and are invoked from many
  resource constructors (`MarkBuffer`, `MarkImage`, …).
- **The singleton machinery itself:** `s_Instance = this` in `Initialize`,
  `s_Instance = nullptr` in teardown, `static Context& Instance()`, and the
  `static inline Context* s_Instance` member in `Context.h`.

## Work

1. **Thread `*this` into the internal primitives.** Give `CommandPool`,
   `DescriptorPool`, `SwapChain`, `Fence`, `Semaphore`, `CommandBuffer`, and the
   per-frame `SynchronizationFrame` setup an explicit `Context&` (constructor
   param or the same `m_Context` back-ref pattern), passed as `*this` from the
   `Context` code that builds them. These are not public `Create` factories the
   sample calls, so no sample migration — purely internal.

2. **De-global `DebugMarkers`.** Give the `Mark*` helpers an explicit device (or
   `Context&`) parameter instead of `Context::Instance()`. Their callers are
   resource constructors that, after plans 01–03, already hold the context
   (`m_Context` / the `context` parameter) — pass it through. (Prefer passing the
   `vk::Device` the marker actually needs over the whole `Context&`, to keep the
   helper's dependency minimal — your call at implementation time.)

3. **Delete the singleton.** Remove `s_Instance`, both assignments, and the
   `static Context& Instance()` declaration + definition. `grep -rn "Instance()"`
   over `src/` + `include/` should return nothing in the renderer context sense.

4. **Update the docs.**
   - `include/Veng/Veng.h` — the threading note still says *"The render Context is
     a singleton (`Context::Instance()`)"*. Rewrite: drop the singleton wording,
     keep the single-threaded contract (one driving thread; `Time`/input/ImGui
     assume it). The contract is unchanged — only the *reason* (no longer "because
     it's a global").
   - `CLAUDE.md` — the top-of-file paragraph (*"The render `Context` is a singleton
     (`Context::Instance()`)"*) and any `Context::Instance().GetNative().Retire(...)`
     reference in the ownership section: restate as an explicit-context back-ref.
     Note de-global is **done** and threading remains future work.
   - `docs/ownership.md` — if it leans on the singleton for the retire/frame-bin
     explanation, restate in terms of the resource's `m_Context` back-ref.

## Dependencies

Needs plans 02 **and** 03 — every app-facing `Create` must already be explicit, or
deleting `Instance()` breaks the build. This is the merge gate for the whole
de-global half.

## Acceptance

- Clean build with `Instance()` **removed** — its absence is the proof the sweep is
  complete (any straggler is now a compile error, not silent global access).
- `ctest` green, smoke binary writes a correct-sized PPM.
- **Validation-verified** under `VE_DEBUG`: `headless_smoke` + `compute_dispatch`
  run directly from `build-debug/`, no new validation ERRORs vs. the planset-3
  baseline. The internal primitives (sync, swapchain, command buffers) are exactly
  the lifetime-sensitive paths, so this check is mandatory.
- `include_hygiene` still green — no public header started leaking a backend type
  (the `DebugMarkers` device parameter must not surface in a public header).
- Docs (`Veng.h`, `CLAUDE.md`, `docs/ownership.md`) no longer claim a singleton.

## Notes

- **Behaviour-preserving**, like 01–03: the device handed to each primitive is the
  same one `Instance()` returned. Multi-context becomes *possible* but `Application`
  still owns exactly one `Context`; do not add a second here.
- The single-threaded v1 contract **stands** — this plan removes the global, it does
  not lift the threading restriction. That lift is the future threading planset.
- This is the natural point to confirm `Application` and `ImGuiLayer` (which already
  take a `Context&`, e.g. `ImGuiLayer::Create(info, m_RenderContext, window)`) never
  depended on `Instance()` — they didn't, so they need no change beyond what 02/03
  threaded.
