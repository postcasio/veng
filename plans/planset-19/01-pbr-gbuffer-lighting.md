# Plan 01 — PBR g-buffer + Cook-Torrance lighting

**Goal:** turn the deferred spine physically-based. Extend the g-buffer with one packed
**ORM** target, rework the opaque material to **metallic-roughness**, introduce a
ring-buffered **per-frame view-constants buffer**, and replace the Lambert lighting pass
with a **Cook-Torrance** BRDF that reconstructs world position from depth. This is the
foundation every later battery reads; it moves the rendered image, so the golden is
regenerated. Tangent-space normal mapping is **Plan 02**, kept off the spine.

## What lands

### The extended g-buffer ([GBuffer.h](../../engine/include/Veng/Renderer/GBuffer.h))

One new color attachment, no struct churn elsewhere:

```cpp
// G2 — packed material params. R=occlusion, G=roughness, B=metallic, A=emissive
// strength. RGBA8: roughness/metallic are perceptually fine at 8-bit; emissive
// strength is a low-dynamic scalar (the lighting pass scales albedo by it).
inline constexpr Format ORMFormat = Format::RGBA8Unorm;
```

`ColorUsage` (`ColorAttachment | Sampled`) applies to G2 as to G0/G1. The header comment
becomes the present-tense fact that the layout is albedo / normal / ORM + sampled depth.
`G0.a` is unused (reserved); only `G2.a` carries emissive strength.

### `GBufferOutput` gains one member ([material.slang](../../engine/assets/core/shaders/material.slang))

```hlsl
struct GBufferOutput
{
    float4 Albedo : SV_Target0;  // rgb sRGB albedo; a unused
    float4 Normal : SV_Target1;  // world-space normal in xyz
    float4 ORM    : SV_Target2;  // R occlusion, G roughness, B metallic, A emissive strength
};
```

### Metallic-roughness `MaterialParams` ([material.slang](../../engine/assets/core/shaders/material.slang))

The block grows from one albedo handle + `Factors` to the metallic-roughness set, still
within the 256-byte stride, patched by reflected offset (no normal-map handle yet — that
is Plan 02):

```hlsl
struct MaterialParams
{
    uint   BaseColor;        uint BaseColorSampler;   // base-color texture handle + sampler
    uint   ORM;              uint ORMSampler;         // occlusion-roughness-metallic texture
    float4 BaseColorFactor;  // tint (replaces Factors)
    float4 EmissiveFactor;   // rgb reserved (colored emissive is a follow-on); a = strength
    float  MetallicFactor;   float RoughnessFactor;
    float  OcclusionStrength; float Pad0;
};
```

This block is **64 bytes** (`4×uint = 16`, `2×float4 = 32`, `4×float = 16`), well within the
256-byte per-material stride.

The surface fragment ([brick.frag.slang](../../examples/hello-triangle/assets/shaders/brick.frag.slang)
and the core/test copies) samples base color and ORM, applies the factors, and writes
G0/G1/G2 — the interpolated world normal into G1, emissive strength
(`EmissiveFactor.a × sampled`) into `ORM.a`.

### The per-frame view-constants buffer

Per-view data the fullscreen lighting pass needs — the inverse view-projection (for
world-position reconstruction), the camera position, and, this plan, the single directional
light — lives in a **ring-buffered uniform buffer**, **not** push constants:

```hlsl
struct ViewConstants
{
    float4x4 InvViewProj;       // world-position reconstruction
    float4   CameraPosition;    // xyz; w unused
    float4   LightDirection;    // xyz; w unused  (single directional light this plan)
    float4   LightColor;        // rgb color, a intensity
    // grows in later plans: light-space matrix (Plan 04), view/proj for SSAO (Plan 05)
};
```

It is owned by the engine and registered as a per-frame binding in **set 0** alongside the
material parameter block (host-visible, persistently mapped, `framesInFlight` copies). The
current frame's copy is selected by **index fold** — *not* a dynamic-offset descriptor,
which mistranslates inside set 0's bindless Metal argument buffer on MoltenVK, exactly as
the material block avoids. `SceneRenderer::Execute` writes the current frame's region each
frame (stall-free, no `UploadSync`); the lighting pass reads it.

**Push constants are reserved for genuinely per-invocation selectors** — the lighting
pass's small bindless handle indices (G0/G1/G2/depth) — and nothing per-view. This is the
discipline every later pass inherits: per-view/per-frame data is a ring-buffered buffer;
push constants carry only small per-draw indices.

### Cook-Torrance lighting ([deferred_lighting.frag.slang](../../engine/assets/core/shaders/deferred_lighting.frag.slang))

The pass samples G0/G1/G2 + depth, reconstructs world position from the view-constants
`InvViewProj`, and evaluates GGX specular + Lambert diffuse over the (still single, this
plan) directional light:

- **World position from depth** — the engine already samples the `D32Sfloat` depth target
  (it is the one depth texture the engine reads). The pass forms NDC from the screen UV +
  sampled depth, transforms by `InvViewProj`, and perspective-divides — recovering world
  position without a fat position target (the bandwidth-cheap standard on the tile-based
  platform).
- **BRDF** — metallic workflow: `F0 = lerp(0.04, albedo, metallic)`, GGX normal
  distribution, Smith geometry, Fresnel-Schlick; diffuse `= albedo * (1 - metallic)`.
- **Ambient/occlusion** — a constant hemispheric ambient (sized so occlusion reads
  visibly, not the prior flat `0.03`) multiplied by `ORM.r × OcclusionStrength`. This is
  the term Plan 05's SSAO folds into; it must be large enough that occlusion is perceptible.
- **Emissive** — `albedo.rgb × ORM.a` added after lighting. Output stays linear HDR.

