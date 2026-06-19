# Plan 05 — SSAO

**Goal:** screen-space ambient occlusion. A fullscreen `ScenePass` reads the g-buffer depth
+ normal, computes an occlusion factor in **view space**, and produces an AO target the
lighting pass folds into its ambient/occlusion term — combined with the baked `ORM.r`. A
`SceneRendererSettings` toggle drives the recompile. Independent of Plan 04 by file set.

## What lands

### `SceneRendererSettings` gains the AO toggle

```cpp
bool AO = true;   // topology: the SSAO pass on/off
```

A topology change (`Configure` → recompile). Radius / intensity / bias ride as constants in
the SSAO shader; v1 keeps a fixed kernel.

### The `SsaoScenePass` ([engine/src/Renderer/](../../engine/src/Renderer/))

A reusable `ScenePass` contributing a fullscreen graph pass and producing an AO id:

- **Owns** a **full-res** AO target (`R8Unorm`, `ColorAttachment | Sampled`), recreated on
  `Resize`/`Configure` and **`Release()`d (deferred) before re-registration** on every
  recreate. It is **single-copy** and consumed within the renderer's internal graph (the
  graph derives its write→sample barrier); registered into bindless once (re-registered on
  recreate). Half-res + a blur pass are named follow-ons (below).
- **Declares** a fullscreen pass that samples the **depth** and **normal** g-buffer handles
  (received through `PassIO`) and works in **view space**: it reconstructs view-space
  position from depth (via the projection/inverse-projection in the view-constants buffer)
  and transforms the world-space G1 normal into view space, then estimates occlusion with a
  **16-sample hemisphere kernel**, depth-range-checked, with a tiling noise rotation. Output
  is a single-channel occlusion value.
- **Hands** the produced AO `TextureHandle` to the lighting pass through `PassIO`.

A new core **`ssao.frag`** (fullscreen, samples depth+normal, writes occlusion) is added to
the core pack, paired with the existing `fullscreen.vert`; its `AssetId` is minted in the
final pass. The view/projection matrices it needs ride the **view-constants buffer** Plan 01
established (extended with `View`/`Proj` as needed), not a per-pass push.

### Occlusion fold-in ([deferred_lighting.frag.slang](../../engine/assets/core/shaders/deferred_lighting.frag.slang))

The lighting pass receives the AO handle (when `AO` is on) and multiplies the
**ambient/occlusion term** by `min(ORM.r × OcclusionStrength, ssao)` — SSAO modulates
ambient/indirect, not the direct light, the standard combination with the baked occlusion
channel. (The ambient term is the visible hemispheric ambient Plan 01 sized, so the AO delta
reads.) With `AO` off, the lighting pass uses only the baked `ORM.r` (a compile-time variant,
not a per-frame branch).

## Decisions

1. **SSAO modulates the ambient/occlusion term only.** Applying AO to direct lighting darkens
   contact incorrectly; v1 occludes indirect/ambient and combines with the baked `ORM.r` by
   `min`. The direct Cook-Torrance terms are untouched.

2. **View space, reusing the view-constants buffer.** SSAO reconstructs view-space position
   and operates with a view-space hemisphere kernel — the standard, well-behaved formulation
   for range checks. The matrices come from the view-constants buffer (no per-pass push),
   consistent with the lighting pass.

3. **A produced AO id consumed by the lighting pass.** Like the shadow map (Plan 04), SSAO
   is a `ScenePass`-produced resource the lighting pass reads via `PassIO` — independent of
   the shadow pass by file set, so 04 and 05 fan out.

4. **Full-res, fixed 16-sample kernel, no blur (v1).** One full-res AO target and a fixed
   kernel; a blur pass (to denoise the raw kernel), half-res for bandwidth, and tunable
   radius/intensity are easy follow-ons behind the same pass. The golden is generated from
   the fixed kernel, so its noise is reproducible.

5. **`AO` is topology.** On/off compiles the pass in or out and selects the lighting variant;
   it is not a per-frame branch (the settings-drive-recompile invariant).

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `AO` setting. |
| `engine/src/Renderer/SsaoScenePass.{h,cpp}` (new) | The fullscreen AO pass + owned AO target (deferred `Release()` on recreate) + produced handle. |
| `engine/src/Renderer/SceneRenderer.cpp` | Wire the SSAO pass when `AO`; extend the view-constants buffer with `View`/`Proj` as needed; feed its handle to the lighting pass via `PassIO`; recreate on `Resize`/`Configure`. |
| `engine/assets/core/shaders/ssao.frag.{slang,shader.json}` (new) | Fullscreen view-space SSAO over depth+normal; minted `AssetId`. |
| `engine/assets/core/shaders/deferred_lighting.frag.slang` | AO handle fold-in to the ambient/occlusion term (compile variant on `AO`). |
| `tests/...` | A gpu test asserting a concave/contact region (sphere-on-plane) darkens with `AO` on vs off; `Configure({AO=false})` recompiles. |

## Verification

- Clean build; toggling `AO` recompiles without per-frame reallocation.
- `gpu` band + `smoke_golden`: regenerated to show contact darkening where the sphere meets
  the receiver plane; `AO=false` matches the baked-occlusion-only result.
- Validation gate clean (the AO pass samples depth+normal with graph-derived transitions; the
  AO target's usage carries `Sampled`; the recreate goes through the deferred `Release()`).
