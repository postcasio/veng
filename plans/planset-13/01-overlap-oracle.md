# Plan 01 — the reuse barrier + comment fix

**Goal:** correct the false `SceneRenderer.h` contract comment and close the cross-graph
write-after-read with a single `PrepareForAccess` call. No test harness — the existing
smoke and validation gate cover regressions.

## Correct the false comment

`engine/include/Veng/Renderer/SceneRenderer.h` carries a `Single-in-flight contract` block
(above the renderer-owned image members) that ends: "… so the frames-in-flight > 1 hazard
(a compositor caching/sampling an output across frames-in-flight) **does not arise**." That
is false at FIF=2 with a cross-graph consumer.

Rewrite it to the true present-tense contract: the renderer-owned output is single-copy;
its cross-frame reuse is serialized by `PrepareForAccess(ColorAttachment)` before each
`Execute` (the reverse of the consumer's `Sample` transition), which suffices without a
semaphore or ring because the consumer read and the next producer write both record on the
single graphics queue in submission order, so the barrier's first synchronization scope
reaches the prior frame's read. The internal g-buffer/depth/HDR targets need no such
barrier because each lives in the renderer's own graph, which serializes its own reuse.
No historical narrative about the never-built assert, no plan citations (CLAUDE.md).

## The barrier

The consumer leaves the output in `ShaderReadOnly` after every frame's read (the composite
declares `.Sample(output)` in its own graph; the windowed ImGui path calls
`PrepareForAccess(GetOutput(), AccessKind::Sample)` directly). The missing partner is the
reverse transition before the output is written again. Add it at the top of
`SceneRenderer::Execute`, before replaying the internal graph:

```cpp
// The output is single-copy; the consumer's Sample transition left it in ShaderReadOnly.
// This barrier's source scope reaches the prior frame's read because both record on the
// single graphics queue in submission order — no semaphore or ring needed.
cmd.PrepareForAccess(m_OutputView, Renderer::AccessKind::ColorAttachment);
m_Internal->Graph->Execute(cmd, bindings, &resolvedView);
```

`PrepareForAccess` funnels through the same `ScopeFor + DecideBarrier` path as the graph
and updates the image's tracked state, so it emits a `ShaderReadOnly → ColorAttachment`
transition on first `Execute` after `Create`/`Resize`/`Configure` (`Undefined →
ColorAttachment`) and every steady-state frame. Correct unconditionally — no precondition on
whether a prior consumer read has occurred.

**Placement: renderer-owned.** The consumer owns the read-side `Sample` transition; the
renderer owns the write-side `ColorAttachment` transition. Emitting it in `Execute` means no
consumer can forget it. A consumer-side barrier would need to be repeated per renderer per
frame; one omission is a silent cross-frame hazard.

## Tests

- **`smoke_golden`:** unchanged — no pixel change; the fix is a barrier, not pixels.
- **`include_hygiene`:** unaffected — `PrepareForAccess`/`AccessKind` are existing public
  vk-free API.
- **Validation gate (`build-debug`, `-L validation`):** run after landing to confirm the
  gate is green (no `SYNC-HAZARD` on the output or internal targets in any existing GPU
  test). No new test is added.

## Acceptance

Clean build; the false single-in-flight comment in `SceneRenderer.h` is rewritten to the
true cross-graph reuse contract; `PrepareForAccess(m_OutputView, ColorAttachment)` is
emitted at the top of `SceneRenderer::Execute`; default `ctest` green; `smoke_golden`
unchanged; smoke PPM correct size + exit 0; validation gate green under `build-debug`.
Commit: `Plan 01: serialize SceneRenderer output cross-frame reuse with a PrepareForAccess
barrier, correct the false single-in-flight comment`.