`SceneRenderer::Execute` builds `InvViewProj` (from the `SceneView` camera) and
`CameraPosition` (via the new `Camera::GetPosition()` accessor), packs the directional light,
and writes them into the current frame's view-constants region.

## Decisions

1. **One packed ORM target, RGBA8.** Roughness/metallic/occlusion are perceptually fine at
   8-bit unorm; one attachment keeps g-buffer bandwidth down on the tile-based primary
   platform. Higher precision or a split is a later layout change, not a v1 default.

2. **Emissive is a scalar in `ORM.a`.** A fullscreen deferred lighting pass has no per-pixel
   material, so emissive lives in the g-buffer. With the packed-ORM layout the only free
   space is `ORM.a`, carrying an emissive **strength** scaling the surface's own albedo.
   Colored emissive (a hue independent of albedo) needs a separate emissive target and is a
   named follow-on.

3. **Per-view data is a ring-buffered buffer, not push constants.** `InvViewProj`,
   `CameraPosition`, and the directional light ride a per-frame view-constants uniform
   buffer in set 0, ringed and index-folded like the material block (no dynamic offset).
   Push constants carry only the lighting pass's small per-invocation bindless handle
   indices. Every later pass (typed lights, shadows, SSAO) extends this buffer rather than
   the push block — there is no per-view push data anywhere in the deferred path.

4. **World position is reconstructed from depth, not stored.** The depth target the engine
   already samples + `InvViewProj` recovers world position in a few ALU ops; a dedicated
   position target would cost an extra high-precision attachment's bandwidth on the tile
   GPU. Reconstruction is the standard deferred choice and the reason the lighting pass
   gains `InvViewProj`.

5. **The format break is loud.** The block layout changes, so `CookedMaterialVersion` bumps
   and the loader rejects stale blobs; every material re-cooks under the build-wired cook.

6. **`material_data.slang` drift is guarded, not just mirrored.** The three byte-identical
   copies of `GBufferOutput`/`MaterialParams`
   (`engine/assets/core/shaders/material.slang`,
   `examples/hello-triangle/assets/shaders/material_data.slang`,
   `tests/gpu/assets/shaders/material_data.slang`,
   `tests/cooker/fixtures/shaders/material_data.slang`) are this plan's largest struct
   change. A new unit test `static_assert`s the engine-side `MaterialParams` `sizeof` and
   each field `offsetof` against the documented layout, so a divergence is a build error,
   not a silent offset-patch misread.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/GBuffer.h` | Add `ORMFormat`; present-tense header; note `G0.a` unused, `G2.a` emissive. |
| `engine/include/Veng/Scene/Camera.h` (+ `.cpp`) | `Camera::GetPosition()` accessor (`inverse(View())[3].xyz`). |
| `engine/assets/core/shaders/material.slang` | `GBufferOutput.ORM`; metallic-roughness `MaterialParams`; `ViewConstants`. |
| `engine/assets/core/shaders/deferred_lighting.frag.slang` | Cook-Torrance BRDF; world-pos-from-depth via `ViewConstants.InvViewProj`; per-invocation handle push; occlusion + scalar emissive. |
| `engine/include/Veng/Renderer/BindlessRegistry.h` (+ `.cpp`) | Per-frame view-constants uniform binding (set 0), ringed + index-folded like the material block. |
| `engine/src/Renderer/SceneRenderer.cpp` | Build `InvViewProj`/`CameraPosition`/directional light → view-constants region; geometry pass binds 3 color targets (+ G2 image owned/imported/bindless-registered). |
| `cooker/src/Importers/MaterialImporter.cpp` | Surface output-contract check widens to require `SV_Target0/1/2`. |
| `examples/hello-triangle/assets/shaders/brick.frag.slang`, `material_data.slang` | Metallic-roughness fragment; mirror the struct changes. |
| `examples/hello-triangle/assets/materials/brick.vmat.json` | PBR fields (ORM handle, factors); add the needed ORM source texture/`.tex.json`. |
| `examples/hello-triangle/.../*.prefab.json` | Add a **receiver plane** beneath the sphere (so Plans 04/05 have geometry to shadow/occlude). |
| `tests/gpu/assets/shaders/material_data.slang`, `tests/cooker/fixtures/shaders/material_data.slang` | Mirror `material_data.slang`; the **positive** surface fixtures gain `SV_Target2`. The `surface_wrong_output` negative fixture (single `SV_Target0`, `"domain": "surface"`) is **unchanged** — it still correctly fails the widened 3-target check; confirm it still rejects. |
| `tests/unit/...` (new) | `static_assert` guard on `MaterialParams` `sizeof`/`offsetof`. |
| `tests/golden/hello_triangle_scene.png` | Regenerated — the scene is now PBR-shaded with a receiver plane. |

## Verification

- Clean build; `vengc` + `libveng` compile with the extended block, 3-target g-buffer, and
  view-constants buffer; the `static_assert` drift guard passes.
- `ctest` cooker band: surface materials require `SV_Target0/1/2`; the positive fixtures
  cook, the `surface_wrong_output` negative fixture is still a located cook error.
- `gpu` band + `smoke_golden`: the capture is regenerated and shows PBR shading (specular
  highlight, roughness response) on the sample sphere over the receiver plane; the
  validation gate is clean (3-target `RenderingInfo` matches the g-buffer formats/usage; the
  view-constants binding follows the material-block pattern).
- A **property assertion** beyond the golden (this is the riskiest plan): a gpu test asserts
  a metallic/smooth pixel shows a specular response a rough/dielectric pixel does not, so the
  BRDF is pinned by a property, not only a re-blessed image.
- A blob cooked at the prior material version is rejected by the loader's version assert.
