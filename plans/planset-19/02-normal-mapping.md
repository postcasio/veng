# Plan 02 — tangent-space normal mapping

**Goal:** add tangent-space normal mapping to the opaque surface material. A metallic
workflow without normal maps is half a PBR material; this is the self-contained quality
increment that completes Plan 01's metallic-roughness surface. It is **contained to the
surface vertex/fragment** — the perturbed normal still lands in G1, which the lighting pass
reads unchanged, so no downstream battery is affected. It moves the rendered image, so the
golden is regenerated.

## What lands

### `MaterialParams` gains the normal-map handle ([material.slang](../../engine/assets/core/shaders/material.slang))

Two handle slots are appended to the Plan 01 block:

```hlsl
struct MaterialParams
{
    uint   BaseColor;        uint BaseColorSampler;
    uint   ORM;              uint ORMSampler;
    uint   Normal;           uint NormalSampler;      // tangent-space normal map (new)
    uint   Pad0;             uint Pad1;
    float4 BaseColorFactor;
    float4 EmissiveFactor;
    float  MetallicFactor;   float RoughnessFactor;
    float  OcclusionStrength; float Pad2;
};
```

This is **80 bytes**, still well within the 256-byte stride. The `static_assert` drift
guard from Plan 01 is updated to the new `sizeof`/`offsetof`.

### Tangent emission ([surface.vert.slang](../../engine/assets/core/shaders/surface.vert.slang))

The standard surface vertex stage emits the **world tangent** alongside the world normal.
The canonical vertex layout already carries a tangent attribute (from the runtime-primitive
work), so this is wiring an existing attribute through, not a layout change.

### Normal perturbation ([brick.frag.slang](../../examples/hello-triangle/assets/shaders/brick.frag.slang) and copies)

The surface fragment samples the tangent-space normal map, builds the TBN from the
interpolated world normal + world tangent (+ derived bitangent), perturbs the normal, and
writes the perturbed world normal into G1. When a material has no normal-map handle, it
writes the geometric world normal unchanged (handle `0` / unbound → identity perturbation).

## Decisions

1. **Tangent-space, canonical-layout tangent.** The perturbation uses the canonical
   vertex's tangent attribute; no new vertex layout. The result is a world-space normal in
   G1 identical in form to the geometric normal, so the deferred lighting pass is untouched.

2. **Off the spine, on purpose.** No battery (typed lights, shadows, SSAO, bloom) reads a
   normal map differently from a geometric normal — the perturbed normal is just what lands
   in G1. Keeping normal mapping out of Plan 01 makes the spine's golden move attributable
   to PBR shading alone, and this plan's golden move attributable to surface detail alone.

3. **The block layout grows; the version bumps again.** Appending the normal-map handles
   changes `MaterialParams`, so `CookedMaterialVersion` bumps and stale blobs reject loudly;
   every material re-cooks.

## Files

| File | Change |
|---|---|
| `engine/assets/core/shaders/material.slang` | `MaterialParams` gains `Normal`/`NormalSampler`. |
| `engine/assets/core/shaders/surface.vert.slang` | Emit world tangent alongside world normal. |
| `examples/hello-triangle/assets/shaders/brick.frag.slang`, `material_data.slang` | Sample + perturb via TBN; mirror the struct change. |
| `examples/hello-triangle/assets/materials/brick.vmat.json` | Normal-map handle field; add the normal-map source texture/`.tex.json`. |
| `tests/gpu/assets/shaders/material_data.slang`, `tests/cooker/fixtures/shaders/material_data.slang` | Mirror `material_data.slang`. |
| `tests/unit/...` | Update the `MaterialParams` `static_assert` to the 80-byte layout. |
| `tests/golden/hello_triangle_scene.png` | Regenerated — the sphere shows normal-mapped detail. |

## Verification

- Clean build; the `static_assert` drift guard passes at the new layout.
- `ctest` cooker band: the normal-map material cooks; the surface contract is unchanged
  (still `SV_Target0/1/2`).
- `gpu` band + `smoke_golden`: regenerated to show normal-mapped surface detail; a material
  with no normal map renders the geometric normal (identity perturbation).
- A blob cooked at the prior material version is rejected by the loader's version assert.
