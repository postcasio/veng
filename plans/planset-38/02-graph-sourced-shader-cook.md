# Plan 02 — graph-sourced shader cook, `Param` provenance, generated `MaterialParams`

**Goal:** ship the emit walk's output through the cook. A `Param` gains a **provenance**
(const / exposed / engine-bound); the emit walk generates the per-material **`MaterialParams`**
struct (ordered large-alignment-first) and the `.vmat` **field list** together (so they agree by
construction); and the cooker gains a **graph-sourced fragment-shader importer** that resolves a
shader's graph source, runs the emit walk through the shared `veng::graph` lib, and compiles the
result through the existing `ShaderImporter`. The editor's cook-on-demand routes through the same
path → hot-reload → `MaterialPreview`. This closes the graph→cook→reload→preview loop end to end with
**no checked-in generated file, no new asset, and no derived id**. Depends on **Plan 01** (the emit
walk in the shared lib) and **Plan 00** (the importable header).

## Why it is its own plan

Plan 01 makes Slang text in the shared lib; this plan makes that text a cooked, rendering material.
The two halves it adds — the generated `MaterialParams`/field-list (so the cooker's offset-patching
has a struct to reflect) and the graph-sourced shader cook — are what turn "we can generate a shader"
into "the graph drives the rendered material." Keeping it distinct from the emit walk isolates the
cook-loop integration (the riskier part) from the pure codegen core.

## The model — the shader's source becomes a graph

A fragment-shader asset a material references **already exists** with its own `AssetId` and manifest
row (hello-triangle's `brick` material references shader `1005`). Codegen does **not** mint a new
shader: it changes that shader entry's *uncooked source* from a `.slang` file to a **graph**. The
manifest row, the id, and the material's `shaders.fragment` reference are untouched.

- A graph-sourced shader entry names a graph rather than a `.slang` (e.g.
  `brick.frag.shader.json` → `{ "source": "brick.frag.graph.json", "entry": "fsMain" }`, where the
  graph file holds the authored node graph). The graph is the **single source of truth** and the
  only checked-in artifact; the generated Slang and SPIR-V are cook output.
- The cooker's shader importer, seeing a graph source, resolves it, runs `CompileMaterialGraph`
  (the shared `veng::graph` walk) → Slang text → the existing SPIR-V compile + reflection. A
  hand-authored shader (a `.slang` source) compiles exactly as today. The cooker can dump the
  generated Slang to the build dir for debugging.
- This keeps the cooker's one-entry → one-blob loop intact (the generated shader is its own entry),
  keeps the material→shader-by-id resolution and the runtime untouched, and lets two materials share
  one generated shader later (the *pure-shader editing* future item) because the shader is a
  first-class addressable asset.

## What lands

- **`Param` gains a provenance** (a node property — an enum on the `Param` POD, schema-tolerant on
  read):
  - **const** — emits its value inline as a Slang literal (`float4(0.8,0.2,0.1,1)`); no uniform.
  - **exposed** — contributes a field to the generated `MaterialParams`, emits a `p.<Name>` read,
    carries an authored default, and **is** the surface a `MaterialInstance` may override (Plan 05).
  - **engine-bound** — contributes a `MaterialParams` field and emits `p.<Name>`, but carries **no
    authored default** and is **not** an instance override surface: the engine writes it by name at
    runtime (`SetParam`/`SetTextureHandle`). This is the category `tonemap`'s `RenderScale` (a
    `vec4` the renderer writes each frame) and `Hdr`/`HdrSampler` (handle slots the post pass rebinds)
    need; a `TextureSample` whose handle is engine-bound rather than a fixed `AssetId` is the texture
    form.

- **The generated `MaterialParams` struct + the `.vmat` field list, produced together, ordered
  large-alignment-first.** The emit walk collects, in one pass: each `TextureSample`'s handle slot
  (`uint <Name>; uint <Name>Sampler;`) and each exposed/engine-bound `Param`'s field
  (`float`/`vec2..4 <Name>;`) → the generated struct, **emitted in descending alignment order**
  (vec4 → vec3 → vec2 → scalar/uint) so the cooker's std140 reflection and the shader's scalar-layout
  `Load<MaterialParams>` resolve **identical offsets** — the layout trap `tonemap.frag.slang` is
  hand-ordered to avoid, now enforced by the generator. The matching **field list** (texture
  `AssetId`s + exposed-param defaults) is produced from the same walk, so the cooker's reflected
  offsets and the material's packed values **agree by construction**. `WriteMaterialVmat`/
  `CompiledField` are reworked to emit *this* generated field list rather than one matched against a
  loaded shader.

- **The graph-sourced shader importer.** In `ShaderImporter` (or a thin graph branch of it): a shader
  entry whose source is a graph is cooked by resolving the graph, running `CompileMaterialGraph`,
  prefixing `#include "Veng/material.slang"` + the generated `MaterialParams`, and feeding the text to
  the existing Slang compile + reflection. `RecordDependency` records the graph file (and the engine
  header) so incremental cooks track it.

