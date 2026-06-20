# Plan 02 — shadow-system / set-1 generalization

**Goal:** grow set 1 from "the directional system" to "a **shadow system**" — keep the directional
cascade atlas + immutable comparison sampler + `ShadowConstants` block untouched, and **add** the
punctual shadow atlas (resource + format + comparison sampler placement), a per-light shadow-record
array as a **separate set-1 binding** (so the 288-byte `ShadowConstantsBlock` and its `static_assert`
are preserved, not reshaped), and a `GpuLight` shadow **slot** riding the existing `Cone.zw` padding.
Descriptor-layout + resource work; the directional path is **unchanged in behavior**. This is the
prerequisite for both the punctual render passes (Plan 03) and the lighting sample (Plan 04).
Consumes Plan 01's `SpotShadowView` / `PointShadowView`.

## What lands

### A `MaxShadowedPunctual` budget constant ([engine/include/Veng/Renderer/SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h))

A small fixed ceiling beside `MaxCascades`/`MaxLights`, sizing the punctual atlas and the record
array. The worst case it bounds is `N` point lights = `6N` cube faces (24 depth tiles at `N = 4`).

```cpp
// The maximum number of shadow-casting point/spot lights. The first N shadow-casting
// punctual lights (by the per-frame selection) get a shadow map; the rest light
// unshadowed, as all punctual lights do today. Small by design: a point light costs
// six cube-face redraws of its caster set, so N bounds the punctual shadow atlas and
// the lighting loop's sample set at 6N depth tiles / sample faces.
inline constexpr u32 MaxShadowedPunctual = 4;
```

### The per-light shadow record + its set-1 binding ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))

Each shadowed punctual light needs, on the GPU: its view-proj(s) (1 for a spot, 6 for a point), its
atlas tile rect(s), its type, near/far, and a bias. These ride a **new set-1 binding** (binding 3),
not the existing `ShadowConstantsBlock` (binding 2). The directional block stays exactly as it is:

```cpp
struct PunctualShadowRecord            // one shadowed punctual light
{
    mat4 ViewProj[CubeFaceCount];      // 384 — [0] for a spot, [0..5] for a point (tile-remap baked in)
    vec4 PositionRange;                // 16  — xyz world position, w range (for the cube face select + linearize)
    vec4 Params;                       // 16  — x type (1 point / 2 spot), y near, z far, w bias
};                                     // 416

struct PunctualShadowBlock             // set 1, binding 3
{
    PunctualShadowRecord Records[MaxShadowedPunctual];
};
```

- **Binding placement.** The record array is a **separate binding**, not a growth of
  `ShadowConstantsBlock`. That block is a std140 dynamic uniform with a hard-asserted **288-byte**
  size and a byte-identical CPU mirror ([SceneRenderer.cpp:179](../../engine/src/Renderer/SceneRenderer.cpp),
  `static_assert(sizeof(ShadowConstantsBlock) == 288, …)`); adding a binding preserves that layout
  and its `static_assert` rather than reshaping it. The directional block, its ring, and its
  dynamic-offset bind are all untouched.
- **Uniform vs SSBO.** `sizeof(PunctualShadowBlock)` at `MaxShadowedPunctual = 4` is `4 · 416 = 1664`
  bytes — comfortably inside the 16 KiB min-max-uniform guarantee, so it is a **std140 uniform**
  bound like `ShadowConstants` (ringed `framesInFlight` regions, current region by a dynamic offset).
  Its CPU mirror gets a `static_assert` against the std140 size the way `ShadowConstantsBlock` does.
  (Were the budget to grow past the uniform ceiling, the binding becomes an SSBO with no layout
  change visible to the shader — the record struct is std140/std430-identical.)
- **The shadow slot rides `Cone.zw`.** `GpuLight` gains no field — its `Cone` is `vec4(cosInner,
  cosOuter, 0, 0)` today ([SceneRenderer.cpp:151](../../engine/src/Renderer/SceneRenderer.cpp)), the
  `zw` pure padding. The renderer packs the light's shadow **slot** (an index into `Records`, or
  `-1` for unshadowed) into `Cone.z`, so `LightStride` (64) and its `static_assert` are unchanged.
  The shader reads `light.Cone.z` as the slot. (`Cone.w` stays reserved.)

### The punctual shadow atlas ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp) + [ShadowScenePass](../../engine/src/Renderer/ShadowScenePass.h))

