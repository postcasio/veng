# Plan 04 — additive forward emissive (color decoupled from albedo, no fifth g-buffer target)

**Goal:** give the deferred renderer **color-decoupled** emissive — a surface that emits a color
unrelated to its base color, including a dark-albedo surface that glows — without adding a fifth
g-buffer target. The g-buffer is already four MRT targets (G0 albedo / G1 world-normal / G2 ORM /
G3 velocity); an RGB emissive channel would be a fifth. The current emissive is a **scalar** packed
in the ORM target (planset-19), added post-lighting as roughly `albedo · emissiveScalar`: it cannot
emit a color the albedo lacks and a black albedo cannot glow at all. Since emissive is an **additive,
lighting-independent** term, it does not belong in the g-buffer; it is written in a small **additive
forward pass** into the lit HDR target.

## Why a forward pass, not a g-buffer target

Emissive is not a lighting *input* — it is an output addend. Carrying RGB emissive through a fifth
g-buffer target pays MRT (multiple-render-target) bandwidth on **every** geometry pixel to store a
value that is zero for the ~all of them that do not emit. (The engine `CLAUDE.md` notes colored
emissive is "one free channel away from needing a fifth target" via the unused G0.a — that packed-
channel framing is superseded here: a single alpha channel cannot carry an RGB color, and the
forward pass avoids the target entirely.) An additive forward pass re-rasterizes **only** emissive
geometry into the already-lit HDR target after the deferred lighting pass, so non-emitters cost
nothing — each *emitter* pays one extra rasterization (vertex shader + read-only depth test) — and the
emissive color is fully independent of albedo. Driving albedo above 1.0 (the forward-renderer trick)
does not work here: a deferred albedo is consumed as `albedo · BRDF · light` (the BRDF, the surface's
bidirectional reflectance), so a shadowed emitter computes to black and an albedo > 1 breaks energy
conservation for any reflection/IBL.

## The starting point

- The g-buffer surface pass writes albedo / world-normal / ORM(+scalar emissive) / velocity; the
  deferred lighting pass reads them and writes the lit **HDR** target; bloom then tonemap follow.
- A surface material's params carry a scalar emissive; there is no RGB emissive channel.
- The renderer gathers visible meshes once (`GatherMeshes` / `SceneBroadphase`) and the geometry +
  shadow passes cull that span; an emissive pass reuses the same gathered span.

## What lands

### 1. An RGB emissive material term

- A surface material gains an **emissive color** (RGB) parameter — and, where authored, an emissive
  texture handle — in its `MaterialParams` block. The scalar ORM emissive (`ORM.a`, added in the
  lighting shader as `albedo · emissive`) is **retired** in favor of the RGB term — one emissive path,
  not two. The observable contract: a surface that previously emitted via the scalar is re-authored
  onto the RGB term (set `emissiveColor = albedo · oldScalar` to reproduce the old look exactly, or a
  new color to decouple). The sample's existing scalar emitter is re-authored this way; no asset keeps
  a live scalar path.

### 2. An `EmissiveScenePass`

- A new `ScenePass` runs **after** deferred lighting and **before** bloom, with **additive** blending
  into the HDR target, the depth buffer bound read-only for correct occlusion. It draws the gathered
  visible meshes whose material has a non-zero emissive term, through the standard surface vertex
  shader + a minimal fragment shader that outputs only `emissiveColor · emissiveTex`.
- The pass is gated by a `SceneRendererSettings` toggle (default on) driving the `Configure`
  recompile, and culls its draws by the camera frustum off the shared broadphase like the geometry
  pass.

### 3. A debug view

- A `DebugView::Emissive` arm blits the emissive contribution (the additive pass into a cleared
  target) so the channel is inspectable like every other.

## Files (sketch — the agent confirms against the tree)

- `engine/include/Veng/Renderer/SceneRenderer.h` — the settings toggle, the `DebugView::Emissive` arm.
- `engine/src/Renderer/Passes/EmissiveScenePass.*` (new) — the additive forward pass.
- `engine/src/Renderer/SceneRenderer.cpp` — wire the pass into the compiled graph between lighting and
  bloom.
- `engine/assets/core/shaders/` — the emissive fragment shader; remove the scalar ORM emissive from
  the surface + lighting shaders (the `ORM.a` write and the `albedo · emissive` add).
- `engine/CLAUDE.md` — update the g-buffer note that frames colored emissive as "one free channel away
  from needing a fifth target": colored emissive lands as a forward additive pass, not a packed
  channel.
- The surface material param block + its C++ mirror — the RGB emissive field.

## Examples to co-migrate

`hello-triangle` gains one **emissive material** on a sample object (a small glowing element) so the
pass is exercised end to end and the bloom picks it up — the visible proof color-decoupled emissive
works (a dark-albedo emitter that glows). `template` stays minimal (no emitter); the pass runs with
zero emissive draws and costs nothing.

## Verification

- The emissive object glows independent of its albedo and lights (visible in shadow); bloom blooms it.
- `smoke_golden` **moves** (the sample gains an emitter, and the scalar path is retired) — see the
  planset README's golden-regeneration note: the golden is regenerated **once against the integrated
  tree**, not per-plan, since Plans 04/06/07 all move it.
- Validation gate clean (`build-debug -L validation`) — the additive blend + read-only depth binding
  is exactly where a barrier/attachment validation error would surface.
- With the `Emissive` toggle off, the image equals a render with the emissive term zeroed — the guard
  that the pass is **purely additive**. (Not byte-identical to the *pre-plan* render: retiring the
  scalar path removes the old `albedo · scalar` glow, which is the point.)

## Risks

- **Read-only depth during an additive pass** — the depth attachment must be bound read-only (no
  writes) so the pass occludes correctly without disturbing later reads; a read-write binding is a
  validation/hazard error.
- **Double-counting emissive** if the retired scalar ORM term is left live in the lighting shader
  alongside the new RGB pass — the scalar `albedo · emissive` add must be fully removed, not just
  unused.
- **HDR precision** — additive emissive can drive values high; it shares the HDR target's range with
  lighting, which is already HDR, so no new format, but extreme emissive + bloom can clip — a tuning
  concern, not a correctness one.
