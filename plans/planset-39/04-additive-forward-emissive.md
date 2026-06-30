# Plan 04 — additive forward emissive (color decoupled from albedo, no fourth g-buffer target)

**Goal:** give the deferred renderer **color-decoupled** emissive — a surface that emits a color
unrelated to its base color, including a dark-albedo surface that glows — without adding a fourth
g-buffer target. The current emissive is a **scalar** packed in the ORM target (planset-19), added
post-lighting as roughly `albedo · emissiveScalar`: it cannot emit a color the albedo lacks and a
black albedo cannot glow at all. Since emissive is an **additive, lighting-independent** term, it does
not belong in the g-buffer; it is written in a small **additive forward pass** into the lit HDR target.

## Why a forward pass, not a g-buffer target

Emissive is not a lighting *input* — it is an output addend. Carrying RGB emissive through a fourth
g-buffer target pays MRT bandwidth on **every** geometry pixel to store a value that is zero for the
~all of them that do not emit. An additive forward pass re-rasterizes **only** emissive geometry into
the already-lit HDR target after the deferred lighting pass, so non-emitters cost nothing and the
emissive color is fully independent of albedo. Driving albedo above 1.0 (the forward-renderer trick)
does not work here: a deferred albedo is consumed as `albedo · BRDF · light`, so a shadowed emitter
computes to black and an albedo > 1 breaks energy conservation for any reflection/IBL.

## The starting point

- The g-buffer surface pass writes albedo / world-normal / ORM(+scalar emissive) / velocity; the
  deferred lighting pass reads them and writes the lit **HDR** target; bloom then tonemap follow.
- A surface material's params carry a scalar emissive; there is no RGB emissive channel.
- The renderer gathers visible meshes once (`GatherMeshes` / `SceneBroadphase`) and the geometry +
  shadow passes cull that span; an emissive pass reuses the same gathered span.

## What lands

### 1. An RGB emissive material term

- A surface material gains an **emissive color** (RGB) parameter — and, where authored, an emissive
  texture handle — in its `MaterialParams` block. The scalar ORM emissive is **retired** in favor of
  the RGB term (or kept only as a multiplier into it; the agent picks the simpler of the two against
  the current shader and documents which).

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
- `engine/assets/core/shaders/` — the emissive fragment shader; remove/retain the scalar ORM emissive
  in the surface + lighting shaders per the decision above.
- The surface material param block + its C++ mirror — the RGB emissive field.

## Examples to co-migrate

`hello-triangle` gains one **emissive material** on a sample object (a small glowing element) so the
pass is exercised end to end and the bloom picks it up — the visible proof color-decoupled emissive
works (a dark-albedo emitter that glows). `template` stays minimal (no emitter); the pass runs with
zero emissive draws and costs nothing.

## Verification

- The emissive object glows independent of its albedo and lights (visible in shadow); bloom blooms it.
- `smoke_golden` **moves** (the sample gains an emitter) — regenerate it **once** per the documented
  procedure, then it holds.
- Validation gate clean (`build-debug -L validation`) — the additive blend + read-only depth binding
  is exactly where a barrier/attachment validation error would surface.
- With the settings toggle off, the image is byte-identical to the pre-plan render (a guard that the
  pass is purely additive).

## Risks

- **Read-only depth during an additive pass** — the depth attachment must be bound read-only (no
  writes) so the pass occludes correctly without disturbing later reads; a read-write binding is a
  validation/hazard error.
- **Double-counting emissive** if the scalar ORM emissive is left live alongside the RGB term — pick
  one path; do not add both.
- **HDR precision** — additive emissive can drive values high; it shares the HDR target's range with
  lighting, which is already HDR, so no new format, but extreme emissive + bloom can clip — a tuning
  concern, not a correctness one.
