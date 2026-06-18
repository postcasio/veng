# Plan 05 — tonemap as a PostProcess material

**Goal:** make tonemap the **first PostProcess material** — the demanding consumer that
proves the plan-04 path end to end. Replace the hardcoded `TonemapScenePass` +
`tonemap.frag` push-constant pipeline with a cooked PostProcess material in the core pack,
its HDR input a runtime-bound handle field and its exposure an exposed authored-param value.
The geometry/lighting spine and the `DebugView` blits are untouched.

## Why tonemap, and why only tonemap

Tonemap crosses the line plumbing does not (planset decision 4): it has an authorable
**curve / exposure / grade**, exactly the "tunable effect with exposed parameters" a
PostProcess material is for. The swapchain composite and the debug-view blits do not — they
are fixed plumbing — so they stay hardcoded. Tonemap is the right first material: it is
already a fullscreen pass sampling one upstream target (HDR) with one scalar knob
(`exposure`), so it maps onto the material model with no new concepts beyond plan 02.

## What lands

### `tonemap.vmat` + `tonemap.frag` in the core pack

The core `tonemap.frag.slang` is reworked from a push-constant shader into a **bindless
postprocess material** fragment shader:

- it declares its combined `MaterialParams { uint Hdr; uint HdrSampler; float Exposure; }`
  (+ padding to `MaterialParamStride`, the fixed per-slot stride) — the unified block of plan
  00, holding handles and the param together — and reads it via the standard
  `LoadMaterialParams(g_PC.MaterialIndex)`;
- its HDR input is the `Hdr`/`HdrSampler` handle fields, read through
  `g_Textures`/`g_Samplers` like any material texture — replacing the `TonemapPushConstants`
  HDR slots;
- it writes a single `float4 : SV_Target0` (the PostProcess output contract plan 02
  validates).

A new `tonemap.vmat.json` in the core pack:

```jsonc
{
  "domain": "postprocess",
  "shaders": { "vertex": <core fullscreen.vert id>, "fragment": <core tonemap.frag id> },
  "fields": [
    { "name": "Hdr",        "type": "texture" },
    { "name": "HdrSampler", "type": "sampler", "texture": "Hdr" },
    { "name": "Exposure",   "type": "float", "value": 1.0 }
  ]
}
```

The `Hdr` texture field has no authored `id` — its bindless index is **runtime-bound** each
frame to the renderer's current HDR target (plan 04: the pass rewrites the input handle field
at record time). A runtime-bound handle field is a first-class case (plan 00): the cooker
emits it with no resolved id, the loader loads no dependency and leaves its slot zero, and
the renderer writes the live index into the ring-buffered block (plan 01).

The core material asset id is a marked placeholder during implementation, minted with
`vengc generate-id --reference engine/assets/core/core.vengpack.json` in the final pass.

### SceneRenderer wires the material, drops the hardcoded pass

In [SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp):

- `Create` loads the core `tonemap.vmat` (`LoadSync` through the `AssetManager`, like the
  core fullscreen shaders it loads today) and holds the `AssetHandle<Material>`.
- The `Final` chain's tail becomes `… → DeferredLighting (→ HDR) → PostProcessScenePass(tonemap material, input = HDR, output)` — a `PostProcessScenePass` (plan 04) driving the
  tonemap material, replacing `TonemapScenePass`.
- `TonemapScenePass`, `m_TonemapPipeline`, `m_TonemapLayout`, `TonemapPushConstants`, and
  the `TonemapFragId` core-shader constant are **deleted** — the material owns its params and
  the pass builds the pipeline. The `Settings.Exposure` knob is written into the tonemap
  material's `Exposure` param (via `Material::SetParam`) each `Execute`, safely, because the
  param buffer is ring-buffered (plan 01).

The `DebugView` blit passes (`FullscreenBlitScenePass`, albedo/normal/depth) are
**unchanged** — they remain hardcoded engine passes (they are debug plumbing, not content).

## Decisions

1. **Tonemap is a core-pack material, loaded by the renderer at `Create`.** It ships with
   the engine (the core pack), like the fullscreen/lighting/blit shaders the renderer
   already `LoadSync`es. A game gets the engine's tonemap for free; a game that wants a
   different tonemap/grade authors its own postprocess material and configures the renderer
   to use it — the seam the post-stack follow-on builds on, opened but not widened here.

2. **`Exposure` is an exposed authored param, set per frame — buffer-backed, not a push
   constant.** The per-frame `SceneRendererSettings::Exposure` is written into the material's
   `Exposure` param each frame; the param stays in the unified block (plan 00), and the
   per-frame write is safe and stall-free because that block is ring-buffered (plan 01). No
   push-constant side channel — the material model stays uniform. `Material::SetParam` writes
   only the field's reflected `Size` bytes, so a scalar `Exposure` is set with a
   `SetParam("Exposure", f32)` scalar overload (added here) rather than smearing a `vec4`
   over the following bytes. This is the concrete proof that a PostProcess material's exposed
   params are runtime-tweakable through the standard path.

3. **The HDR input is runtime-bound, the rest is cooked.** The tonemap material's `Hdr`
   handle field carries no cooked `id`; the renderer writes the current HDR target's
   bindless index each frame (plan 04). Its `Exposure` param and the fullscreen pipeline are
   cooked/built once. This split — cooked params + a runtime-bound scene-texture input — is
   the postprocess material pattern.

4. **The golden is governed here.** This plan re-homes the tonemap *mechanism*; the tonemap
   *math* is preserved (the same curve, the same exposure multiply). If the regenerated
   `smoke_golden` capture moves at all, it moves only within the fuzzy-compare tolerance
   from pipeline-build/precision differences; a deliberate visible change is regenerated per
   the `CLAUDE.md` golden procedure and called out in the commit.

## Files

| File | Change |
|---|---|
| `engine/assets/core/shaders/tonemap.frag.slang` | Rework to a bindless postprocess material fragment: `MaterialParams { Exposure }`, HDR via a handle field, single color out. |
| `engine/assets/core/shaders/tonemap.frag.shader.json` | Unchanged source/entry (still a fragment shader). |
| `engine/assets/core/materials/tonemap.vmat.json` | New — the core tonemap PostProcess material. |
| `engine/assets/core/core.vengpack.json` | New `tonemap.vmat` material entry (placeholder id → minted); the pack now contains a material, so it re-cooks under the planset's material format (plans 00 + 02). |
| `engine/include/Veng/Asset/Material.h` + `engine/src/Asset/Material.cpp` | `SetParam(string_view, f32)` scalar overload (writes the field's reflected `Size` bytes). |
| `engine/src/Renderer/SceneRenderer.cpp` | Load the tonemap material; replace `TonemapScenePass` with a `PostProcessScenePass`; delete the tonemap pipeline/layout/push struct; route `Settings.Exposure` into the material param each `Execute`. |

## Verification

- Clean build; the core pack cooks with its first material (domain `postprocess`,
  validated against the single-color output contract by plan 02's check).
- Smoke PPM correct size + exit 0; `smoke_golden` re-checked — tonemap output matches the
  prior pipeline within tolerance (decision 4), golden regenerated only if a deliberate
  change lands.
- `gpu` band green; `validation_gate` clean (the postprocess pass's derived barrier over
  the HDR target is correct).
- The `Final` debug view still tonemaps; the `Albedo`/`Normal`/`Depth` debug views are
  unchanged (their blit passes were not touched).
