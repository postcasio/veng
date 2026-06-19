# Plan 03 — multiple & typed lights

**Goal:** lift the lighting pass from one directional light to a **list of typed lights**
(directional / point / spot). The `Light` builtin gains a type tag and the fields each type
needs; the light list rides a dedicated SSBO; the Cook-Torrance pass loops over it with
distance and cone attenuation.

## What lands

### The typed `Light` builtin ([Components.h](../../engine/include/Veng/Scene/Components.h))

The `Light` struct and its `VE_REFLECT` block live in `Components.h` (registered in
`BuiltinTypes.cpp`). `Light` gains a type enum and the punctual fields (position comes from
the entity's `Transform`, not stored on `Light`):

```cpp
enum class LightType : u32 { Directional = 0, Point = 1, Spot = 2 };

struct Light
{
    LightType Type      = LightType::Directional;
    vec3      Direction = {0, -1, 0};  // directional + spot
    vec3      Color     = {1, 1, 1};
    f32       Intensity = 1.0f;
    f32       Range     = 10.0f;       // point + spot falloff radius
    f32       InnerCone = 0.0f;        // spot, radians (full intensity within)
    f32       OuterCone = 0.5f;        // spot, radians (zero beyond)
};
```

`LightType` is authored with `VE_LEAF` (a reflected enum) next to `Light` in `Components.h`;
the new `Light` fields are added to its `VE_REFLECT` block with editor metadata
(`Min`/`Max`/`Step` for the cone/range/intensity drags). `Light`'s `TypeId` is unchanged (a
field addition is schema-tolerant — old prefabs read back with defaults); `LightType`'s new
`TypeId` and any new field-type ids are minted with `vengc generate-type-id`.

### The light list SSBO

`SceneView` stops carrying a single `Light` by value (Plan 01 placed the lone directional
light in the view-constants buffer; that single-light field is removed here) and carries a
**bounded span** the renderer fills into a dedicated light buffer:

```cpp
struct GpuLight  // std430-friendly, matches the shader struct
{
    vec4 PositionRange;   // xyz world position, w range
    vec4 DirectionType;   // xyz direction, w LightType
    vec4 ColorIntensity;  // rgb color, a intensity
    vec4 Cone;            // x cos(inner), y cos(outer), zw pad
};
```

`SceneRenderer::Execute` walks `View<Transform, Light>` (capped at a `MaxLights` constant —
`16`), packs each into a `GpuLight`, and writes them into a renderer-owned, host-mapped,
frames-in-flight-ringed light buffer — the **same N-buffer + index-fold pattern** the
material block and the Plan 01 view-constants buffer use (no dynamic offset, dodging the
MoltenVK set-0 trap). Unlike the write-once material block, the light buffer is **fully
rewritten into the current frame's region every frame** (light count and contents are
per-frame data), so it needs no dirty-counter propagation. The lighting pass receives the
buffer's binding + the live light count (a per-invocation push uint) and loops.

### The lighting loop ([deferred_lighting.frag.slang](../../engine/assets/core/shaders/deferred_lighting.frag.slang))

The Cook-Torrance evaluation from Plan 01 is factored into a per-light function and called
in a loop over `[0, lightCount)`:

- **Directional** — `L = -normalize(DirectionType.xyz)`, no attenuation, ignores
  `PositionRange`/`Cone` (Plan 01's path).
- **Point** — `L` toward `PositionRange.xyz`; inverse-square attenuation with a smooth
  `Range` cutoff.
- **Spot** — point attenuation × a smoothstep cone factor between `Cone.x` (cos inner) and
  `Cone.y` (cos outer).

The ambient/occlusion and emissive terms are applied once outside the loop.

## Decisions

1. **Position is the `Transform`'s, not a `Light` field.** A light is placed by its entity's
   transform like everything else; `Light` carries only the light's own parameters. The
   renderer reads world position from `ComputeWorldMatrices` / the entity transform when
   packing `GpuLight`, so a parented/animated light moves correctly.

2. **A fixed `MaxLights` cap (16), no culling.** v1 loops every light for every pixel up to
   the cap; the per-pixel cost is `O(MaxLights)` full Cook-Torrance evaluations, which is
   why clustered/tiled light culling is the named follow-on. The cap keeps the SSBO a fixed
   size and the loop bounded.

3. **The light buffer rings like the material block, fully rewritten per frame.** Per-frame
   light data is written into the current frame-in-flight's region of a host-mapped buffer,
   selected by index fold — stall-free, no `UploadSync`, no dynamic offset. Each frame
   writes its whole region fresh (no cross-region dirty propagation); no recompile (light
   **count** is per-frame data, not topology).

4. **Shadowing stays directional-only (Plan 04).** Adding point/spot here does not add their
   shadows; punctual lights are unshadowed this planset. The loop leaves a shadow-factor hook
   the directional case fills in Plan 04.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/Components.h` | `LightType` (`VE_LEAF`); `Light` fields + `VE_REFLECT` metadata; minted ids. |
| `engine/src/Scene/BuiltinTypes.cpp` | Register `LightType`; the updated `Light` schema. |
| `engine/include/Veng/Renderer/SceneRenderer.h` | `SceneView` carries a light span (+ `MaxLights`); drop the single-light field. |
| `engine/src/Renderer/SceneRenderer.cpp` | Pack `View<Transform, Light>` → ringed `GpuLight` buffer; bind + push count to the lighting pass. |
| `engine/assets/core/shaders/deferred_lighting.frag.slang` | `GpuLight` SSBO; per-light function; the directional/point/spot loop; drop the single-light view-constants fields. |
| `examples/hello-triangle/.../*.prefab.json` or app setup | A second (point/spot) light to exercise the loop. |
| `tests/...` | A scene-renderer test asserting multiple lights accumulate; the two-renderer interleaved test still passes. |

## Verification

- Clean build; the reflected `Light`/`LightType` round-trip through the prefab cook + load
  (an old single-`Light` prefab still loads, new fields defaulted).
- `gpu` band + `smoke_golden`: regenerated for the second light; a single directional scene
  matches Plan 01's result (the loop with one directional light is Plan 01's math).
- Validation gate clean (the light SSBO binding + ring write follow the material-block
  index-fold pattern already exercised — no dynamic-offset descriptor).
