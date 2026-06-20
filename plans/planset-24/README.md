# planset-24 — shadowed punctual lights

**Phase goal:** extend the shadow system from **directional-only** to **point and
spot** — the other named increment behind the delivered `AABB`/`Frustum`/gather
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
does. Each shadow view culls its casters off the **shared visibility gather** — the
many-shadow-views workload that most rewards planset-23's cache and the future BVH.

This is a feature on the scale of [planset-20](../planset-20/README.md) (which stood
up the directional CSM): it **generalizes set 1** from "the directional system" to "a
shadow system" carrying directional cascades **and** a punctual shadow atlas with
per-light records, adds the punctual shadow-view math and render passes, and threads
the per-light shadow samples into the lighting loop. It is **foundation-first**: the
punctual shadow-view math (Plan 01) is pure and device-free, unit-tested before any
pass renders a depth map.

> **Status:** scoped (this README). Per-plan files are written when the planset is
> taken up — the plan table below fixes the shape and order; the detailed plan files
> (`01-…md` … `05-…md`) land at execution time, matching how each planset details its
> plans when it begins.

## Dependencies & ordering

This planset **builds on [planset-23](../planset-23/README.md)** but does not strictly
require it: each shadowed punctual light needs its **own** caster cull (a spot
frustum, or six cube-face frustums per point light), so N shadowed lights mean N (or
6N) culls of the candidate list each frame — exactly the workload planset-23's cache
(and the future BVH) exists to make cheap. Taking planset-23 first means these culls
ride the warm cache; taking this first leaves them on the per-frame linear scan
(correct, just colder). **Recommended order: 23 → 24.**

## Scope decisions

Eight decisions fix the boundary of this work.

1. **A bounded shadowed-punctual budget, like `MaxLights`.** Only the first `N`
   shadow-casting point/spot lights (a `MaxShadowedPunctual` constant, small — e.g. 4)
   get a shadow map; the rest light unshadowed, as all punctual lights do today. This
   keeps the punctual shadow atlas a fixed-size resource and the lighting loop a
   bounded sample set. Which lights are chosen is a per-frame selection (the renderer
   already selects/packs lights each `Execute`); the budget is a recompile knob.

2. **Spot = one perspective shadow map; point = a six-face cube.** A spot light's cone
   maps to a single perspective frustum (fovy from the cone angle, aspect 1, range =
   the light's `Range`) → one depth tile, sampled projectively like a cascade tile. A
   point light is omnidirectional → six 90°-fovy faces into a **cube** (or six atlas
   tiles), sampled by the light→fragment direction. On MoltenVK each cube face is its
   own render (per-face viewport / layer), the **same** finding planset-20 recorded for
   the cascade atlas: no geometry-amplification win, so six faces = six draws of the
   caster set — which is exactly why the per-light caster cull (decision 5) matters.

3. **A punctual shadow atlas alongside the directional cascade atlas — set 1
   generalizes.** Set 1 grows from "the directional system" to "the shadow system": it
   keeps the directional cascade atlas + comparison sampler + `ShadowConstants`, and
   **adds** a punctual shadow atlas (a 2D atlas tiling the spot maps and the point
   cube faces, or a cube-array — chosen at execution time) plus a per-light shadow
   **record** array (each shadowed light's view-proj(s), atlas tile rect(s), and
   type). `ShadowConstants` becomes a broader shadow block. The directional path is
   unchanged in behavior; the set is the thing that generalizes.

4. **The lighting pass samples per-shadowed-light, gated by the light's shadow
   index.** A `GpuLight` gains a shadow slot (an index into the per-light record array,
   or `-1` for unshadowed). The punctual loop, after computing a light's contribution,
   multiplies by its shadow visibility: a projective `SampleCmp` of its spot tile, or a
   cube-direction `SampleCmp` of its six faces, with PCF + a per-light bias — mirroring
   `DirectionalShadowVisibility`. An unshadowed light (slot `-1`) skips the sample and
   is fully lit, so the cost scales with the shadowed budget, not the light count.

5. **Each shadow view culls its casters off the shared gather, against its own
   frustum.** Like planset-21's per-cascade cull: a spot light culls the candidate
   span against its single light frustum; a point light culls each of its six faces
   against that face's frustum. Off-screen casters that shadow into the light's volume
   are kept (the cull is the *light's* frustum, not the camera's), and casters outside
   the light's range/cone are dropped. This is the consumer that turns planset-23's
   cache — and a future BVH — from a nicety into the thing that keeps N shadowed lights
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
   under `VE_DEBUG`. A `DebugView` arm visualizes a selected punctual shadow map (like
   the directional `Shadows`/`Cascades` arms).

8. **Out of scope:** contact/ray-traced shadows, per-light dynamic resolution /
   shadow LOD, cached/static shadow maps (re-render only on light or caster move),
   clustered/tiled light culling (the lighting loop stays a bounded linear loop), and
   colored/translucent shadows. Each is a named follow-on reading the same shadow-system
   surface.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | Punctual shadow-view math | New `Veng/Renderer/PunctualShadows.h` (glm-only, beside `ShadowCascades.h`): spot light → one perspective view-proj from cone angle + range; point light → six cube-face view-projs. Pure, device-free, unit-tested (face directions, frustum coverage, range/cone fit) with no ICD. Within `include_hygiene`. | proposed |
| 02 | Shadow-system / set-1 generalization | Set 1 grows from the directional system to a shadow system: add the punctual shadow atlas (resource + format), the per-light shadow-record array, and a `GpuLight` shadow slot; `ShadowConstants` → a broader shadow block. Descriptor-layout + resource work; directional behavior unchanged. | proposed |
| 03 | Punctual shadow render passes | Depth-only render of each budgeted shadowed light (spot: one view; point: six faces) into the punctual atlas, reusing `ShadowScenePass`; each view/face culls the shared gather span against its own frustum (decision 5). | proposed |
| 04 | Lighting integration | The deferred lighting punctual loop samples each shadowed light's map (projective spot / cube point) with PCF + per-light bias, gated by the light's shadow slot; a `DebugView` arm for a selected punctual map. | proposed |
| 05 | Budget/settings + sample + docs/roadmap | `MaxShadowedPunctual` budget + a `PunctualShadows` toggle (recompile knobs); a shadowed spot/point light in the sample scene; **golden regenerated once**; validation gate; roadmap re-cut (clustered light culling + cached/static shadow maps named next; the per-light cull named as the BVH's prime consumer). | proposed |

## Dependency analysis

```
01 (punctual shadow-view math, pure)
   └──► 02 (set-1 generalization) ──► 03 (shadow render passes) ──► 04 (lighting) ──► 05 (settings + sample + docs)
```

Foundation-first and serial: the math (01) is device-free and lands its value in unit
tests; the descriptor/resource generalization (02) is the prerequisite for both the
render passes (03) and the sampling (04); 05 exposes the surface, regenerates the
golden, and re-cuts the roadmap.

## On completion

A bounded set of point and spot lights cast real shadows: a spot through one
perspective map, a point through a cube, each sampled in the deferred lighting pass
with hardware `SampleCmp` + PCF and multiplied into its contribution, and each
culling its casters off the shared visibility gather against its own frustum. Set 1 is
a general **shadow system** carrying the directional cascades and the punctual atlas
with per-light records. The many-shadow-view caster cull is the workload that most
rewards [planset-23](../planset-23/README.md)'s cache and the named **BVH broadphase**;
clustered/tiled light culling and cached/static shadow maps are the named next
increments behind the delivered shadow system.
</content>
