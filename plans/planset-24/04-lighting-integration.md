# Plan 04 — lighting integration: per-shadowed-light sample

**Goal:** the deferred lighting punctual loop samples each shadowed light's map and multiplies its
contribution by the visibility — a **projective `SampleCmp`** of a spot's tile, a **major-axis
face `SampleCmp`** of a point's six faces — gated by the light's shadow slot (`Cone.z`, Plan 02), with
PCF + a per-light bias, mirroring `DirectionalShadowVisibility`. An unshadowed light (slot `-1`) skips
the sample and is fully lit, so cost scales with the shadowed budget, not the light count. Consumes
Plan 02's set-1 bindings + records and Plan 03's rendered atlas. Completes the punctual-shadow
visibility path.

## What lands

### `PunctualShadowVisibility` ([engine/assets/core/shaders/shadow.slang](../../engine/assets/core/shaders/shadow.slang))

A sibling to `DirectionalShadowVisibility`, in the same set-1 shadow header, reading the
`PunctualShadowBlock` (binding 3) and the punctual atlas (binding 4) Plan 02 declared. Given a
fragment's world position, normal, the surface→light direction `L`, and a shadow **slot**, it returns
the light's visibility:

1. **Gate on the slot.** `slot < 0` → return `1.0` (unshadowed — the loop skips the sample). The
   renderer wrote `-1` into `Cone.z` for every light past the budget or not selected, so an
   unshadowed light costs nothing.

2. **Spot — projective `SampleCmp`.** For a spot record (`Params.x == 2`), transform `worldPos` by
   `Records[slot].ViewProj[0]` (the tile remap already baked in, Plan 02), perspective-divide to the
   tile UV + reference depth, and `SampleCmp` the tile with a PCF tap grid — **the exact
   `SampleCascade` body**, generalized to read a record's matrix instead of a cascade's. Every tap UV
   is clamped to the record's tile sub-rect (inset a half-texel) so a tap cannot bleed into a
   neighbouring record's tile — the atlas's one cost, the same one the cascade path pays.