- **The cook-on-demand loop drives generated source.** `MaterialEditorPanel`'s edit → 300ms-debounced
  `RequestCook` now cooks the graph-sourced shader (the same importer path, in-process) **and** the
  material that references it, mounts the result via `MountMemory`, and hot-reloads behind the stable
  `AssetHandle`. A value-only edit (an exposed param's default) recooks and re-packs. A
  **layout-changing edit** (adding/removing an exposed param, retyping one) recompiles the shader, so
  the reflected `MaterialField` table and block size change — the loader rebuilds the material's block
  from the new reflection on reload; any in-flight `SetParam` targeting a vanished field name is a
  no-op (the existing name-keyed dispatch already tolerates an unknown name). `MaterialPreview`
  re-fetches and renders the regenerated material.

- **The cooker stays format-stable.** It produces a normal cooked shader blob (SPIR-V + reflection)
  and a normal cooked material blob referencing it by id. An offline `vengc cook` with no editor
  running cooks the checked-in graph exactly the same way (the emit walk is in `veng::graph`, which
  the cooker links). **No cooked-material format change and no runtime change** — the material resolves
  its fragment by id and loads plain SPIR-V + reflection.

## Decisions

1. **A `Param` has one of three provenances.** const folds inline, exposed is an author-tweakable
   uniform with a default, engine-bound is a uniform the engine writes by name (no default, not an
   instance override). One node with a flag, not three node types; the generated `MaterialParams` is
   exactly the exposed + engine-bound set, and the instance override surface (Plan 05) is exactly the
   exposed subset.
2. **The struct is ordered large-alignment-first.** Generating the struct ourselves, we own the
   layout: emitting vec4→vec3→vec2→scalar/uint makes std140 reflection and scalar `Load<T>` agree, so
   the offset-patching loader packs where the shader reads. A GPU/pixel test guards it (the
   `static_assert` that used to is gone with `MaterialParams.h`).
3. **The struct and the field list come from one walk.** Generating both together guarantees the
   cooker's reflected offsets match the packed values — without a cross-check step.
4. **The shader's source is a graph; the cooker compiles it.** No new asset, no minted/derived id, no
   checked-in generated file — only the existing shader's source changes from `.slang` to a graph. The
   emit walk runs in `veng::graph` (cooker-linked), so the editor preview and an offline cook are
   identical. The authored graph is the single source of truth.
5. **Generation runs at cook time, in shared code.** The cooker (and the editor's in-process cook)
   both call the same `CompileMaterialGraph`. There is no editor-writes / cooker-compiles split to
   diverge.

## Files

| File | Change |
|---|---|
| `MaterialCatalog.h` / `.cpp` (`veng::graph`) | Add the provenance enum to `Param`; its emit-fn branches const-literal / exposed `p.<Name>` / engine-bound `p.<Name>`; collect the generated-struct field set with provenance. |
| `MaterialCompile.h` / `.cpp` (`veng::graph`) | Produce the generated `MaterialParams` struct text (large-alignment-first ordering) + the `.vmat` field list from one walk; rework `WriteMaterialVmat`/`CompiledField` to emit the generated list. |
| `cooker/src/Importers/ShaderImporter.cpp` | A graph-sourced branch: resolve the graph, run `CompileMaterialGraph`, compile the text; `RecordDependency` the graph + engine header. |
| `editor/src/panels/MaterialEditorPanel.cpp` | Route edit → in-process graph-shader cook + material cook → `MountMemory` hot-reload (incl. layout-changing reload) → `MaterialPreview` re-fetch. |
| `editor/src/material/MaterialShaderInterface.h` (or its `veng::graph` home) | The schema source is the generated field set; the material's field-list-vs-shader validation uses the generated shader's reflection. |
| `tests/…` (cooker/unit + a gpu pixel test) | Round-trip: a graph → emit → cook (offline) → load → assert the material binds the generated fragment and packs exposed defaults at the reflected offsets; a const param produces no field; **a scalar-before-vec graph renders the correct (non-black) pixels** (the layout-ordering guard). |
| `cooker/CLAUDE.md`, `editor/CLAUDE.md` | Document the graph-sourced shader cook: the shader's source is a graph, generated artifacts are cook output, the cooker-runs-the-walk contract. |

## Verification

- Clean build; `ctest` green. The round-trip test loads a cooked graph-material, binds the generated
  fragment, and packs the exposed defaults at the reflected offsets; the const-param case produces no
  field; the **GPU pixel test** confirms a scalar-before-vec param graph renders correctly (the
  layout-ordering invariant, per the `material_param_vec_layout` memory) — not just an offset-equality
  assert.
- The editor edit loop works end to end: editing an exposed `Param` value live-recooks and updates
  `MaterialPreview` within the debounce window; adding/removing a param (a layout change) recooks and
  reloads correctly (manual/preview verification noted).
- An **offline `vengc cook`** of a pack with a graph-sourced shader (no editor running) compiles the
  checked-in graph and produces a valid shader + material blob.
- `smoke_golden` does **not** move yet — no shipping material is migrated until Plan 04; this plan's
  fixtures are test-only.
- `include_hygiene` unaffected; validation gate clean (the generated shader binds set-0 bindless + the
  standard attachments, identical to a hand-authored material).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
