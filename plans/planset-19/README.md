# planset-19 — PBR `SceneRenderer` + über-pipeline batteries

**Phase goal:** turn `SceneRenderer`'s minimal deferred spine into a real
**physically-based** deferred renderer, then hang the first batteries off the richer
g-buffer. First **extend the g-buffer with PBR channels** (occlusion / roughness /
metallic, packed into one new target) and replace the Lambert lighting pass with a
**Cook-Torrance** BRDF reconstructing world position from depth, behind a ring-buffered
**per-frame view-constants buffer**. On that: **tangent-space normal mapping**, **multiple
typed lights** (directional / point / spot), a **directional shadow map**, **SSAO**, and
**bloom authored as a PostProcess material**. Takes up
[future area 8](../future/README.md#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries)'s
named **batteries** increment, folding in the **G2 PBR g-buffer target** the same area
reserves. Design overview: [future/scene-renderer.md](../future/scene-renderer.md).

`SceneRenderer` today renders a deliberate minimum: an MRT g-buffer of
**albedo + world-normal** ([GBuffer.h](../../engine/include/Veng/Renderer/GBuffer.h)),
a fullscreen **Lambert** N·L over a **single directional light** read from the
`SceneView`, into HDR, then tonemap to the output. There is no roughness/metallic, no
specular, no shadowing, no ambient occlusion, and no second light. The g-buffer was
built to grow — its outputs funnel through one `GBufferOutput` struct precisely so a
PBR channel set is a one-place addition — and the lighting/shadow/AO passes were all
named as future increments behind the same `ScenePass` + `Configure`-recompile
mechanism. This planset cashes those in.

The work is **spine-first**: the g-buffer layout, the BRDF, and the per-view-constants buffer
settle before any battery reads them, because each battery (shadows reconstruct world
position; SSAO reads depth+normal; the lit HDR feeds bloom) consumes the foundation Plan 01
lays.

## Scope decisions

Seven decisions fix the boundary of this work.

1. **PBR channels are a packed ORM (occlusion-roughness-metallic) target extending the
   existing `GBufferOutput`.** No new struct: `GBufferOutput` gains one member,
   `float4 ORM : SV_Target2` (R=occlusion, G=roughness, B=metallic, A=emissive strength), and
   the engine adds one `GBuffer::ORMFormat` color attachment. The geometry pass binds three
   color targets; the cooker's **Surface** output-contract check widens to require
   `SV_Target0/1/2`. This is the standard metallic-roughness g-buffer, kept to one new
   attachment for bandwidth on the tile-based primary platform.

2. **Emissive is a scalar in `ORM.a`, not a separate target.** A deferred fullscreen
   lighting pass cannot read per-material emissive (it has no material at the pixel), so
   emissive must survive *in the g-buffer*. With the packed-ORM choice, the geometry pass
   writes an emissive **strength** into `ORM.a` and the lighting pass adds
   `albedo.rgb * strength`. Independent **colored** emissive (an emissive color distinct
   from albedo) needs the separate-emissive-target layout and is a **named follow-on**, not
   this planset.

3. **Per-view data is a ring-buffered buffer, never push constants.** `InvViewProj`,
   `CameraPosition`, the directional light (Plan 01), the light-space matrix (Plan 04), and
   the SSAO view/projection (Plan 05) live in a per-frame **view-constants buffer** in the
   engine's set 0, ringed for frames-in-flight and selected by index fold — *not* a
   dynamic-offset descriptor (which mistranslates in set 0 on MoltenVK), and *not* push
   constants. Push constants in the deferred path carry only small per-invocation bindless
   handle indices. This discipline is set in Plan 01 and every later pass extends the buffer.

4. **Cook-Torrance, reconstructing world position from depth.** The lighting pass moves from
   Lambert to GGX specular + Lambert diffuse over the metallic-roughness inputs. The view
   vector and (Plan 03) the point/spot light vectors need world position, which the pass
   reconstructs from the already-sampled depth target via the camera's inverse
   view-projection — so the pass reads `InvViewProj` + `CameraPosition` from the view-constants
   buffer. A dedicated position target would cost an extra high-precision attachment's
   bandwidth; reconstruction is the standard deferred choice.

5. **Typed lights replace the single-light field.** The `Light` builtin gains a `LightType`
   (Directional / Point / Spot) + `Range` / cone fields (position from the entity's
   `Transform`); a dedicated SSBO carries a **bounded light list** the lighting pass loops
   over. **Shadowing is directional-only this planset** — point/spot shadow cubemaps/atlas
   are a named follow-on; punctual lights are unshadowed.

6. **Bloom is a PostProcess material; shadows and SSAO are plumbing.** The bloom bright-pass
   + blur + composite is authored as a **tunable PostProcess-domain material** run by the
   shipped `PostProcessScenePass` (planset-18), with `Threshold` / `Intensity` exposed params
   — the proof that the authorable post-stack mechanism scales past tonemap (and the first
   multi-stage post chain). Shadows and SSAO are **plumbing** (they read the g-buffer through
   fixed wiring, not exposed knobs), so they stay hardcoded `ScenePass` units — the same
   plumbing-vs-effect line the tonemap planset drew.

7. **On-tile / subpass-fused deferred is explicitly out — a measure-first maybe.** The richer
   g-buffer raises its bandwidth payoff on Apple Silicon, but it is a `RenderGraph`-core
   change (local-read / pass fusion + an input-attachment g-buffer binding path), gated on a
   MoltenVK `dynamic_rendering_local_read` capability check and on g-buffer round-trip being a
   **measured** bottleneck. It is its own future planset, not a battery here; Plan 07's
   roadmap re-cut records the downgrade.

## The g-buffer, before and after

| Target | Before | After | Carries |
|---|---|---|---|
| G0 `SV_Target0` | `RGBA8Srgb` | `RGBA8Srgb` | albedo (rgb, sRGB); a unused |
| G1 `SV_Target1` | `RGBA16Sfloat` | `RGBA16Sfloat` | world normal (xyz), normal-mapped (Plan 02) |
| **G2 `SV_Target2`** | — | **`RGBA8` (`ORMFormat`)** | **occlusion (R), roughness (G), metallic (B), emissive strength (A)** |
| Depth | `D32Sfloat`, sampled | `D32Sfloat`, sampled | sampled → **now world-position reconstruction** |

`GBufferOutput` grows from two members to three; `MaterialParams` grows from one albedo
handle + `Factors` to the metallic-roughness handle/factor set (base-color + ORM +
tangent-space normal map handles, base-color tint, metallic/roughness/occlusion/emissive
factors) — all within the existing 256-byte per-material stride, patched by reflected offset
exactly as today.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [PBR g-buffer + Cook-Torrance lighting](01-pbr-gbuffer-lighting.md) | Extend `GBufferOutput` with the packed `ORM` target (`SV_Target2`); add `GBuffer::ORMFormat`; widen the geometry pass to 3 color targets + the cooker Surface contract check. Rework `MaterialParams` to metallic-roughness (ORM handle, PBR factors). Introduce the ring-buffered **view-constants buffer** (`InvViewProj`/`CameraPosition`/directional light). Rewrite the lighting pass to Cook-Torrance GGX with world-position-from-depth reconstruction and scalar emissive. `Camera::GetPosition()`; `MaterialParams` `static_assert` drift guard; receiver plane in the sample scene. Migrate the example/test surface materials. | done |
| 02 | [Tangent-space normal mapping](02-normal-mapping.md) | `MaterialParams` gains the normal-map handle; `surface.vert` emits the world tangent; the surface fragment perturbs the world normal via the TBN into G1. Contained to the surface vertex/fragment — the lighting pass is unchanged. Depends only on 01; parallelizable. | done |
| 03 | [Multiple & typed lights](03-typed-lights.md) | `Light` builtin (in `Components.h`) gains `LightType` (Directional/Point/Spot) + `Range`/cone fields (position from `Transform`); new field `TypeId`s minted via `vengc generate-type-id`. A dedicated `GpuLight` SSBO (ringed, index-folded) carries a bounded light list; the lighting pass loops with distance/cone attenuation. Supersedes the single-light view-constants field. | done |
| 04 | [Directional shadow map](04-directional-shadows.md) | A `ShadowScenePass` rendering scene depth from the directional light into a shadow-map target (produced `ResourceId` + bindless handle through `PassIO`; light-space matrix into the view-constants buffer); the lighting pass samples it with **manual PCF**. Fixed-size ortho box (no bounds facility yet). `SceneRendererSettings` gains `Shadows` + `ShadowResolution`. Point/spot shadows out. | proposed |
| 05 | [SSAO](05-ssao.md) | A fullscreen `ScenePass` reading depth + normal in **view space** → an AO target the lighting pass folds into the occlusion term (combined with the baked `ORM.r`). Full-res, fixed 16-sample kernel. `SceneRendererSettings` gains `AO`. Independent of Plan 04. | done |
| 06 | [Bloom as a PostProcess material](06-bloom-postprocess.md) | A core bloom PostProcess material (bright-pass threshold + single-level separable blur) as **four chained `PostProcessScenePass` stages** with renderer-owned intermediates, composited into the HDR chain ahead of tonemap; `Threshold`/`Intensity` exposed params. The first multi-stage authorable post chain. | done |
| 07 | [DebugView channels + docs/roadmap re-cut](07-debugview-docs.md) | `DebugView` gains `Roughness`/`Metallic`/`Occlusion`/`AO`/`Shadows` arms (the recompile-topology proof, extended). Update `GBuffer.h`, `scene-renderer.md`, `CLAUDE.md`'s renderer paragraphs, the `plans/README.md` entry, the area-8 roadmap status (batteries + PBR g-buffer delivered; on-tile downgraded to measure-first; scene AABB/bounds named next), and this status table. No render code beyond the debug arms. | proposed |

## Dependency analysis

```
01 (PBR g-buffer + BRDF + view-constants)
   ├──► 02 (normal mapping) ───────────────────────────────────────┐
   │                                                                │
   ├──► 03 (typed lights) ──► 04 (directional shadows) ────────────┤
   │                      └──► 05 (SSAO) ──────────────────────────┼──► 07 (DebugView + docs)
   │                                                                │
   └──► 06 (bloom PostProcess material) ────────────────────────────┘
```

- **Plan 01** is the spine: the g-buffer layout, the metallic-roughness material block, the
  view-constants buffer, and the Cook-Torrance pass. Everything downstream reads the PBR
  channels, the view-constants buffer, and/or the reconstructed world position it adds.
- **Plan 02** (normal mapping) depends only on 01's material block + surface shaders; it is
  contained to the surface vertex/fragment and independent of every battery — parallelizable.
- **Plan 03** reshapes the lighting pass from one directional light to a typed light loop; it
  depends on 01 (the BRDF the loop evaluates) and is the pass shape 04/05 read back into.
- **Plans 04, 05** depend on 03 (the lighting pass they feed) and are **mutually independent**
  by file set — 04 adds a depth-only pass + a manual-PCF sample, 05 adds a fullscreen AO pass
  + an occlusion multiply. Safe to fan out once 03 lands.
- **Plan 06** depends only on 01 (a lit HDR image worth blooming) and the shipped
  `PostProcessScenePass`; it is independent of 02–05 and can land in parallel.
- **Plan 07** is the DebugView arms (each needs its channel to exist) + docs, last.

The natural order is **01 → 03 → {04, 05} → 07**, with **02** and **06** parallelizable
against the whole 03→{04,05} chain, all converging at 07.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify
(clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a
device is present, the smoke PPM correct size + exit 0, the `smoke_golden` capture
re-checked) → update this table → one commit per plan (`Plan NN: <summary>`,
`Co-Authored-By` trailer).

Common to all plans:

- **`smoke_golden` moves deliberately, and is regenerated per the `CLAUDE.md` procedure.**
  Plans 01–06 each change the rendered image (PBR shading, normal detail, a second light,
  shadows, AO, bloom). Each plan that moves the capture regenerates
  `tests/golden/hello_triangle_scene.png` in the same pass and states the visual change it
  expects — a moved golden with no intended change is a regression, not a rubber stamp. The
  golden's confidence rests on the sample scene having the geometry to show each effect:
  **Plan 01 adds a receiver plane** beneath the sphere so the shadow (04) and contact-AO (05)
  goldens are meaningful, and each of 04/05/06 carries a **dedicated `gpu` property assertion**
  (a caster darkens a receiver, a concave region darkens, a bright region blooms) so the
  feature is pinned by a property, not only a re-blessed image. Plan 01 likewise carries a
  BRDF property assertion (specular response) since it is the riskiest single golden move.
- **The cooked material format may break; `CookedMaterialVersion` guards it.** Plan 01 reworks
  `MaterialParams`/`GBufferOutput` and Plan 02 appends the normal-map handle; each block-layout
  change bumps the version and the loader rejects a stale blob loudly. Every material blob
  re-cooks under the build-wired cook — re-cook is the migration.
- **`material_data.slang` drift is guarded.** The example and `gpu`/cooker fixtures each carry
  a byte-identical copy of the core `material.slang` `GBufferOutput`/`MaterialParams`
  declarations. Plan 01 adds a `static_assert` unit guard on the engine-side `MaterialParams`
  `sizeof`/`offsetof`, so a divergence is a build error, not a silent misread; every copy is
  still mirrored in the same pass as the core change (the named copies are
  `examples/hello-triangle/assets/shaders/`, `tests/gpu/assets/shaders/`,
  `tests/cooker/fixtures/shaders/`).
- **New `AssetId`s use a marked placeholder during implementation**, minted with `vengc
  generate-id` in the final pass (new core shaders: shadow depth, SSAO, the bloom material +
  its shaders). Hardcoded C++ literals are uppercase hex; JSON packs are decimal. New builtin
  field `TypeId`s (Plan 03) are minted with `vengc generate-type-id`.
- **JSON asset keys/values are lowercase and use the importer vocabulary** (`"float"`, not
  `"f32"`); the new material fields and the bloom material follow.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy applies; no
  "used to be Lambert" or "the old single-light path" narrative.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

`SceneRenderer` is a physically-based deferred renderer. Materials are metallic-roughness
(base color + occlusion/roughness/metallic + tangent-space normal map, with PBR factors),
writing a three-target g-buffer through the one extended `GBufferOutput`. Per-view data rides
a ring-buffered view-constants buffer; no per-view data is pushed. The lighting pass evaluates
a Cook-Torrance BRDF over **multiple typed lights** (directional / point / spot),
reconstructing world position from depth, with a **directional shadow map** (manual PCF),
**SSAO** folded into the ambient/occlusion term, and scalar emissive. **Bloom** is a tunable
PostProcess **material** in the HDR chain, proving the authorable post stack past tonemap as
the first multi-stage chain. Every battery is a `SceneRendererSettings` toggle driving the
`Configure` recompile, and a `DebugView` arm visualizes each new channel. The remaining
renderer increments — a transparent/forward pass (a second material contract), shadowed
punctual lights, colored emissive (a separate g-buffer target), scene/mesh AABB+bounds (the
gate on a tight shadow fit and CSM), and on-tile/subpass-fused deferred (a measure-first
`RenderGraph`-core change) — stay future, each behind the same mechanism.
