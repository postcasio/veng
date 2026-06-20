# planset-20 — scene/mesh AABB + bounds, cascaded shadow maps (CSM)

**Phase goal:** stand up the engine's first **bounds facility** — an `AABB` math
primitive, a **local-space bound per `Mesh`**, and a **world-space bound per `Scene`** —
and on it deliver **cascaded shadow maps** for the directional light: fit a cascade per
camera-frustum depth slice (fit-to-cascade, the production approach), render scene depth
into a shadow atlas, and have the lighting pass select + sample the cascade per fragment.
This cashes in the prerequisite [planset-19](../planset-19/README.md)'s directional shadow
map ([04-directional-shadows.md](../planset-19/04-directional-shadows.md) decision 3)
deferred — "scene/mesh AABB + bounds is the prerequisite; a tight shadow fit and CSM both
need it" — and takes it all the way to CSM rather than the intermediate single-map fit.

`SceneRenderer`'s directional shadow today
([SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp)) renders one shadow map
through an orthographic box of a **hardcoded** `ShadowOrthoHalfExtent = 6.0`, centered on
the world origin. One map over a fixed box has fixed texel density everywhere — it wastes
resolution on off-screen area and gives soft, low-resolution shadows near the camera where
they matter most. CSM is the standard fix: split the view frustum into depth ranges and
give each its own tightly-fit shadow map, so near cascades get high texel density and far
cascades cover distance. CSM needs to fit each cascade to the camera frustum slice — which
needs frustum math — and to extend each cascade's near plane toward the light to catch
off-screen casters — which needs a scene bound. Both are the bounds/frustum facility the
engine lacks; this planset builds it and uses it.

The work is **foundation-first**: the `AABB` primitive and the mesh/scene bounds (Plan 01)
and the pure cascade math (Plan 02) settle before any GPU resource consumes them, because
the cascade matrices are a pure function of the camera, the light, and the scene bound, and
are fully unit-testable with no device. The atlas + render (Plan 03), the lighting-pass
selection (Plan 04), and the debug/settings surface (Plan 05) build on that analytic core.

## Scope decisions

Eight decisions fix the boundary of this work.

1. **`AABB` is a glm-only math primitive in a new `Veng/Math/` home.** A min/max `vec3`
   pair with the usual union/expand/center/extents/corners/transform algebra and an empty
   sentinel (`min > max`, the `Union` identity) — Plan 01 carries the full member surface. It
   pulls in nothing but glm, so it stays inside `include_hygiene`, and it seeds the `Veng/Math/`
   area.

2. **Mesh bounds are local-space, computed from geometry — no cooked-format change.**
   `Mesh::GetBounds()` is folded from the canonical vertex positions in `MeshLoader`,
   `Primitives::*`, and runtime `Mesh::Create`. The `.vengpack` mesh format is untouched (a
   one-pass-over-vertices compute at load); storing it in `CookedMeshHeader` is a measured
   optimization, not done here.

3. **Scene bounds are world-space, recomputed on demand — no dirty-flag cache.** A
   `Veng/Scene/` `SceneBounds(scene)` unions every resident `(Transform, MeshRenderer)`
   entity's world-space mesh bound, reusing `ComputeWorldMatrices`' one amortized pass (not
   per-entity `WorldMatrix`, which re-walks the parent chain). Recompute-on-demand mirrors
   `ComputeWorldMatrices`; a spatial structure (BVH) is the scaling answer shared with
   frustum culling and explicitly deferred. CSM consumes the scene bound only for
   **near-plane extension** (off-screen casters), not for the cascade XY extent.

4. **Fit-to-cascade, not fit-to-scene.** Each cascade's ortho box is fit to the **camera
   view frustum** sliced by depth — O(1) in scene size, the approach Unreal/Unity use — not
   to the whole scene AABB (which wastes resolution on a large world). The split scheme is
   the practical PSSM blend of logarithmic and uniform splits (a `lambda` knob). The
   per-cascade fit uses a **bounding sphere** of the slice's corners (rotation-invariant
   size → no shimmer as the camera turns) and **texel-snaps** the box's light-space min (no
   shimmer as the camera translates) — the two standard CSM stabilizations.

