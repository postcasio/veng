# planset-24 — shadowed punctual lights

**Phase goal:** extend the shadow system from **directional-only** to **point and
spot** — the other named increment behind the delivered `AABB`/`Frustum`/gather/BVH
facility. Today the directional light is the only shadowed light: set 1 is hardwired
as "the whole **directional**-shadow system" (a cascaded depth atlas + an immutable
comparison sampler + a `ShadowConstants` block,
[shadow.slang](../../engine/assets/core/shaders/shadow.slang)), and the typed
point/spot lights the lighting pass already loops over
([SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp), `GpuLight`) cast
no shadow. This planset gives a **bounded set** of punctual lights real shadows: a
**spot** light renders a single perspective shadow map (one frustum); a **point**
light renders a **cube** (six faces) into a shadow atlas; the deferred lighting pass
samples each shadowed light's map (projective for spot, cube-direction for point)
and multiplies its contribution by the visibility, exactly as the directional term
does. Each shadow view culls its casters with the **delivered `SceneBroadphase`** —
the many-shadow-views workload (a spot frustum, or six cube-face frustums per point
light) that most exercises planset-23's BVH: **one tree, queried once per view**.

This is a feature on the scale of [planset-20](../planset-20/README.md) (which stood
up the directional CSM): it **generalizes set 1** from "the directional system" to "a
shadow system" carrying directional cascades **and** a punctual shadow atlas with
per-light records, adds the punctual shadow-view math and render passes, and threads
the per-light shadow samples into the lighting loop. It is **foundation-first**: the
punctual shadow-view math (Plan 01) is pure and device-free, unit-tested before any
pass renders a depth map.

> **Status:** scoped — this README fixes the plan shape and order, and the per-plan
> files (`01-…md` … `05-…md`) are drafted (each `proposed`, awaiting review).

## Dependencies

[planset-23](../planset-23/README.md) (the BVH broadphase) is **done**: the renderer
owns a `SceneBroadphase` whose tree is rebuilt only when the scene's spatial version
moves and queried per view via `Cull(frustum, out)`, and the shadow-cascade pass
already descends it once per cascade with each cascade's light frustum. This planset's
per-light shadow views are **new consumers of that delivered tree** — each shadow view
is one more `Cull` against the same broadphase, the "one tree, many queries" workload
planset-23 was built to serve and which it names as its first new consumer. No ordering
question remains.

## Scope decisions

Eight decisions fix the boundary of this work.

1. **A bounded shadowed-punctual budget, like `MaxLights`.** Only the first `N`
   shadow-casting point/spot lights (a `MaxShadowedPunctual` constant, small — e.g. 4)
   get a shadow map; the rest light unshadowed, as all punctual lights do today. This
   keeps the punctual shadow atlas a fixed-size resource and the lighting loop a
   bounded sample set. Which lights are chosen is a per-frame selection (the renderer
   already selects/packs lights each `Execute`); the budget is a recompile knob. The
   worst case the budget sizes is `N` point lights = `6N` depth tiles and `6N`
   caster-set redraws per frame (24 at `N = 4`), **on top of** the directional
   cascades and the g-buffer pass — which is exactly why the per-light caster cull
   (decision 5) matters and why the budget stays small.