3. **Point — major-axis face select + projective `SampleCmp`.** For a point record (`Params.x == 1`),
   pick the face from the **major axis of the light→fragment direction** (`-L`, since `L` points
   surface→light): the largest-magnitude component selects one of the six `CubeFace`-ordered faces
   (matching Plan 01's canonical basis), then `SampleCmp` `Records[slot].ViewProj[face]`'s tile
   exactly as the spot does. The face select and the render's face assignment agree by construction
   (Plan 01's unit test pins the partition), so the sampled face is the one that rendered that
   direction.

4. **Per-light bias.** The reference-depth bias is the record's `Params.w` bias plus a slope-scaled
   term (∝ `1 − dot(N, L)`), mirroring the cascade path's acne/peter-panning balance. The bias is
   per-record (a spot tile and a point face cover different world-per-texel), carried on the record so
   the renderer sets it from the light's range/resolution.

The point face seam is handled by the same half-texel tile-inset clamp the spot uses; a tap near a
cube-face boundary clamps inside its face's tile rather than reading the neighbouring face (Plan 01's
seam-consistency test confirms the two faces agree at the edge, so the clamp loses no correctness).

### The punctual loop multiplies in the visibility ([engine/assets/core/shaders/deferred_lighting.frag.slang](../../engine/assets/core/shaders/deferred_lighting.frag.slang))

The light loop today computes a directional `visibility` and passes `1.0` for point/spot
([deferred_lighting.frag.slang](../../engine/assets/core/shaders/deferred_lighting.frag.slang), the
`isDirectional ? DirectionalShadowVisibility(…) : 1.0` line). The punctual arm changes to read the
light's slot and sample:

```hlsl
float visibility;
if (isDirectional)
    visibility = DirectionalShadowVisibility(worldPos, N, L, viewDepth);
else
{
    int slot = int(light.Cone.z);            // -1 unshadowed, else the punctual record index
    visibility = PunctualShadowVisibility(worldPos, N, L, slot);
}
direct += EvaluateLight(N, V, L, radiance * attenuation, albedo, roughness, metallic, f0) * visibility;
```

These edits live in the **one shared fragment body**; the AO-fold variant
(`deferred_lighting_ssao.frag.slang`, which `#define`s `VE_USE_SSAO` and `#include`s this file)
inherits them unchanged, so both lighting pipelines sample punctual shadows from a single authored
source — the same single-body property the directional path relies on.

### `DebugView::PunctualShadows` — the arm that distinguishes the golden move

The `DebugView::PunctualShadows` arm landed in Plan 03 (it blits a selected punctual atlas tile's raw
depth, pinning the producer). It needs no shader change here; this plan's golden move (Plan 05) is
pinned by it plus the gpu-band presence assertion below — a re-blessed image alone is not the only
guard. (Unlike CSM's `Cascades` tint, the punctual map's depth-blit arm already exists; the
lit-result distinguisher is the gpu assertion that a shadowed spot/point darkens its receiver where an
occluder stands and lights it where none does.)

## Decisions

1. **Hardware `SampleCmp` through the shared set-1 comparison sampler.** A spot tile, a point face,
   and a cascade tile all `SampleCmp` the same hardware comparison sampler (Plan 02, set 1, off the
   bindless argument buffer), so the MoltenVK mistranslation that bars it from set 0 does not apply —
   the same bet planset-20 verified for the directional path, now extended. No manual in-shader
   compare. The validation gate pins MoltenVK accepts the punctual `SampleCmp` in a plain descriptor
   set.

2. **The spot and point paths reuse the cascade sample body.** A spot tile is a cascade tile by
   another name (a projective `SampleCmp` of a baked-remap matrix's tile); a point face is six of
   them with a major-axis select in front. Generalizing `SampleCascade` to take a record's matrix +
   tile keeps one sample/PCF/clamp implementation, not three — the 2D-atlas choice (Plan 02 decision
   3) is what makes this reuse possible.

3. **Slot-gated, so cost scales with the shadowed budget.** An unshadowed light (slot `-1`, the
   common case past `MaxShadowedPunctual`) short-circuits to full visibility with no sample, so a
   scene with many unshadowed punctual lights pays only for the budgeted few — the bounded-loop
   property the README's decision 1/4 promises.

4. **Major-axis face select, agreeing with the render by construction.** The largest-magnitude
   component of `-L` picks the cube face, the same partition Plan 01's basis renders into and its unit
   test pins. So the sampled face is the rendering face — no per-face containment test, the cube
   analogue of the cascade's view-depth select.

## Files

| File | Change |
|---|---|
| `engine/assets/core/shaders/shadow.slang` | `PunctualShadowVisibility` (slot gate; spot projective `SampleCmp`; point major-axis face select + `SampleCmp`; per-record + slope bias; tile-inset clamp), generalizing `SampleCascade`. |
| `engine/assets/core/shaders/deferred_lighting.frag.slang` | The punctual loop reads `light.Cone.z` as the slot and multiplies in `PunctualShadowVisibility`; the directional arm unchanged. (The `_ssao` variant inherits it via its `#include`.) |
| `engine/src/Renderer/SceneRenderer.cpp` | Set each shadowed record's `Params.w` bias from the light's range/resolution when filling the record (Plan 02's fill loop). |
| `tests/gpu/scene_renderer.cpp` (extend) + the `gpu` suite list | Punctual-shadow lit-result assertions (occluded-vs-unoccluded receiver, spot + point). |

## Verification

- Clean build; `ctest` green across the bands.
- **`gpu` band** (`SKIP_RETURN_CODE 77`, skips with no ICD): presence alone cannot distinguish a
  correct sample from a broken one (a transposed tile remap or an inverted compare still darkens
  something), so the assertion is **occlusion-distinguishing**:
  - **Spot:** a shadowed spot over a receiver with an occluder between them darkens the receiver
    **behind** the occluder (in shadow) and lights it **beside** the occluder (lit) — a sampled
    region pair with a clear visibility delta, not just "darker somewhere."
  - **Point:** a shadowed point with an occluder casts a shadow on the receiver in the occluder's
    direction (the correct major-axis face), and a probe in a **different** face's direction is
    unshadowed — pinning the face select, not just shadow presence.
  - **Slot gate:** a punctual light past `MaxShadowedPunctual` (slot `-1`) lights its receiver fully
    (no sample), confirming the budget bound.
  - **Validation gate under `VE_DEBUG`** clean — the punctual `SampleCmp` against the set-1 atlas +
    shared comparison sampler carries no MoltenVK validation error (the dedicated-set hardware-compare
    path, now exercised by the punctual sample). Pin the gate after this plan (README common-to-all).
- **`smoke_golden` is byte-identical** — the sample scene gains the shadowed punctual light in **Plan
  05**, not here; with no shadowed punctual light in the smoke pose, the slot is `-1` for every light
  and the lit image does not move. The golden moves once, in Plan 05. No regeneration here.
- Smoke PPM correct size + exit 0 through the launcher.