A **second** D32 depth atlas alongside the directional cascade atlas, `DepthAttachment | Sampled`,
created at `Create`/`Resize`/`Configure` and recreated through the deferred `Release()` window. It
is a **2D atlas tiling the spot maps and point cube faces** — `MaxShadowedPunctual · CubeFaceCount`
tiles of `PunctualShadowResolution²`, the most general packing (a point uses six tiles, a spot one).
A spot's tile and a point's six faces are addressed by their record's baked tile-remap matrices,
exactly as a cascade tile is. It is **off bindless** (the same closed-resource placement the cascade
atlas uses) and reaches the lighting pass through a set-1 binding (binding 4, the sampled image) plus
the shared comparison sampler.

> **Why a 2D atlas, not a cube-array.** The README leaves the 2D-atlas-vs-cube-array choice to
> execution time. A **2D tile atlas** is chosen here on three grounds: it reuses the cascade atlas's
> exact machinery (one depth image, per-tile viewports, baked tile-remap matrices, projective
> `SampleCmp`), so a point face and a cascade sample by the identical code path; it packs spots and
> point faces in one resource without a separate cube image per light; and it keeps the render
> single-`RenderingInfo`-per-pass (planset-20 decision 6) with no multiview (which MoltenVK
> implements by instance multiplication anyway, README decision 2). A cube/cube-array would force a
> per-light image and a direction-vector sample distinct from the projective cascade path — more
> surface for no gain at this budget.

### Set 1 grows two bindings; the comparison sampler is shared ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp), both lighting layouts)

Set 1 today holds bindings 0–2 (cascade atlas / immutable comparison sampler / `ShadowConstants`
dynamic uniform). It **adds**:

- **binding 3** — the `PunctualShadowBlock` dynamic uniform (ringed, current region by dynamic
  offset, beside the `ShadowConstants` ring).
- **binding 4** — the punctual shadow atlas (sampled image), written once on
  `Create`/`Configure`/`Resize` (single-copy, serialized by the graph's write→sample barrier, like
  the cascade atlas).

The **immutable comparison sampler stays a single binding** (binding 1) — a point's faces, a spot's
tile, and the cascades all `SampleCmp` through the **same** hardware comparison sampler, so no second
sampler is added. It stays **in set 1, off the set-0 bindless argument buffer**, the placement the
directional sampler already uses because a comparison sampler mistranslates inside the Metal argument
buffer on MoltenVK; a plain set-1 descriptor is unaffected. Both lighting pipeline layouts
(`m_LightingLayout` and `m_SsaoLightingLayout`) gain bindings 3–4 — but the shader-side work (Plan
04) is authored once in the shared fragment body, so both variants inherit it.