2. **Spot = one perspective shadow map; point = a six-face cube.** A spot light's cone
   maps to a single perspective frustum (fovy from the cone angle, aspect 1, range =
   the light's `Range`) → one depth tile, sampled projectively like a cascade tile. A
   point light is omnidirectional → six 90°-fovy faces into a **cube** (or six atlas
   tiles), sampled by the light→fragment direction. On MoltenVK each cube face is its
   own render (per-face viewport / layer), the **same** finding planset-20 recorded for
   the cascade atlas — MoltenVK implements multiview by instance multiplication, not
   geometry amplification — so six faces = six draws of the caster set, which is exactly
   why the per-light caster cull (decision 5) matters.

3. **A punctual shadow atlas alongside the directional cascade atlas — set 1
   generalizes.** Set 1 grows from "the directional system" to "the shadow system": it
   keeps the directional cascade atlas + comparison sampler + `ShadowConstants`, and
   **adds** a punctual shadow atlas (a 2D atlas tiling the spot maps and the point
   cube faces, or a cube/cube-array — chosen at execution time on packing and sampling
   grounds) plus a per-light shadow **record** array (each shadowed light's
   view-proj(s), atlas tile rect(s), and type). The punctual comparison sampler stays
   **in set 1, off the set-0 bindless argument buffer** — the same placement the
   directional sampler uses, because a comparison sampler mistranslates inside the
   Metal argument buffer on MoltenVK; a plain set-1 descriptor (including a cube-array
   one) is unaffected. The per-light record array is likely a **separate set-1
   binding**, not a growth of the existing `ShadowConstants` block: that block is a
   std140 dynamic uniform with a hard-asserted 288-byte size and a byte-identical CPU
   mirror ([SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp),
   `ShadowConstantsBlock`), so the directional block's layout (and its `static_assert`)
   is preserved by adding a binding rather than reshaping it; whether the record array
   is a dynamic uniform or an SSBO is an execution-time call on its size. The
   directional path is unchanged in behavior; the set is the thing that generalizes.

4. **The lighting pass samples per-shadowed-light, gated by the light's shadow
   slot.** A `GpuLight` gains a shadow **slot** (an index into the per-light record
   array, or `-1` for unshadowed) — it rides the existing `Cone.zw` padding so
   `LightStride` and its `static_assert` are unchanged. The punctual loop, after
   computing a light's contribution, multiplies by its shadow visibility: a projective
   `SampleCmp` of its spot tile, or a cube-direction `SampleCmp` of its six faces, with
   PCF + a per-light bias — mirroring `DirectionalShadowVisibility`. An unshadowed light
   (slot `-1`) skips the sample and is fully lit, so the cost scales with the shadowed
   budget, not the light count.

5. **Each shadow view culls its casters through the broadphase, against its own
   frustum.** Like the existing per-cascade cull (now served by `SceneBroadphase`): a
   spot light calls `SceneBroadphase::Cull` with its single light frustum; a point
   light calls it once per cube face with that face's frustum. The punctual passes
   reuse the **single per-`Execute` `Sync`** already wired for the camera + cascade
   passes — they query the same synced tree, they do not re-sync it. Off-screen casters
   that shadow into the light's volume are kept (the cull is the *light's* frustum, not
   the camera's), and casters outside the light's range/cone are dropped. This is the
   consumer that most exercises planset-23's delivered BVH: `N` (or `6N`) frustum
   queries against one tree per frame is the many-view workload it was built to make
   affordable.

6. **One shadow render stage contributes the punctual passes; the directional pass is
   unchanged.** The punctual shadow maps render in depth-only passes (reusing the
   `ShadowScenePass` machinery: depth pipeline, per-view/per-face viewport, the cull).
   Whether that is one pass with many viewports or a pass per light is an
   execution-time call; the directional cascade pass stays as planset-20 built it.

7. **The sample scene grows a shadowed spot (and/or point) light; the golden moves
   once.** Unlike planset-23, this planset **changes the image** — a shadowed spot/point
   light casts new shadows. The `smoke_golden` is **regenerated once** (the documented
   `HT_SMOKE` → `sips` path) as a deliberate render change, and the validation gate runs
   under `VE_DEBUG` after the render passes (Plan 03) and the sampling (Plan 04)
   specifically, since both add descriptor + render-pass infrastructure MoltenVK
   validation can flag. A `DebugView` arm visualizes a selected punctual shadow map (like
   the directional `Shadows`/`Cascades` arms).

8. **Out of scope:** contact/ray-traced shadows, per-light dynamic resolution /
   shadow LOD, cached/static shadow maps (re-render only on light or caster move),
   clustered/tiled light culling (the lighting loop stays a bounded linear loop), and
   colored/translucent shadows. **Cached/static maps are the highest-value of these
   follow-ons:** without them the `6N` point-cube redraw is paid every frame even for a
   scene whose lights and casters never move. Each is a named follow-on reading the same
   shadow-system surface.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | Punctual shadow-view math | New `Veng/Renderer/PunctualShadows.h` (glm-only, beside `ShadowCascades.h`): spot light → one perspective view-proj from cone angle + range; point light → six cube-face view-projs. Pure, device-free, unit-tested (face directions, frustum coverage, range/cone fit) with no ICD. Within `include_hygiene`. | proposed |
| 02 | Shadow-system / set-1 generalization | Set 1 grows from the directional system to a shadow system: add the punctual shadow atlas (resource + format) and its comparison sampler (in set 1, off bindless), the per-light shadow-record array as a separate set-1 binding (preserving the 288-byte `ShadowConstantsBlock` layout + its `static_assert`), and a `GpuLight` shadow slot riding `Cone.zw`. Descriptor-layout + resource work; directional behavior unchanged. | proposed |
| 03 | Punctual shadow render passes | Depth-only render of each budgeted shadowed light (spot: one view; point: six faces) into the punctual atlas, reusing `ShadowScenePass`; each view/face culls via `SceneBroadphase::Cull` against its own frustum, reusing the per-`Execute` `Sync` (decision 5). Validation gate. | proposed |
| 04 | Lighting integration | The deferred lighting punctual loop samples each shadowed light's map (projective spot / cube point) with PCF + per-light bias, gated by the light's shadow slot; a `DebugView` arm for a selected punctual map. Validation gate. | proposed |
| 05 | Budget/settings + sample + docs/roadmap | `MaxShadowedPunctual` budget + a `PunctualShadows` toggle (recompile knobs); a shadowed spot/point light in the sample scene; **golden regenerated once**; validation gate; roadmap re-cut (move shadowed-punctual to delivered in `future/README.md` and reconcile the broadphase-consumer note across planset-23/future; clustered light culling + cached/static shadow maps named next). | proposed |

## Dependency analysis

External precondition: [planset-23](../planset-23/README.md) (BVH broadphase) is
**done**; Plan 03's per-view culls query its `SceneBroadphase`.

```
01 (punctual shadow-view math, pure)
   └──► 02 (set-1 generalization) ──► 03 (shadow render passes) ──► 04 (lighting) ──► 05 (settings + sample + docs)
```

Foundation-first and serial: the math (01) is device-free and lands its value in unit
tests; the descriptor/resource generalization (02) is the prerequisite for both the
render passes (03) and the sampling (04); 05 exposes the surface, regenerates the
golden, and re-cuts the roadmap.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass →
verify (clean build, `ctest` green across the unit/death/cooker bands and the `gpu`
band where a device is present, the smoke PPM correct size + exit 0) → update this
table → one commit per plan (`Plan NN: <summary>`, `Co-Authored-By` trailer).

Common to all plans:

- **The golden moves exactly once, in Plan 05.** A shadowed punctual light is a
  deliberate render change; regenerate `tests/golden/hello_triangle_scene.png` via the
  documented `HT_SMOKE` → `sips` path when the sample scene gains the light, and leave
  it unmoved on every other plan.
- **Run the validation gate under `VE_DEBUG`** (`ctest --test-dir build-debug -L
  validation`): Plans 03 and 04 add new descriptor sets, a comparison sampler, and
  render passes, so pin the gate after each.
- **The punctual shadow-view math is a pure device-free unit** (Plan 01): face
  directions, frustum coverage, and range/cone fit are tested with no ICD on the primary
  dev path, the way `ComputeCascades` is.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy
  applies; "the directional light is shadowed by cascaded shadow maps" becomes "the
  directional light is shadowed by cascaded shadow maps and a bounded set of punctual
  lights by a shared punctual atlas," with no historical narrative.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

A bounded set of point and spot lights cast real shadows: a spot through one
perspective map, a point through a cube, each sampled in the deferred lighting pass
with hardware `SampleCmp` + PCF and multiplied into its contribution, and each culling
its casters through `SceneBroadphase::Cull` against its own frustum. Set 1 is a general
**shadow system** carrying the directional cascades and the punctual atlas with
per-light records. This per-light shadow cull is the prime new consumer of planset-23's
delivered **BVH broadphase** — the many-view "one tree, many queries" workload it was
built for; clustered/tiled light culling, cached/static shadow maps, and per-light
dynamic resolution / shadow LOD are the named next increments behind the delivered
shadow system.
