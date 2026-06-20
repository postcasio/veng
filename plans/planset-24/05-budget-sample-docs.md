# Plan 05 — budget/settings + sample + docs/roadmap re-cut

**Goal:** expose the authoring surface (the `MaxShadowedPunctual` budget — already a constant from
Plan 02 — and a `PunctualShadows` toggle, both recompile knobs), grow the sample scene a shadowed
punctual light, **regenerate the golden once** (the planset's single deliberate render change), run
the validation gate, and re-cut the roadmap — moving shadowed punctual lights to **delivered** and
naming clustered/tiled light culling + cached/static shadow maps as the next increments. Consumes
Plans 01–04.

## What lands

### The `PunctualShadows` toggle ([engine/include/Veng/Renderer/SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h))

`SceneRendererSettings` gains the toggle beside `Shadows` (the directional toggle):

```cpp
// Whether the budgeted shadow-casting point/spot lights cast shadows. A topology
// change: it inserts/removes the punctual shadow depth pass and the lighting pass's
// punctual sample, so it drives a Configure → recompile. With it off, every punctual
// light's shadow slot is -1 and the lighting pass reads full visibility for them
// (the directional Shadows path is independent). MaxShadowedPunctual caps how many
// punctual lights are shadowed when it is on.
bool PunctualShadows = true;

// The per-tile shadow edge length in texels for the punctual atlas (a spot uses one
// tile, a point six). Sizing: changing it recreates the punctual atlas through the
// deferred retire path and recompiles.
u32 PunctualShadowResolution = 1024;
```

`PunctualShadowResolution` (declared in Plan 02 as the sizing field) is surfaced here as the settings
knob; `MaxShadowedPunctual` stays a compile-time constant (it sizes the fixed atlas + record array,
like `MaxLights`/`MaxCascades`). The toggle gates the punctual pass insertion (Plan 03) and the
per-light slot assignment (Plan 02's selection writes `-1` to every light when off).

### The sample scene grows a shadowed punctual light ([examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp))

The sample already has a directional light and a warm **point** light at `(1.5, 0.5, 1.5)`, `Range`
6, over a sphere and a plane ([main.cpp:184](../../examples/hello-triangle/main.cpp)). With
`PunctualShadows` on (default) and the budget non-zero, that point light is **selected as the first
shadowed punctual light** and casts the sphere's shadow onto the plane — a new shadow in the smoke
pose, the deliberate render change this planset makes. (If the existing point's geometry casts no
clear shadow in the fixed pose, nudge its position or add a small occluder so the golden move is a
visible, distinguishable shadow — the smoke pose is fixed, so the change is reproducible run to run.)

No new light type is needed to move the golden — the existing point light becoming shadowed is the
change. A shadowed **spot** can be added too for coverage, but one shadowed punctual light is enough
to bless the move.

### The golden moves once ([tests/golden/hello_triangle_scene.png](../../tests/golden/hello_triangle_scene.png))

This is the **single golden regeneration** of the planset (Plans 02–04 all stayed byte-identical by
construction — dummy binds, no sampled punctual atlas, no shadowed light in the smoke scene). The new
punctual shadow on the receiver is a deliberate render change; regenerate via the documented path:

```sh
HT_SMOKE=/tmp/ht.ppm build/examples/hello-triangle/hello_triangle-launcher
sips -s format png /tmp/ht.ppm --out tests/golden/hello_triangle_scene.png
```

The expected change is stated: the warm point light now casts the sphere's shadow onto the plane in
the fixed smoke pose. The PPM stays the correct size (≈ 2,764,816 bytes) and the launcher exits 0.

### The example debug UI ([examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp))

In the renderer settings panel (`Veng::UI`), beside the `Shadows`/`ShadowResolution`/`CascadeCount`
controls, expose the `PunctualShadows` toggle and `PunctualShadowResolution`, and add the
`DebugView::PunctualShadows` arm to the debug-view combo (the arm landed in Plan 03). Bring the
`modeNames`/combo array fully in line with the `DebugView` enum so the new arm is selectable, the same
resync planset-20 Plan 05 did for the cascade arm.

### `CLAUDE.md` — the shadow system is general

Update the `SceneRenderer` shadow paragraph from "the directional light is the only shadowed light" to
the present fact: the directional light is shadowed by **cascaded shadow maps** and a **bounded set of
punctual lights** (`MaxShadowedPunctual`) by a **shared punctual shadow atlas** — a spot through one
perspective map, a point through six cube faces, each sampled in the deferred lighting pass with
hardware `SampleCmp` + PCF and multiplied into its contribution, each culling its casters through
`SceneBroadphase::Cull` against its own frustum. State that **set 1 is a general shadow system** — the
directional cascade atlas + the punctual atlas + the **shared** immutable comparison sampler + the
`ShadowConstants` (directional) and `PunctualShadowBlock` (punctual records) bindings — off the set-0
bindless argument buffer because a comparison sampler mistranslates there on MoltenVK. Note the
`GpuLight` shadow slot rides `Cone.zw`, `PunctualShadows`/`PunctualShadowResolution` as the knobs, and
`DebugView::PunctualShadows` as the visualizer. Note the per-light shadow cull is the prime consumer
of the delivered BVH broadphase — one tree, many queries (`N` spot frustums + `6N` cube faces). Mention
`Veng/Renderer/PunctualShadows.h` (the device-free spot/cube view math) beside `ShadowCascades.h`.
Present-tense fact — no "used to be directional-only" narrative.

### Roadmap re-cut

- **`plans/future/scene-renderer.md`** — move **shadowed punctual lights** from "the next renderer
  feature" / "still future" to **delivered** (planset-24): a bounded set of point/spot lights shadowed
  through a shared punctual atlas, each culling via the broadphase. Reconcile the existing forward-
  reference lines (the "prime new consumer — a shadow frustum per punctual light queries one tree, many
  times" note at line ~445, the "next renderer feature" at ~446–447 and ~533–534) into the delivered
  framing rather than double-adding. Re-point what sat behind it — the named next increments reading the
  delivered shadow system:
  - **Clustered/tiled light culling** — which lights are worth a tile (the lighting loop stays a
    bounded linear loop until then).
  - **Cached/static shadow maps** (re-render only on light/caster move) — the highest-value follow-on,
    since the `6N` point-cube redraw is otherwise paid every frame even for a static scene.
  - **Per-light dynamic resolution / shadow LOD** — a near light gets a larger tile than a distant
    one (every tile is uniform `PunctualShadowResolution²` today). The atlas shape makes this a
    localized change: the set-1 records already address tiles by a **baked per-tile remap matrix**, so
    the tile rect becomes variable and a packer (skyline/guillotine) replaces the fixed grid math, with
    the sample shader unchanged. It lands naturally **alongside clustered culling** — that selection is
    what decides each light's budget.
- **`plans/future/README.md`** — the area-9 / shadowed-punctual lines (~195–196, ~415): shadowed
  punctual lights **delivered** (planset-24); clustered/tiled light culling, cached/static shadow maps,
  and per-light dynamic resolution / shadow LOD named next. Reconcile the broadphase-consumer note (the
  per-light shadow cull is now the *delivered* prime consumer of the BVH, not a future one).
- **`plans/README.md`** — add the planset-24 summary paragraph (shadowed punctual lights) in the same
  form as the others, and flip planset-24 to **done**. Adjust any cross-reference that calls
  shadowed-punctual a future feature.
- Finalize the planset-24 status table (all plans `done`).

## Decisions

1. **`PunctualShadows` recompiles; `MaxShadowedPunctual` is a constant.** The toggle inserts/removes
   the punctual pass + sample (a topology change → `Configure`), mirroring the `Shadows` directional
   toggle; the budget sizes a fixed resource, so it is a compile-time constant like `MaxLights`. The
   selection of *which* lights are shadowed is per-frame; only the cap is fixed.

2. **The existing point light becomes the shadowed one — the smallest golden move.** The sample
   already has a point light over a sphere and plane; enabling punctual shadows makes it cast, which is
   the minimal change that produces a visible new shadow in the fixed smoke pose. A re-blessed image is
   acceptable here because the gpu-band occlusion assertions (Plan 04) and the `PunctualShadows` debug
   arm (Plan 03) pin the shadow's correctness independently of the golden.

3. **The golden moves exactly once, here.** Plans 01–04 stayed byte-identical (math, dummy-bound
   resources, an atlas no visible pixel sampled, no shadowed light in the smoke scene); the single
   deliberate render change is the sample gaining the shadowed light, so the golden is regenerated once
   in this plan and left unmoved on every other. This is the README's common-to-all rule.

4. **The refinements are recorded now, while the seam is fresh.** Clustered/tiled light culling,
   cached/static shadow maps, and per-light dynamic resolution / shadow LOD all drop behind the
   delivered shadow-system surface (`Sync`/`Cull` + version-gate, the set-1 baked-remap records);
   cached/static maps are flagged the highest-value (they retire the per-frame `6N` redraw for a static
   scene), and per-light resolution is the atlas's payoff over a fixed cube-array (a variable tile rect
   in the records + a packer, sample shader unchanged), landing alongside clustered culling. Roadmap
   notes, not commitments.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `PunctualShadows` toggle + `PunctualShadowResolution` setting (surface the Plan 02 sizing field). |
| `engine/src/Renderer/SceneRenderer.cpp` | Gate the punctual pass + per-light slot assignment on the toggle; resize the punctual atlas on a `PunctualShadowResolution` change (deferred `Release()` window). |
| `examples/hello-triangle/main.cpp` | The point light casts a shadow (nudge geometry if needed for a clear shadow); expose `PunctualShadows`/`PunctualShadowResolution` + the `PunctualShadows` debug arm; resync the debug-view combo. |
| `tests/golden/hello_triangle_scene.png` | Regenerated **once** (the shadowed point light's new shadow). |
| `CLAUDE.md` | The shadow system is general (directional cascades + punctual atlas); `PunctualShadows.h` mention; knobs + debug arm; the BVH's delivered prime consumer. |
| `plans/future/scene-renderer.md` | Shadowed punctual lights delivered; clustered light culling + cached/static maps named next; reconcile the forward-references. |
| `plans/future/README.md` | Same re-cut in the area-9 lines; reconcile the broadphase-consumer note. |
| `plans/README.md` | Planset-24 entry; flip to done. |
| `plans/planset-24/README.md` | Status table → all `done`. |

## Verification

- Clean build; `ctest` green across all bands; the example links and the debug panel shows the toggle
  + the new debug arm (windowed run: the shadowed point's shadow turns on/off with the toggle).
- **`smoke_golden` moves** (regenerated per the `CLAUDE.md` procedure): the warm point light casts the
  sphere's shadow onto the plane in the fixed smoke pose — the stated, expected change. The PPM is the
  correct size and the launcher exits 0.
- **Validation gate clean under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`): the
  punctual pass + sample now exercised by a real shadowed light in the smoke/gpu path carry no MoltenVK
  validation error across the `Configure` recompiles (toggling `PunctualShadows`, changing
  `PunctualShadowResolution` — no stale atlas sample after the resize, the deferred `Release()` window
  covers the old atlas).
- The docs read as present-tense fact (no historical narrative — the comment policy holds: "the
  directional light is shadowed by cascaded shadow maps **and a bounded set of punctual lights by a
  shared punctual atlas**"), and every roadmap cross-reference (planset numbers, the named follow-ons)
  resolves.
- The smoke PPM correct size + exit 0 through the launcher.
