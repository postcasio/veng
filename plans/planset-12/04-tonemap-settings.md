# Plan 04 — the tonemap pass, `Settings`/`DebugView` recompile, the design-for-N test

**Goal:** close the v1 deferred spine. Add a **tonemap** pass (HDR → the output
format) swapping plan 03's `HdrBlitScenePass`, giving the chain its final stage; grow
`SceneRendererSettings` with a **`DebugView`** selector that **re-wires the pass set
through `Configure`** — the live proof that *settings drive recompile* (decision 2);
pin the **single-in-flight** contract with a `Create`-time assert and record the
frames-in-flight > 1 decision (decision 6); and add the **two-renderer interleaved**
unit/gpu test that proves the design-for-N surface (decision 7).

## Why this is its own plan, and on the main thread

This plan fixes the **recompile seam** — the second half of the lifetime split. Plan
01 established that `Configure` *can* recompile; this plan makes it actually change
**topology** (which passes are wired) for a real setting, which is the architecture's
central claim and the thing the editor will lean on hardest. The single-in-flight
contract and the N-renderer test are the honest bookkeeping that closes area 8's v1.

## The tonemap pass — `TonemapScenePass`

- **Input:** the HDR image (plan 03's lighting-pass output), sampled via its bindless
  `TextureHandle`.
- **Output:** the renderer's **imported output** image (`OutputFormat`).
- **Math:** a fullscreen tonemap (e.g. Reinhard or ACES-approx) + an `Exposure`
  setting; the choice is documented next to the pass. This **swaps plan 03's
  `HdrBlitScenePass`** in the same slot — a clean single-pass replacement (the lighting
  pass's HDR output target is unchanged) — so the chain is now
  **g-buffer → deferred lighting → tonemap → output**.

## `SceneRendererSettings` + the `DebugView` recompile proof

```cpp
enum class DebugView : u8 { Final, Albedo, Normal, Depth };

struct SceneRendererSettings
{
    DebugView Mode     = DebugView::Final;   // field named for role, not type
    f32       Exposure = 1.0f;               // tonemap; per-frame-safe but kept a setting for simplicity
    // future battery toggles (Shadows/AO/Bloom) land here behind the same recompile
};
```

- **`Mode` re-wires the pass set** (a genuine topology change → `Configure` →
  recompile):
  - `Final` → `[GBuffer, DeferredLighting, Tonemap]` (the full chain).
  - `Albedo` → `[GBuffer, blit G0 → output]` (lighting + tonemap dropped).
  - `Normal` → `[GBuffer, blit (decoded) G1 → output]`.
  - `Depth` → `[GBuffer, depth-visualize → output]`.
- This is the **settings-drive-recompile** demonstrator: the *pass set itself* changes
  with the setting — exactly the `CompiledGraph` structural-recompile model — and the
  debug views are genuinely useful for the renderer (not a throwaway effect). The debug
  blits reuse the fullscreen-pass shape; the albedo blit is the same `AlbedoBlitScenePass`
  plan 02 introduced.
- **`Exposure`** is a tonemap-pass uniform; it is kept a setting (recompile-safe — it
  never changes topology) so the surface is exercised, with a note as present-tense
  fact that a purely per-frame knob could instead ride `SceneView` (the invariant-2
  distinction).

## The single-in-flight contract + the FIF>1 guard (decision 6)

- **Stated as a fact** next to the renderer-owned image allocation: a `SceneRenderer`
  is single-in-flight — one `Execute` resolves and completes before the next begins;
  its owned images (g-buffer, depth, HDR, output) are single-copy; the retire path
  covers destruction safety on `Resize`/`Configure`, not cross-frame reuse hazards.
- **Recorded *and guarded*, not built:** when frames-in-flight > 1, the **output**
  image (sampled by the compositor for frame N while the renderer writes N+1) and any
  image read across an `Execute` boundary must be ring-buffered per frame-in-flight — a
  compile-time allocation decision. `Create` asserts the invariant:
  ```cpp
  VE_ASSERT(context.GetMaxFramesInFlight() == 1,
            "SceneRenderer is single-in-flight; ring-buffer GetOutput() (and any "
            "cross-Execute image) before raising frames-in-flight");
  ```
  This is a present-tense fact (not "future work" phrasing — CLAUDE.md), and it fires
  the instant someone bumps FIF, at the exact place the ring buffer must be added.
  Nothing drives FIF > 1 while the render thread is single, so the ring buffer is not
  built.

## The design-for-N test (decision 7)

A **two-renderer interleaved** test: construct two `SceneRenderer`s (different extents
and `Mode`s — one `Final`, one `Albedo`) over **one** `const Scene&` with two cameras;
`Execute` them **interleaved in one command stream** (A, B, A) to exercise each
renderer's independent barrier domain (decision 5), not just two isolated draws; then
assert their `GetOutput()`s are **independent** — each sized to **its own extent** (the
crisp independence proof) and producing **distinguishable content** (differing at a
known texel, consistent with the differing `Mode`). This proves the multi-renderer
surface the editor will use, with the sample still wiring one. Labelled `gpu`; skips
with no ICD.

## Sample migration

- hello-triangle's `SceneRenderer` now produces a **tonemapped** final image; the
  composite samples `GetOutput()` unchanged.
- Optionally expose `Mode` as an ImGui combo in the existing "Scene" panel (calling
  `Configure`) to make the recompile seam visible in the running sample — small, and it
  exercises `Configure` at runtime. (Kept minimal; not the N-renderer second viewport,
  which stays out of scope.)

## Tests

- **GPU (`veng_gpu`):** the two-renderer interleaved independence test; a `Configure`
  test that switches `Mode` and asserts the **output pixels change accordingly** (the
  recompile is proven by the observable output difference between modes — there is no
  pass-count introspection on `CompiledGraph`, so the proof is the rendered result, not
  internal topology); an `Exposure` change alters the tonemapped result.
- **`smoke_golden`:** **regenerated** — final tonemapped look (decision 10). Regenerate
  per CLAUDE.md. This is the v1 final golden; the GPU assertions are the real gate.
- **Validation gate:** green, allowlist empty (the tonemap + debug blits add fullscreen
  passes only).
- **`include_hygiene`:** `DebugView`/settings additions stay backend-free.

## Acceptance

Clean build; `ctest` green incl. the two-renderer interleaved test and the
`Configure`/`Mode` recompile test (proven by output-pixel diff); the chain renders
g-buffer → lighting → tonemap → output; `Mode` re-wires the pass set through
`Configure`; the `Create`-time single-in-flight assert is in place; `smoke_golden`
green against the **regenerated final** golden; smoke PPM correct size + exit 0;
validation gate green, allowlist empty. Commit: `Plan 04: tonemap pass,
Settings/DebugView recompile, single-in-flight guard, design-for-N test`.
