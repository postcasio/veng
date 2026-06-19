# Plan 04 — directional shadow map

**Goal:** shadow the directional light with a classic shadow map. A new depth-only
`ShadowScenePass` renders the scene from the light's view into a shadow target; the lighting
pass samples it with **manual PCF** to attenuate the directional light's contribution. A
`SceneRendererSettings` toggle drives the recompile.

## What lands

### `SceneRendererSettings` gains the shadow knobs

```cpp
struct SceneRendererSettings
{
    DebugView Mode             = DebugView::Final;
    f32       Exposure         = 1.0f;
    bool      Shadows          = true;   // topology: the shadow pass on/off
    u32       ShadowResolution = 2048;   // sizing: the shadow map extent
};
```

`Shadows` is a topology change (the pass is present or absent → `Configure` → recompile);
`ShadowResolution` is sizing (recreates the shadow target → recompile). Both are
compile-time, per the settings-drive-recompile invariant.

### The `ShadowScenePass` ([engine/src/Renderer/](../../engine/src/Renderer/))

A reusable `ScenePass` that contributes one `RenderGraph` pass and produces a shadow-map id:

- **Owns** a `ShadowResolution²` depth image (`D32Sfloat`, `DepthAttachment | Sampled`),
  allocated at `Create`/`Resize`/`Configure` and **`Release()`d (deferred) before
  re-registration** on every recreate — the renderer-owned-imported-image pattern the
  g-buffer uses, registered into bindless once (re-registered on recreate). The target is
  **single-copy** and consumed within the renderer's internal graph; the graph derives its
  write→sample barriers (no cross-graph handoff). Because `ShadowResolution` recreates both
  the image and its bindless slot, the old slot must go through the deferred `Release()` path
  so an in-flight frame's sample is not reclaimed early.
- **Declares** a depth-only graph pass: renders the scene's opaque meshes from the
  **light-space matrix** using a minimal depth-only vertex shader (no fragment outputs). The
  same per-submesh draw loop as the geometry pass, but a depth-only pipeline and the
  light-space MVP.
- **Hands** the produced shadow-map `TextureHandle` to the lighting pass through `PassIO`;
  the **light-space matrix is written into the view-constants buffer** (the home Plan 01
  established for per-view matrices), not a push constant.

A new core **`shadow_depth.vert`** (canonical layout in, light-space-MVP transform, no
fragment stage) is added to the core pack; its `AssetId` is minted in the final pass.

### Shadow sampling in the lighting pass ([deferred_lighting.frag.slang](../../engine/assets/core/shaders/deferred_lighting.frag.slang))

The pass reads the shadow-map handle (push) + the light-space matrix (view-constants buffer).
For the **directional** light, it transforms the reconstructed world position into light
space and does **manual PCF**: a small kernel of ordinary `Sample`s of the raw depth,
compared in-shader against the fragment's light-space depth (with a slope-scaled depth bias
to curb acne), averaged into a visibility factor. The shadow-factor hook Plan 03 left in the
per-light loop is filled for `LightType::Directional`; point/spot pass visibility `1.0`
(unshadowed).

## Decisions

1. **Directional only.** A single orthographic shadow map for the directional light.
   Point/spot shadows (cubemaps / a spot atlas) and cascaded shadow maps (CSM) for large
   directional ranges are named follow-ons — this lands the shadow-map machinery (depth-only
   pass, PCF sample, bias) that they extend.

2. **Manual PCF with a regular sampler, not hardware `SampleCmp`.** A hardware comparison
   sampler (`samplerShadow`/`SampleCmp`) is a distinct descriptor type; placing one in set
   0's shared bindless sampler array is exactly the class of construct documented to
   mistranslate inside the Metal argument buffer on MoltenVK (cf. `STORAGE_BUFFER_DYNAMIC`).
   v1 therefore samples raw depth through an ordinary bindless sampler and compares in-shader
   — portable by construction. Hardware `SampleCmp` is a measured later optimization. This
   plan's verification **must run the validation gate under `VE_DEBUG`**.

3. **Fixed-size ortho box, no scene-bounds fit.** The light-space matrix is an orthographic
   projection over a **fixed-size box** around the scene origin (large enough for the sample
   scene), not a tight fit to scene/camera-frustum bounds — the engine has no AABB/bounds
   facility yet. A tight fit (and CSM) is the quality follow-on; **implementing scene/mesh
   AABB + bounds is the prerequisite, recorded in the roadmap re-cut (Plan 07)** as the next
   thing this needs.

4. **A produced `ResourceId` + bindless handle through `PassIO`.** The shadow map is the
   first `ScenePass`-**produced** resource another pass consumes — the case
   [scene-renderer.md](../future/scene-renderer.md) names ("a future shadow pass = sub-passes
   + a produced shadow-map id"). It rides the same named-slot `PassIO` wiring the renderer
   owns.

5. **`Shadows` defaults on; a scene still renders with it off.** Toggling it off removes the
   pass and the lighting pass reads full visibility — the data-driven-emptiness vs topology
   line: the pass is compiled out, not recording zero draws.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `Shadows` + `ShadowResolution` settings. |
| `engine/src/Renderer/ShadowScenePass.{h,cpp}` (new) | The depth-only pass + owned shadow target (deferred `Release()` on recreate) + light-space matrix + produced handle. |
| `engine/src/Renderer/SceneRenderer.cpp` | Wire the shadow pass into the graph when `Shadows`; write its light-space matrix into the view-constants buffer + feed its handle to the lighting pass via `PassIO`; recreate the target on `Configure`/`Resize`. |
| `engine/assets/core/shaders/shadow_depth.vert.{slang,shader.json}` (new) | Depth-only canonical-layout vertex stage, light-space MVP; minted `AssetId`. |
| `engine/assets/core/shaders/deferred_lighting.frag.slang` | Shadow-map handle + light-space matrix; manual-PCF visibility for the directional term. |
| `tests/...` | A gpu test asserting a shadow caster (the sphere) darkens the receiver plane; `Configure({Shadows=false})` recompiles and asserts **no validation error** from a stale shadow-slot sample. |

## Verification

- Clean build; toggling `Shadows` recompiles without reallocating per frame.
- `gpu` band + `smoke_golden`: regenerated to show the sphere casting a shadow onto the
  receiver plane; with `Shadows=false` the capture matches the no-shadow lighting.
- **Validation gate under `VE_DEBUG`** clean: the depth-only pass's `RenderingInfo` (depth
  attachment, no color), the downstream depth-as-texture sample, and the manual-PCF path
  carry the right usage/layout transitions (graph-derived) with no MoltenVK validation error.
