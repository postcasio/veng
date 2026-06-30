# planset-18 — material parameter storage, domains, and the PostProcess fullscreen-material path

**Phase goal:** two things, in dependency order. First, **rework material parameter storage**
into one reflection-sized, ring-buffered per-material block — removing the fixed engine
`MaterialData` struct so a material has an arbitrary, shader-defined handle set and
per-frame-writable params. On that foundation, give a material a first-class **domain**
(`Surface` / `PostProcess`) — the property that selects its output contract, pipeline shape,
standard vertex shader, and invocation site — while the rest of the material system
(parameter schema, bindless, authoring, inspector) stays one shared mechanism. Then stand up
the **PostProcess fullscreen-material path** in `SceneRenderer` and prove it by migrating the
hardcoded tonemap stage into the first PostProcess-domain material. Takes up
[future area 13](../future/README.md#13-material-domains--shader-graph-codegen--delivered-plansets-18-38-39)'s
**prioritized first slice**; the full design overview is
[future/material-codegen.md](../future/material-codegen.md).

veng's material is hardwired two ways today. Its GPU parameters are a **fixed 16-byte engine
block** (`Renderer::MaterialData` — one albedo texture + sampler + two pad slots) parallel to
a variable authored block — so a material is forced to be a one-albedo surface, even though
libveng already patches the block purely by reflected offset (the fixed struct is read by no
one). And it has **one implicit domain**: an opaque surface whose fragment shader writes the
g-buffer (`GBufferOutput { Albedo, Normal }`, the fixed contract
[GBuffer.h](../../engine/include/Veng/Renderer/GBuffer.h) and the geometry pass agree on).
There is no domain concept — the output contract is baked into the `Material` ↔ geometry-pass
agreement, the standard surface vertex shader is hand-shipped per game
([examples/hello-triangle/assets/shaders/brick.vert.slang](../../examples/hello-triangle/assets/shaders/brick.vert.slang),
generic but example-local), and every fullscreen effect (tonemap, the debug blits) is a
hardcoded `ScenePass` over a hand-authored core fragment shader with a bespoke push-constant
block. A second domain — and a material with anything other than one albedo texture — has
nowhere to attach.

This planset first **unifies and rings the storage** (a material's fields, handles and params
alike, live in one host-mapped, N-buffered block, written safely per frame), then introduces
the **domain** as the tag that picks the I/O template, the pipeline builder, the standard
vertex shader, and the invocation site, and delivers the PostProcess domain end-to-end: a
fullscreen pass that builds its pipeline from a **material's** fragment shader, samples
upstream targets through set-0 bindless, and exposes the material's authored params as the
tunable knobs — the **exposure / tonemap-curve / color-grade** class of effect, now authored
as content rather than compiled in C++.

## Scope decisions

Five decisions fix the boundary of this work.

1. **Storage is reworked first, as the foundation.** The fixed engine `MaterialData` block is
   deleted; a material's handles and params share one reflection-sized block, ring-buffered
   so per-frame writes are cheap and frame-safe. This is a prerequisite, not a side quest:
   the PostProcess input handle and the live exposure param both need a shader-defined handle
   set and a per-frame-writable buffer. Authored params stay **buffer-backed** — no
   push-constant side channel.

2. **Two domains, `Surface` and `PostProcess`.** The standard cross-engine factoring (Unreal
   `MaterialDomain`, Unity Shader Graph targets, Godot `shader_type`). `Surface` is the
   existing implicit behavior made explicit — canonical vertex layout, the g-buffer output
   contract, drawn per-submesh by the geometry pass. `PostProcess` is the new one — the
   screenspace fullscreen-triangle vertex stage, a single final-color output, invoked by the
   post chain with upstream targets bound as bindless inputs. The shared spine (the unified
   parameter block, bindless handles, `Material::GetFields()`, the `.vmat` authoring, the
   editor inspector) is **one system** across both — the domain only selects the four things
   that differ.

3. **Standard engine vertex shaders, one per domain.** A domain implies its vertex stage, so
   the engine **ships it in the core pack** rather than every game hand-writing one.
   `PostProcess` reuses the existing core
   [fullscreen.vert](../../engine/assets/core/shaders/fullscreen.vert.shader.json) (already
   the VS of the engine's internal fullscreen passes). `Surface`'s generic deferred vertex
   stage — today the example's `brick.vert`, which is entirely generic (canonical-layout in,
   MVP transform, normal-matrix to world space, UV + world normal out; nothing
   brick-specific) — is **promoted into the core pack** as `surface.vert`, and the example
   material references the engine asset. A game stops shipping a vertex shader to put an
   opaque mesh on screen.

4. **The PostProcess pipeline path is a material pipeline, not a new shader path.** The
   fullscreen material pass builds its `GraphicsPipeline` from the PostProcess material's
   cooked fragment shader exactly as a `Material` builds its surface pipeline — same reflected
   layout, same set-0 bindless reservation, same unified parameter block. The only novelty is
   the pipeline *shape* (fullscreen triangle, one color target, no vertex inputs) and the
   *invocation* (a `ScenePass` in the post chain). No new runtime asset path: a PostProcess
   material cooks, loads, and binds like any material.

5. **Fixed plumbing composites stay hardcoded engine passes.** The line
   [material-codegen.md](../future/material-codegen.md) draws survives this planset: the
   swapchain composite (`SwapChainCompositePass`) and the debug-view blits have no authorable
   surface and are **not** content. A PostProcess material is for *tunable effects with
   exposed parameters* (exposure, grade); plumbing is C++. Tonemap crosses the line — it has
   an authorable curve/exposure — so it becomes the first PostProcess material; the blits do
   not.

## What stays out (named follow-on: codegen)

This planset delivers the **storage rework** and **domains**, the prioritized half of area
13. It does **not** deliver **node→Slang codegen** — nodes remain expression *routers* binding
to a hand-authored fragment shader, not expression *emitters* generating one. The one
codegen-shaped step it does take is in the editor (plan 06): the node catalog becomes
**domain-aware**, and `MaterialOutput`'s pins are driven by the **domain's output contract**
rather than only mirroring a loaded shader's `GetFields()`. This is the foundational inversion
the codegen design calls for — "each domain has a different set of output pins" — landed here
as the editor manifestation of the domain concept. The deeper reshape (every node an emitter,
`Param` gaining const-vs-exposed, compile's target becoming generated Slang) is the named
follow-on planset.

## The domain, end to end

| Domain | Vertex shader (core) | Vertex layout | Outputs | Pipeline shape | Invoked by |
|---|---|---|---|---|---|
| `Surface` | `surface.vert` | canonical | g-buffer MRT (`GBufferOutput`) | mesh pipeline, depth-tested | geometry pass, per submesh |
| `PostProcess` | `fullscreen.vert` | screenspace | single color (`SV_Target0`) | fullscreen triangle, one target | a post-chain `ScenePass` |

`.vmat.json` gains one **lowercase** `"domain"` key (default `"surface"`, so every existing
material is unchanged). The `fields` array is unaffected by the storage rework — handles and
params are authored exactly as today; they simply share one block underneath:

```jsonc
// brick.vmat.json — Surface (explicit), vertex shader now the engine asset
{
  "domain": "surface",
  "shaders": { "vertex": <core surface.vert id>, "fragment": 1005 },
  "fields": [
    { "name": "Albedo",        "type": "texture", "id": 1001 },
    { "name": "AlbedoSampler", "type": "sampler", "texture": "Albedo" },
    { "name": "Factors",       "type": "vec4",    "value": [1.0, 1.0, 1.0, 1.0] }
  ]
}

// tonemap.vmat.json — the first PostProcess material (core pack)
{
  "domain": "postprocess",
  "shaders": { "vertex": <core fullscreen.vert id>, "fragment": <core tonemap.frag id> },
  "fields": [
    { "name": "Hdr",        "type": "texture" },
    { "name": "HdrSampler", "type": "sampler", "texture": "Hdr" },
    { "name": "Exposure",   "type": "float", "value": 1.0 }
  ]
}
```

The `Hdr` texture field carries no authored `id` — its bindless index is **runtime-bound**,
written by the PostProcess pass each frame (plan 04). The unified block (plan 00) makes a
handle field with no cooked asset a first-class case; the ring buffer (plan 01) makes the
per-frame rewrite safe.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | [Unified material parameter storage](00-material-param-storage.md) | Delete the fixed engine `MaterialData` block; a material's handles + params share one reflection-sized block (set 0 binding 4), patched by offset+kind. `CookedMaterialHeader` gains `Version`, drops the two-block split. Arbitrary handle count, shader-defined. | done |
| 01 | [Ring-buffered material parameter writes](01-ring-buffered-material-params.md) | N-buffer the material block buffer host-mapped; per-frame `SetParam`/`SetTexture` writes the current frame's region directly (dirty-flush over the in-flight window, current region selected by folding the frame base into the pushed material index) — no `UploadSync`, no hazard. Params stay buffer-backed. | done |
| 02 | [Material domain concept](02-material-domain.md) | The `MaterialDomain` enum (`Surface`/`PostProcess`), the lowercase `"domain"` `.vmat.json` key (default `surface`), `CookedMaterialHeader.Domain` (version bump), cook-time validation that the fragment shader's outputs match the domain's contract, and `Material::GetDomain()`. | done |
| 03 | [Standard engine vertex shaders](03-standard-vertex-shaders.md) | Promote the generic surface vertex stage (today the example's `brick.vert`) + the engine-standard `material.slang` declarations into the **core pack** as `surface.vert`; confirm `fullscreen.vert` as the PostProcess standard. Migrate `brick.vmat` to the core `surface.vert` asset; delete the example's `brick.vert.{slang,shader.json}` and its pack entry. | done |
| 04 | [PostProcess fullscreen-material pipeline path](04-postprocess-pipeline-path.md) | A `PostProcessScenePass` that builds a `GraphicsPipeline` from a PostProcess **material's** fragment shader against one color target (format supplied by the renderer), binds set-0 bindless, samples an upstream target via a runtime-bound handle field, and drives the material's authored params. A domain-keyed selector-push offset. The reusable runtime mechanism, exercised by a test-only identity material. | done |
| 05 | [Tonemap as a PostProcess material](05-tonemap-as-material.md) | Replace the hardcoded `TonemapScenePass` + `tonemap.frag` push-constant path with the first PostProcess material (HDR as a runtime-bound handle field, `Exposure` as an exposed param written per frame via the ring buffer). The geometry/lighting spine and `DebugView` blits are untouched; tonemap proves the path end to end. | done |
| 06 | [Domain-aware node catalog](06-domain-aware-node-catalog.md) | Make `RegisterMaterialNodeTypes` take the domain; `MaterialOutput`'s pins follow the **domain's output contract** (Color for PostProcess) rather than only mirroring `GetFields()`. `MaterialCompile` emits the `"domain"` key and seeds the domain-correct output node. `MaterialEditorPanel` reads the loaded material's domain. The foundational inversion toward codegen. | done |
| 07 | [Docs + roadmap re-cut](07-docs-roadmap.md) | `CLAUDE.md` material-storage/domain paragraphs, `plans/README.md` entry, `plans/future/README.md` area-13 status (storage + domains slice done; codegen still future), the `material-codegen.md` supersede note, this status table. No code. | done |

## Dependency analysis

```
00 (unified storage) ──► 01 (ring buffer) ──┐
   │                                         ├─► 04 (postprocess path) ──► 05 (tonemap)
   ├─► 02 (domain concept) ──────────────────┘            ▲
   │        ├─► 03 (core vertex shaders)                   │
   │        └─► 06 (domain-aware catalog)                  │
   │                                                       │
   └─ (02/03 build on the unified block in the header + material.slang)
                                                  05 also needs 01 (per-frame exposure)
                                                          │
                                                          ▼
                                               07 (docs) — after all land
```

- **Plan 00** is the storage foundation: the unified block, the reworked `CookedMaterialHeader`
  (with `Version`), the registry/loader/cooker/shader changes. It moves no pixel (a storage
  change, not a shading change). Everything downstream reads the one-block model.
- **Plan 01** rings that one buffer for safe per-frame writes; it depends on 00 (one buffer to
  ring) and is required by the per-frame writers (04's input handle, 05's exposure).
- **Plan 02** adds the `domain` concept on 00's reworked header (it bumps `Version` again when
  it inserts `Domain`).
- **Plans 03, 06** depend on 02 and are **mutually independent** by file set: 03 touches
  `engine/assets/core/` + the example pack (and 00's unified block in `material.slang`), 06
  touches `editor/src/material/`. Safe to fan out once 00–02 land.
- **Plan 04** depends on 02 (domain), 01 (per-frame input handle), and 00 (runtime-bound
  handle field). **Plan 05** depends on 04 and 01 (per-frame exposure) and lands inline after.
- **Plan 07** is docs-only and lands last.

The natural order is **00 → 01 → 02 → {03, 06, (04 → 05)} → 07**, with 03/06 parallelizable
against the 04→05 chain.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify
(clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a
device is present, the smoke PPM correct size + exit 0, the `smoke_golden` capture
re-checked) → update this table → one commit per plan (`Plan NN: <summary>`, `Co-Authored-By`
trailer).

Common to all plans:

- **JSON asset keys and values are lowercase, and use the JSON vocabulary.** Every
  `.vmat.json`/`.shader.json`/`.tex.json` key is lowercase; the new `"domain"` key follows.
  Field `type` values use the importer's vocabulary (`texture`/`sampler`/`float`/`vec2`/
  `vec3`/`vec4`/`uint`) — a scalar is `"float"`, **not** the C++ alias `"f32"`. (Hardcoded
  C++ `AssetId` literals stay uppercase hex per house style — the C++ representation
  convention, not the JSON one.)
- **The default domain is `surface`.** An existing material with no `"domain"` key cooks
  exactly as today; the planset adds no migration burden to packs it does not touch.
- **The cooked format breaks; `CookedMaterialVersion` guards it.** Plans 00 and 02 both change
  `CookedMaterialHeader`; each bumps the version, and the loader rejects a stale blob loudly.
  Every material blob re-cooks under the build-wired cook — re-cook is the migration.
- **`smoke_golden` stays green.** Plans 00–03 change no rendered pixel (storage fold is a
  byte-for-byte same render; domain defaults to surface; the promoted surface vertex shader is
  byte-identical to the example's). Plan 05 swaps the tonemap *mechanism*, not its math — the
  regenerated golden, if it moves at all, moves only by floating-point pipeline-build
  differences and is regenerated per the `CLAUDE.md` procedure if a deliberate change lands.
- **New `AssetId`s use a marked placeholder during implementation**, minted with `vengc
  generate-id` in the final pass (core `surface.vert`, the core `tonemap.vmat` material).
  Hardcoded literals are uppercase hex; JSON packs are decimal.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy applies; no
  "used to be a hardcoded pass" or "the old engine block" narrative.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

A material's GPU parameters are **one reflection-sized block** — handles and authored params
together, an arbitrary shader-defined handle set, ring-buffered and host-mapped so a
per-frame `SetParam`/`SetTexture` is a direct write with no stall and no frames-in-flight
hazard. On that, `libveng` materials carry a first-class **domain**. `Surface` is the existing
opaque g-buffer path made explicit; `PostProcess` is a new fullscreen-material path in
`SceneRenderer` that builds a pipeline from a material's fragment shader, samples upstream
targets through set-0 bindless, and exposes the material's authored params as tunable knobs.
The engine ships the **standard vertex shader for each domain** in the core pack —
`surface.vert` and `fullscreen.vert` — and the example material references the engine asset
instead of hand-shipping its own vertex stage. Tonemap is the first PostProcess **material**,
authored with an exposed `exposure` param rather than compiled in C++; the authorable post
stack (grade, curve) is now expressible as content behind the same mechanism. The node
catalog is **domain-aware** — `MaterialOutput`'s pins follow the domain's output contract —
the foundational inversion toward node→Slang codegen, which remains the named follow-on
planset.
