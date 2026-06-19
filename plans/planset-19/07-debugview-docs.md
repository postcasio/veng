# Plan 07 — DebugView channels + docs/roadmap re-cut

**Goal:** expose the new g-buffer/battery channels through `DebugView`, then bring the docs
and roadmap to present-tense fact. No render-path code beyond the debug arms.

## What lands

### `DebugView` gains the PBR/battery arms ([SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h))

```cpp
enum class DebugView : u8 {
    Final,
    Albedo, Normal, Depth,           // existing
    Roughness, Metallic, Occlusion,  // ORM channels
    AO,                              // the SSAO target
    Shadows,                         // the directional shadow visibility
};
```

Each new arm re-wires the pass set through `Configure` → recompile (the existing
settings-drive-recompile proof, extended): the debug blit samples the relevant
channel/target (an `ORM` channel, the AO target, or the shadow visibility) instead of the lit
`Final`. The blits follow the existing albedo/normal/depth/HDR blit pattern — hardcoded
engine passes, no authorable surface. `AO`/`Shadows` arms degrade gracefully when their
battery is toggled off (blit a constant where the target is absent, or gate the arm on the
setting).

### Docs and roadmap (no code)

- **[GBuffer.h](../../engine/include/Veng/Renderer/GBuffer.h)** — already present-tense after
  Plan 01; confirm the three-target + emissive-in-`ORM.a` contract is stated as fact, `G0.a`
  is noted **unused/reserved** (only `G2.a` carries emissive, leaving one free channel before
  the colored-emissive follow-on needs a 4th target), and the "the only depth target read as
  a texture" line becomes **depth *targets*** (the g-buffer depth + the directional shadow
  map).
- **[CLAUDE.md](../../CLAUDE.md)** — update the SceneRenderer / deferred-material-contract
  paragraphs: PBR metallic-roughness g-buffer (three targets) + tangent-space normal mapping,
  Cook-Torrance over typed lights, the **per-frame view-constants buffer** (per-view data is
  a ring-buffered set-0 buffer, not push constants), the shadow/SSAO/bloom batteries,
  bloom-as-PostProcess-material (the multi-stage post path).
- **[scene-renderer.md](../future/scene-renderer.md)** — move the delivered batteries (PBR
  g-buffer, normal mapping, Cook-Torrance, typed lights, directional shadows, SSAO, bloom)
  from "still future" to delivered; keep the genuinely-future remainder (transparent/forward
  pass, shadowed punctual lights, colored emissive, CSM, clustered light culling) and add
  **scene/mesh AABB + bounds** as the named next prerequisite — directional shadows ship with
  a fixed-size ortho box because no bounds facility exists; a tight shadow fit and CSM need
  it first.
- **[future/README.md](../future/README.md)** — area-8 status: batteries + G2 PBR target
  delivered; **downgrade on-tile/subpass-fused deferred** from a "named next increment behind
  the same mechanism" to a **measure-first maybe** — a `RenderGraph`-core change (local-read /
  pass fusion + an input-attachment g-buffer binding path), gated on a MoltenVK
  `dynamic_rendering_local_read` capability check and on g-buffer round-trip being a measured
  bottleneck, **not** a `ScenePass`-level change.
- **[plans/README.md](../README.md)** — add the planset-19 entry.
- **This README's status table** — mark all plans `done`.

## Decisions

1. **Debug arms are blits, not effects.** Visualizing a channel is plumbing with no
   authorable surface, so each `DebugView` arm is a hardcoded blit like the existing
   albedo/normal/depth arms — not a PostProcess material.

2. **Each arm is a topology change.** Selecting a debug view re-wires the pass set
   (`Configure` → recompile), consistent with the existing `DebugView` handling; it is not a
   per-frame branch.

3. **The on-tile downgrade is recorded here, in prose.** This planset deliberately does
   **not** build on-tile deferred; the roadmap edit states why (architecture + driver +
   measurement gates) so a future reader does not mistake it for a dropped commitment.

4. **Scene/mesh AABB + bounds is named as the next prerequisite.** The fixed-size ortho box
   in Plan 04 is a placeholder; the roadmap records that a real bounds facility is the gate
   on a tight shadow fit and CSM.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | New `DebugView` arms. |
| `engine/src/Renderer/SceneRenderer.cpp` | Wire each new arm to its channel/target blit. |
| `engine/include/Veng/Renderer/GBuffer.h` | `G0.a` unused note; "depth targets" plural. |
| `CLAUDE.md` | SceneRenderer / material-contract paragraphs → present-tense PBR + view-constants buffer + batteries. |
| `plans/future/scene-renderer.md` | Delivered vs. remaining re-cut; AABB/bounds named next. |
| `plans/future/README.md` | Area-8 status; on-tile downgrade to measure-first maybe. |
| `plans/README.md` | planset-19 entry. |
| `plans/planset-19/README.md` | Status table → `done`. |

## Verification

- Clean build; each `DebugView` arm recompiles to its blit and renders the expected channel
  (manual/`gpu` spot-check; the smoke path renders `Final`).
- `smoke_golden` unchanged by this plan (the default `Final` view is untouched; only new
  debug arms are added).
- Docs read as present-tense fact with no plan-citation or future-tense hedging in code
  comments (the `CLAUDE.md` comment policy).