5. **Shadows leave bindless: a dedicated descriptor set + hardware `SampleCmp`.** The
   shadow map is a **closed producer→consumer resource** — the shadow pass writes it, the
   lighting pass (and the optional `DebugView` blit) reads it, nothing else. It does **not**
   belong in the global set-0 bindless registry. It is bound instead in a **dedicated
   descriptor set (set 1)** on the consuming pipeline, with an **immutable comparison
   sampler** in that set's layout, so the lighting pass uses **hardware `SampleCmp`** (the
   normal, faster, smoother shadow-comparison path) instead of manual in-shader PCF. The
   MoltenVK argument-buffer mistranslation that bars a comparison sampler from set 0's
   bindless array (planset-19's shadow decision 2) does not apply outside the argument
   buffer — a plain comparison sampler in a dedicated set is standard and supported. This
   removes the manual-PCF workaround at its root. Set 1 is the home of the **whole directional-
   shadow system** — the atlas (sampled image), the immutable comparison sampler, and the
   per-frame `ShadowConstants` buffer (cascade matrices + splits + params, decision 7) — so the
   shadow state lives together, off the global registry. The shadow resource is still an
   `Import`ed, graph-declared resource (the lighting pass's `.Sample(shadow)` drives the
   graph-derived `DepthAttachment → ShaderReadOnly` barrier exactly as before) — only its
   *binding* moves off bindless; barriers are unaffected.

6. **An atlas as the render structure (a texture array is the named alternative).** With
   the comparison sampler now in a dedicated set, a depth **texture array** (layer = cascade)
   is no longer barred — but veng's `RenderGraph` builds **one `RenderingInfo` per graphics
   pass**, so an array (N distinct layer views) forces either N passes or multiview, while a
   single 2D **atlas** renders all cascades in **one** pass via per-cascade viewports — the
   structure that fits the graph. The atlas is **sized to `CascadeCount`** (a
   `min(Count,2)×ceil(Count/2)` grid of `ShadowResolution²` tiles — 1×1 / 2×1 / 2×2), so a low
   cascade count pays for no idle tiles, and the per-cascade `ShadowResolution` **default is
   1024**, making a default 4-cascade atlas a 2048² D32 — the same footprint as the prior single
   2048 map, not a 4× regression. The atlas's only cost over an array is a per-cascade tile-UV
   remap in the `SampleCmp` call (trivial), versus the array's clean per-layer sample but messier
   render. The atlas keeps rendering single-pass and is the pick; the array (with multiview as a
   future single-draw optimization) is the named alternative should the per-cascade tile-UV remap
   ever earn replacing.

7. **`MaxCascades = 4`, carried in a dedicated `ShadowConstants` buffer in set 1 — not the
   view-constants block.** The cascade matrices and splits are the directional-shadow system's
   private state (lighting pass + debug blit only), so they ride the **same dedicated set 1**
   as the atlas they describe — not the per-view block, which stays lean and material-facing.
   `ShadowConstants` is `CascadeViewProj[4]` (256 B, tile-remap baked in) + `CascadeSplits`
   (one `vec4`, the cascades' view-space far distances) + `ShadowParams` (one `vec4`: 1/tileRes
   (texel size) / blend band / cascade count / enabled) — a 288-byte **std140** block. It is a **dynamic
   uniform buffer**: because set 1 is a plain descriptor set (not the bindless argument buffer),
   the per-frame region is selected by a conventional **dynamic offset** at bind, not the
   index-fold the set-0 ring needs. This path is **net-new descriptor infrastructure** veng
   gains here — `DescriptorType::UniformBufferDynamic` + the `pDynamicOffsets` bind plumbing (Plan
   03) — a reusable per-frame-constants mechanism, not shadow-specific. The set-0 `ViewConstants`
   block keeps only its genuine per-view camera state — `InvViewProj`, `CameraPosition`, `View`,
   `Proj` — which a material and the SSAO pass legitimately read; its `LightViewProj`/`ShadowParams`
   move out to `ShadowConstants`. So set 1 is the whole directional-shadow system (atlas +
   comparison sampler + cascade constants), set 0 stays a single lean block shared by materials,
   lighting, and SSAO, and neither buffer fights a stride budget. Four cascades is the standard
   ceiling; more is a `ShadowConstants` size bump, not done here.

8. **No punctual-light shadows, no frustum culling, no per-submesh bounds.** This lands
   directional CSM and the bounds facility it needs. Shadowed point/spot lights (cubemap /
   spot atlas), frustum culling (the other prime consumer of mesh bounds), per-submesh
   bounds, and a tight-fit single-map mode are named follow-ons that read this same
   facility — out of scope.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [AABB facility + mesh/scene bounds](01-aabb-mesh-bounds.md) | New `Veng/Math/AABB.h` (glm-only). `Mesh::GetBounds()` from canonical positions (`MeshLoader`/`Primitives`/runtime `Create`). `SceneBounds(scene)` unioning resident `(Transform, MeshRenderer)` world bounds via `ComputeWorldMatrices`. Device-free unit tests for the algebra, primitive bounds, and a scene bound. | done |
| 02 | [Cascade math: frustum split + per-cascade fit](02-cascade-math.md) | A pure, device-free `ComputeCascades(camera, lightDir, sceneBounds, count, lambda, resolution)` → per-cascade view-proj + split distances. Frustum-corner extraction (inverse view-proj over the NDC cube), PSSM split, bounding-sphere fit, texel snapping, scene-bound near-plane extension. `Camera` exposes `GetNear()/GetFar()`. Heavily unit-tested (split monotonicity, snap stability, slice coverage). | done |
| 03 | [Shadow atlas + dedicated-set handoff + multi-cascade render](03-shadow-atlas.md) | `ShadowScenePass` → a cascaded atlas pass: owns a D32 atlas **sized to `CascadeCount`** (**not** bindless), renders scene depth once per cascade into its tile (per-cascade viewport + light-matrix push). Two net-new descriptor extensions (immutable samplers + **dynamic uniform buffers**); the `PassIO` **bound-view** seam delivering a producer's view into a consumer's dedicated set; **both** lighting pipeline layouts gain a **set 1** holding the whole shadow system — atlas + immutable comparison sampler + a dynamic-uniform `ShadowConstants` buffer (`CascadeViewProj[4]` + `CascadeSplits` + `ShadowParams`, `MaxCascades = 4`). `SceneRenderer::Execute` runs `ComputeCascades`, threads the raw cascade matrices to the shadow pass via `SceneView`, and writes the tile-remapped matrices + splits into `ShadowConstants`. The set-0 `ViewConstants` block is trimmed to camera/view state. | ready |
| 04 | [Lighting-pass cascade selection + `SampleCmp`](04-lighting-cascades.md) | The shared `deferred_lighting.frag.slang` body (both pipelines inherit it via `#include`) selects the cascade by the fragment's view-space depth against `CascadeSplits`, remaps to the atlas tile, and uses hardware **`SampleCmp`** (the set-1 comparison sampler) with a boundary cross-fade + per-cascade depth-bias scaling. Consolidates the three divergent set-0 `ViewConstants` declarations to one lean `view_constants.slang` (**fixing the latent SSAO 256-stride/offset bug**). Lands the **`DebugView::Cascades`** arm (with the spanning-receiver `gpu` test) that pins this plan's golden move. | ready |
| 05 | [CSM settings + debug-UI exposure](05-debugview-settings.md) | `SceneRendererSettings` gains `CascadeCount` + `CascadeSplitLambda`, and lowers the `ShadowResolution` default to 1024 (count sizes the atlas grid → recompile; lambda is a recompile-safe per-frame value). Exposes the `DebugView::Cascades` arm (landed in Plan 04) and the knobs in the example debug UI; resyncs the stale `modeNames` combo. | ready |
| 06 | [Docs + roadmap re-cut](06-docs-roadmap.md) | Document the bounds facility + CSM in `CLAUDE.md`'s renderer/scene paragraphs; mark scene/mesh AABB + CSM delivered in `future/scene-renderer.md` + `future/README.md` (shadowed punctual lights / frustum culling named next behind the facility); add the `plans/README.md` entry; finalize this table. | ready |

## Dependency analysis

```
01 (AABB + mesh/scene bounds)
   └──► 02 (cascade math) ──► 03 (atlas + dedicated-set handoff) ──► 04 (lighting select + SampleCmp) ──► 05 (debug + settings) ──► 06 (docs)
```

- **Plan 01** is the primitive: the `AABB` type, mesh bounds, and `SceneBounds`. Plan 02's
  fit consumes both.
- **Plan 02** is the analytic heart — the cascade matrices as a pure function, fully
  unit-tested with no GPU. Everything downstream consumes its output.
- **Plan 03** stands up the GPU side (atlas + multi-cascade render + the set-1 `ShadowConstants`
  buffer + trimmed view block), Plan
  04 the shader selection, Plan 05 the debug/settings surface. All serial.
- **Plan 06** is docs/roadmap, last.

The order is **01 → 02 → 03 → 04 → 05 → 06**, fully serial. Plans 01 and 02 are pure/
device-free and land their value in unit tests before any GPU work; the render-behavior
change (and the only `smoke_golden` move) is concentrated in 03+04.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify
(clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a
device is present, the smoke PPM correct size + exit 0, the `smoke_golden` capture
re-checked) → update this table → one commit per plan (`Plan NN: <summary>`,
`Co-Authored-By` trailer).

Common to all plans:

- **`smoke_golden` moves in 03–04, regenerated per the `CLAUDE.md` procedure.** Plans 01–02
  add no rendering and must leave the golden byte-identical (a move there is a bug). The
  atlas render + cascade sampling change the shadow on the receiver plane (sharper near the
  camera), and Plan 04's SSAO stride/offset fix shifts the AO term (a correction, not a
  regression) — the moved golden states both expected changes. It is pinned by a
  **CSM-distinguishing** `gpu` assertion that **lands with its arm in Plan 04** (not deferred to
  05, so the golden never moves un-pinned): not just "the caster darkens the receiver" (which a
  broken cascade also satisfies), but cascade *selection* via the `DebugView::Cascades` arm (near
  region → cascade 0, far region → a higher cascade) over a **grazing-view fixture whose visible
  receiver actually spans the splits** (the existing 8×8-plane fixture is degenerate — its whole
  surface lands in cascade 1). A re-blessed image alone is not the guard. **Run the validation
  gate under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`): the atlas's
  `RenderingInfo` (one depth attachment, per-cascade viewport), the depth-as-texture sample, the
  per-cascade draw loop, and the set-1 comparison-sampler + dynamic-offset uniform must carry the
  graph-derived transitions with no MoltenVK validation error. The dynamic-offset ring is
  additionally pinned by a **cross-frames-in-flight data-correctness test** (Plan 03), since a
  mistranslated offset is a data hazard the validation gate would not catch.
- **The bounds + cascade math is glm-only and device-free.** `AABB.h` and the
  `ComputeCascades` API are public/pure, inside `include_hygiene`, and tested with no ICD —
  the value of 01–02 lands before any GPU work.
- **New core shaders re-use placeholder `AssetId`s during implementation**, minted with
  `vengc generate-id` in the final pass (the cascaded depth pass may reuse the existing
  `shadow_depth.vert` unchanged — it already takes a light-space MVP push; if a per-cascade
  variant is needed it is minted then). Hardcoded C++ literals are uppercase hex; JSON packs
  are decimal.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy applies; no
  "used to be a single fixed box" or "the old hardcoded extent" narrative.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

The engine has an `AABB` bounds primitive (`Veng/Math/`), every `Mesh` carries a
local-space bound from its geometry, and a `Scene` reduces to a world-space bound on demand.
The directional light is shadowed by **cascaded shadow maps**: the camera frustum is split
into depth ranges, each cascade fit (bounding-sphere + texel-snapped) to its slice and
rendered into a shadow atlas bound in a **dedicated descriptor set** (off bindless), and the
lighting pass selects the cascade and **`SampleCmp`s** it (hardware comparison sampler) per
fragment with a boundary cross-fade — high texel density near the camera, range to the far
plane, stable under camera motion. The facility is general: shadowed punctual lights,
frustum culling, and editor focus-on-selection all read the same `AABB`/`SceneBounds`, and
CSM's per-cascade fit is the same math a future tight-fit single map would use — each a named
future increment behind this foundation.