When punctual shadows are **off** (Plan 05's toggle, or the budget is zero this frame), binding 4
binds the **same `SceneRenderer`-owned dummy depth** the directional path already allocates
(`m_DummyShadowImage`, cleared to 1.0 → `SampleCmp` yields full visibility) and binding 3 binds a
**zeroed `PunctualShadowBlock`** (every record's `Params.x` type = 0, read as "no map"), so the
layout is always satisfied. These dummies are owned by `SceneRenderer` (long-lived past any
`Configure`), like the directional dummies — the layout must be satisfiable whenever it exists,
independent of any pass's lifetime.

### `SceneView` carries the per-light shadow assignment ([engine/include/Veng/Renderer/SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h))

`SceneView` already carries the packed lights and the cascade matrices. It gains the punctual shadow
**records** (the CPU mirror the shadow pass renders from and the renderer flushes to binding 3) and a
**count**:

```cpp
// The shadowed punctual lights selected this frame (the first MaxShadowedPunctual
// shadow-casting point/spot lights). The punctual shadow pass renders each record's
// view(s) into the atlas; the lighting pass samples Records[slot]. PunctualShadowCount
// records are valid. A caller's values are overwritten by the renderer each Execute.
std::array<PunctualShadowRecord, MaxShadowedPunctual> PunctualShadows{};
u32 PunctualShadowCount = 0;
```

The renderer's light-pack loop ([SceneRenderer.cpp:1591](../../engine/src/Renderer/SceneRenderer.cpp))
gains the selection: as it walks `(Transform, Light)`, the first `MaxShadowedPunctual` point/spot
lights are assigned a slot, their `PunctualShadowRecord` filled via `ComputeSpotShadowView` /
`ComputePointShadowView` (Plan 01) with the tile-remap baked in, and that slot written into the
`GpuLight`'s `Cone.z`; the rest get slot `-1`. This plan **wires the selection + record fill + the
GPU flush**; the render passes (Plan 03) and the sample (Plan 04) consume it.

## Decisions

1. **A separate binding preserves the directional block's `static_assert`.** The 288-byte
   `ShadowConstantsBlock` has a byte-identical CPU mirror and a hard size assert; reshaping it to
   carry punctual records would churn that layout and risk the directional path. A new binding leaves
   it untouched and the directional behavior unchanged — the README's stated preference (decision 3).

2. **The shadow slot rides `Cone.zw`, so `LightStride` is unchanged.** `GpuLight` is exactly 64
   bytes with `Cone.zw` already padding; packing the slot into `Cone.z` adds no field and keeps the
   `static_assert(sizeof(GpuLight) == LightStride)` and the shader's `Load<GpuLight>` stride intact.
   This is the README's decision 4 — the slot is per-light state that already has a home.

3. **A 2D tile atlas, not a cube-array.** Reuses the cascade atlas machinery whole (per-tile
   viewport render, baked tile-remap, projective `SampleCmp`), packs spots and point faces in one
   resource, and keeps the render single-pass-per-`RenderingInfo`. A cube-array buys a
   direction-vector sample at the cost of a per-light image and a second sample path — no gain at a
   four-light budget. (README decision 3 left this to execution time; this is the call.)

4. **One comparison sampler, shared across directional and punctual.** The hardware compare is the
   same LESS-or-equal 2×2-PCF operation for a cascade tile, a spot tile, and a point face; one
   immutable sampler in set 1 serves all three. No second sampler binding.

5. **The selection is per-frame; the budget is a recompile knob.** Which lights are shadowed is
   chosen each `Execute` (the renderer already selects/packs lights per frame); `MaxShadowedPunctual`
   sizes the atlas + record array, so it (and Plan 05's `PunctualShadows` toggle) is a
   `Configure`/`Resize` recompile knob. The per-frame records never recompile.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `MaxShadowedPunctual`; `PunctualShadowRecord`; `SceneView::PunctualShadows` + `PunctualShadowCount`; a `PunctualShadowResolution` setting field (sizing). |
| `engine/src/Renderer/SceneRenderer.cpp` | `PunctualShadowBlock` + its `static_assert`; the punctual atlas (create/recreate via the retire path); set-1 bindings 3–4 on **both** lighting layouts; dummy-bind when off (reuse `m_DummyShadowImage` + a zeroed punctual block); the light-pack loop's slot selection + `PunctualShadowRecord` fill (via Plan 01's `Compute*ShadowView`) + `Cone.z` slot write + the binding-3 ring flush. |
| `engine/src/Renderer/ShadowScenePass.{h,cpp}` | Expose the punctual atlas view (`GetPunctualShadowView`) for the set-1 bound-view handoff, or a sibling pass owns it (Plan 03 finalizes which). |
| `engine/assets/core/shaders/shadow.slang` | The `PunctualShadowRecord`/`PunctualShadowBlock` struct + the set-1 binding-3/4 declarations (the sample helpers land in Plan 04). |
| `tests/gpu/scene_renderer.cpp` (extend) + the `gpu` suite list | The set-1-coexists assertion: set 0 = registry, set 1 = directional + punctual bindings coexist on both lighting pipelines without a validation error. |

## Verification

- Clean build; `include_hygiene` still compiles the public headers (the `PunctualShadowRecord` in
  `SceneRenderer.h` is glm-only — no backend leak).
- The `static_assert`s hold: `ShadowConstantsBlock == 288` (unchanged), `GpuLight ==
  LightStride` (unchanged), and the new `PunctualShadowBlock` matches its std140 size; the directional
  ring stride and dynamic offset are untouched.
- **`gpu` band** (`SKIP_RETURN_CODE 77`): both lighting pipelines build with set 0 (registry) + set 1
  (bindings 0–4) coexisting; binding 4 binds the dummy depth and binding 3 a zeroed block when the
  budget is zero, and the layout is satisfied (no descriptor validation error). The punctual atlas is
  sized to `MaxShadowedPunctual · CubeFaceCount` tiles.
- **`smoke_golden` is byte-identical** — this plan adds descriptor/resource infrastructure but renders
  no punctual shadow yet (the passes are Plan 03, the sample Plan 04); the dummy bind yields full
  visibility, so the lit image does not move. No golden regeneration.
- **Validation gate clean under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`): the new
  set-1 bindings, the second comparison-sampler-shared atlas, and the dummy binds carry no MoltenVK
  validation error — the dedicated-set placement of the second atlas + the shared comparison sampler
  is the specific portability point this gate pins (the punctual atlas sits in a plain set-1
  descriptor, unaffected by the argument-buffer bar).
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
