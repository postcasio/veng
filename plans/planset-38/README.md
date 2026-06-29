# planset-38 — node→Slang material codegen (the graph generates the shader)

**Phase goal:** make the node material editor **author shading**, not wire a pre-existing
shader. Today a `.vmat` graph is a binding diagram over a hand-authored fragment shader: a
`TextureSample` picks which texture fills a reflected field, a `Param` fills a value field, and
`MaterialOutput`'s pins mirror the loaded shader's `Material::GetFields()`
([MaterialCompile.cpp](../../editor/src/material/MaterialCompile.cpp) matches feeders to fields
positionally — note its `CompileMaterialGraph` `(void)domain;`). This planset inverts that: every
node becomes an **expression emitter**, the graph is topologically walked into **generated Slang
fragment source**, and the cooker compiles that source when it cooks the fragment shader the
material already references. The hand-authored fragment shader goes away — the graph *is* the
source.

Generating the shader also draws the line every production engine draws between a **material** and a
**material instance**. A graph that generates a fragment shader *and* its exposed-param schema is a
**parent material** (Unreal's *Material*): it owns the expensive half — the permutation, the
pipeline. The exposed params it generates are, by definition, the surface a caller may tweak
*without* touching the shader — i.e. the override surface of a **material instance** (Unreal's
*Material Instance*). So codegen and instances are two halves of one idea: codegen **defines what is
tweakable**, an instance **tweaks it cheaply across many copies that share the generated shader and
pipeline**. Plans 05–06 land that split — the planset's runtime/cook/asset work, where the generated
parameter schema earns a cheap consumer.

This is [future area 13](../future/README.md#13-material-domains--shader-graph-codegen--prioritized)'s
**prioritized follow-on** (node→Slang codegen), built on the material-domains slice
(planset-18) and the node-graph surface (planset-15). It folds in
[future area 14](../future/README.md#14-engine-owned-material-shader-header--cross-pack-slang-includes--prioritized)
(engine-owned material shader header + cross-pack Slang includes) as its **precursor** (Plan 00),
because a generated fragment shader must be able to `#include` the engine's material contract —
which nothing can do today. **Design overview:** [material-codegen.md](../future/material-codegen.md).

## The shape — the graph is the AST; Slang is the compiler; the cooker runs the walk

The keystone is that the node graph **is the syntax tree** — a typed DAG with acyclicity already
enforced by the topology core and coercion already recorded on each link (planset-15) — so codegen
needs **no parsed-AST intermediate**. It needs one thin layer: a typed **`EmittedValue`** the emit
walk threads, the same code-chunk model every production shader-graph system uses (Unreal's
`FHLSLMaterialTranslator` chunk pool, Unity ShaderGraph's slot variables).

```
  graph (the AST)              EmittedValue table              Slang (the real compiler)
  ─────────────────           ──────────────────────          ─────────────────────────
  typed DAG, acyclic    ──►   topo walk from MaterialOutput   ──►  generated .slang text
  coercion on links          one SSA temp per output pin           ShaderImporter → SPIR-V
  node = emitter             { Expr; PinType; IsConst }            + offline reflection
```

veng deliberately stops there. Slang's own front-end does the parsing, type-checking, CSE,
constant folding, and dead-code elimination, then lowers to SPIR-V — so building a deeper IR would
reimplement the compiler we already pay for. Our job is narrowly **structured graph → correct Slang
text**; the topo walk from `MaterialOutput` gives dead-code elimination for free (an unreached node
never emits), and `EmittedValue` makes coercion, const-folding, and temp-vs-inline first-class
instead of string-munged.

A **material domain** (Surface or PostProcess, from planset-18) fixes the output node's sink set and
the entry-point signature; the walk is otherwise domain-agnostic.

**The graph is the fragment shader's source, and the cooker generates from it.** A fragment-shader
asset whose source is a **graph** (rather than a `.slang` file) is cooked by walking the graph into
Slang text and compiling that text through the **existing** `ShaderImporter` path — the same SPIR-V
back-end a hand-authored `.slang` takes. So the emit walk has to be reachable from the cooker: it
moves out of editor-only code into a shared library both `libveng_editor` and `libveng_cook` link
(Plan 01). The editor and an offline `vengc cook` then run the **same** walk, so editor preview and
shipped cook are identical by construction. Hand-authored shaders still cook from their `.slang`
source, unchanged.

**No new asset, no minted/derived id, no checked-in generated file.** The fragment-shader asset the
material references **already exists** with its own `AssetId` and manifest row (e.g. hello-triangle's
`brick` material references shader id `1005`). Migrating a material to codegen does **not** create a
new asset: it changes that shader's *uncooked source* from a `.slang` file to a graph, deletes the
dead `.slang`, and leaves the material's `shaders.fragment` reference untouched. The **authored**
artifact (the graph) is checked in; the **generated** artifacts (the Slang text and the SPIR-V) are
cook output, never checked in (the cooker can dump the Slang to the build dir for debugging). So
there is nothing to drift, nothing to mint, and **no cooked-material format change and no runtime
change** — the material resolves and loads its fragment by id exactly as today.

**The `.vmat` still carries a field list**, but it is now *generated from the graph* (the exposed
params' defaults + the texture nodes' `AssetId`s) rather than *matched against a loaded shader's
reflection*. Because the same emit walk produces the generated `MaterialParams` struct **and** the
material's field list, the cooker's reflected offsets and the material's packed values agree — the
invariant the cooker's offset-patching loader relies on. The generated struct orders its fields
**large-alignment-first** so the cooker's std140 reflection and the shader's scalar-layout `Load<T>`
resolve identical offsets (the layout trap `tonemap.frag.slang` is hand-ordered to avoid).

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | [Cross-pack Slang includes + the material header split (area 14)](00-cross-pack-includes-header-split.md) | A shared cooker Slang-session helper adds an engine shader-include dir to `searchPaths` (the three `searchPathCount = 1` sites), so any `.slang` can `#include "Veng/material.slang"`. `material.slang` splits: the **engine contract** stays (bindless declarations, `g_ViewConstants`, `GBufferOutput`, `DrawData`, the **domain-keyed push blocks**, `ComputeMotionVector`, and the per-domain fragment-input struct), **`MaterialParams` moves to the authoring shader** (it is per-shader by definition). All **four** vendored `material_data.slang` copies are deleted; the dependent shaders `#include` the engine header; the `Veng/Renderer/MaterialParams.h` C++ mirror and its drift-guard test are retired. Hand-authored shaders, same SPIR-V — `smoke_golden` does not move. | done |
| 01 | [The emit walk — a shared graph lib, `EmittedValue`, a schema-independent catalog](01-emit-walk-emittedvalue.md) | Relocate the node-graph topology core + the material node catalog + `CompileMaterialGraph` into a new library both the editor and the cooker link. `CompileMaterialGraph` is rewritten from feeder-matching to a **topological emit**: a per-node-type emit-fn maps input `EmittedValue`s + property bytes to an output `EmittedValue`; the walk assigns one SSA temp per output pin and substitutes downstream, applying link-recorded coercion. The catalog stops being shaped from a loaded shader's reflected fields and becomes a **fixed, schema-independent** node set. `MaterialOutput` emits the domain entry point (`GBufferOutput` for Surface, `SV_Target0` for PostProcess) with defined defaults for unconnected sinks. Output is a Slang source string. Still only the three existing node types. Depends on 00. | done |
| 02 | [Graph-sourced shader cook + const/exposed/engine-bound `Param` + generated `MaterialParams`](02-graph-sourced-shader-cook.md) | A `Param` gains a **provenance**: *const* (folds inline), *exposed* (a generated `MaterialParams` field with an authored default, instance-tweakable), or *engine-bound* (a field the engine writes by name at runtime, no authored default, not instance-tweakable). The emit walk generates the `MaterialParams` struct (large-alignment-first) + the `.vmat` field list together. The cooker gains a **graph-sourced fragment-shader importer** (resolve graph → emit via the shared lib → compile through `ShaderImporter`); the editor's cook-on-demand routes through it → hot-reload → `MaterialPreview`. The full graph→cook→reload→preview loop, with no checked-in generated file. Depends on 01. | done |
| 03 | [Expand the node catalog to real shading math](03-node-catalog-shading-math.md) | The catalog grows real emitter nodes — `Constant`, `Multiply`/`Add`/`Subtract`/`Divide`, `Lerp`/`Saturate`/`Clamp`/`OneMinus`, `Dot`/`Cross`/`Normalize`/`Length`, `Split`/`Combine`, `ScalarParam` — each a `NodeType` carrying an explicit leaf `TypeId`, an emit-fn, and a JSON round-trip. The graph becomes genuinely expressive. Depends on 02. | done |
| 04 | [Migrate the samples onto graph-sourced shaders](04-migrate-samples.md) | hello-triangle's `brick` fragment shader's source becomes a graph (delete `brick.frag.slang`, author the equivalent graph; shader id and material reference unchanged); the template's `flat` fragment is co-migrated to a trivial generated Surface graph. The catalog gains authored field **names** on `Param`/`ScalarParam`/`TextureSample` (so an engine-bound field resolves by name) plus `Min`/`Max`/`ScreenUV` nodes; the shader importer no longer records the generated module's synthetic non-existent path as a depfile dependency. `smoke_golden` regenerated once (the current Surface contract has no ORM sink / normal-map node, so the brick image drops that detail by construction). **The core `tonemap` migration is deferred:** the embedded core pack is cooked by the veng-free `veng_cook_bootstrap` (the cycle-break), which cannot link the `veng::graph` walk, so a core shader cannot be graph-sourced without a separate cycle-breaking refactor. Depends on 03. | done |
| 05 | [Material instances — runtime core](05-material-instances-runtime.md) | Split the fused `Material` into a **parent** (`Material`: shader → pipeline + the exposed-param **schema** + defaults) and a **`MaterialInstance`** (a cheap override over a parent: its own SSBO slot + texture set, no shader). The instance half — the per-material slot + ring-buffered `SetParam`/`SetTexture` + selector push — **moves** off `Material`. New `AssetType::MaterialInstance` + cooked blob + loader; a bare parent doubles as a zero-override instance; the mesh material list, the `MeshSource` shape fields, `Primitives`, the components, and the prefab/level `AssetHandle` resolution all repoint. Runtime-built instance + per-frame `SetParam` **is** the MID (Material Instance Dynamic). Depends on 02; lands after 04. | proposed |
| 06 | [Material instances — cooker importer + editor inspector](06-material-instances-authoring.md) | The authoring surface over Plan 05's runtime: a `MaterialInstanceImporter` cooks `*.vmatinst.json` (parse `parent` + sparse `overrides`, validate names/types against the parent's reflected fields — the `.vmat`-against-shader check lifted to instance-against-parent), an editor material-instance inspector (parent picker + per-field override toggle over `Parent->GetFields()`), and an authored sample instance exercising the path on GPU. Depends on 05. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is the precursor (area 14) — it stands alone (cooker + shaders + examples + tests, no editor),
  and every generated shader in 02–04 `#include`s the engine header it lands. Engine/cooker/test-only;
  it is the one plan that touches no editor codegen.
- **01** depends on 00 — the emit walk targets the split engine header (the generated entry point
  `#include`s `material.slang` and writes its `GBufferOutput`/`SV_Target0`), and relocates the
  node-graph machinery into the shared lib the cooker will link in 02.
- **02** depends on 01 — the graph-sourced shader importer compiles the *output* of the emit walk;
  the const/exposed/engine-bound `Param` provenance is the first feature the generated `MaterialParams`
  needs.
- **03** depends on 02 — new emitter nodes ride the emit walk + the generated-struct machinery 01/02
  establish; nothing new in the cook/preview loop.
- **04** depends on 03 — the sample graphs use the expanded catalog, and the migration is the
  end-to-end proof of the whole chain.
- **05** depends on 02 (the exposed-param schema it overrides) and lands after **04** (the migrated
  parents it instances). It is the planset's **runtime/format** plan — `Material`, `MaterialInstance`,
  the cooked instance blob, the mesh/component/prefab material references — on a different file set from
  the editor codegen of 01–04.
- **06** depends on 05 — the cooker importer and the editor inspector are the authoring surface over
  05's runtime types.

The editor-codegen plans **chain** (`00 → 01 → 02 → 03 → 04`): 01–04 all touch the material codegen
files (`MaterialCompile`, `MaterialCatalog`) now living in the shared lib, so they merge in number
order. **05 → 06** run on the runtime/cook/editor-inspector file set and merge after 04. Per
[[project_megaexec_worktree_base]], `isolation: "worktree"` branches from `origin/main`: only **00**
(on `origin/main`) can use it directly; **01 → 02 → 03 → 04** each dispatch against a manual worktree
cut from the prior plan's integration commit, **05** against a worktree cut from 04's, and **06** from
05's. Plan 05's example migration edits `examples/hello-triangle/assets/`, which Plan 04 also rewrites,
so 05 must rebase on 04's manifest, not `origin/main`'s.

## The decisions this planset settles

- **The graph is the AST; there is no parsed-AST intermediate.** The node graph is already a typed,
  acyclic DAG with coercion on its links — re-parsing emitted text into an AST would be a
  structure→text→structure round trip that buys nothing. The only intermediate is a thin typed
  **`EmittedValue`** (the code-chunk model of Unreal/Unity), threaded by the emit walk.
- **Slang is the compiler; we only generate text.** No multi-backend IR, no graph-level
  optimization layer — Slang parses, type-checks, folds, and lowers the generated source to SPIR-V.
  veng's job is "structured graph → correct Slang." A deeper IR would reimplement the Slang
  front-end veng already links.
- **The cooker generates the fragment from the graph; the emit walk lives in a shared library.** A
  fragment-shader asset whose source is a graph is cooked by walking the graph → Slang → SPIR-V
  through the existing `ShaderImporter`. The walk (node-graph core + material catalog + emit) moves
  out of editor-only code into a library both `libveng_editor` and `libveng_cook` link, so the editor
  preview and an offline `vengc cook` run identical code. This keeps the authored graph the **single
  source of truth**: the generated Slang and SPIR-V are cook output, never checked in, so they cannot
  drift from the graph and the editor-writes/cooker-compiles split (and its silent-divergence risk) is
  gone.
- **No new asset, no minted or derived id.** The fragment-shader asset a material references already
  has an id and a manifest row. Codegen changes that shader's *uncooked source* from `.slang` to a
  graph; it does not mint, derive, or reserve any id. There is no checked-in generated file and **no
  cooked-material format change and no runtime change** — the material loads its fragment by id exactly
  as today.
- **`MaterialParams` is generated, lives in the authoring shader, and is ordered large-alignment-first.**
  There is no fixed engine param struct (planset-18 made the runtime block reflection-sized); area 14
  moves the struct out of the engine header because it is per-shader by definition, and codegen
  generates it per material. The emitter orders the generated fields vec4 → vec3 → vec2 → scalar/uint
  so the cooker's std140 reflection and the shader's scalar-layout `Load<T>` resolve identical offsets,
  and a GPU/pixel test guards the invariant (the C++ `static_assert` that used to guard it is retired
  with `MaterialParams.h`).
- **A `Param` has one of three provenances; the catalog is schema-independent.** *const* folds inline,
  *exposed* contributes an author-tweakable `MaterialParams` field with a default, *engine-bound*
  contributes a field the engine writes by name at runtime (no authored default, not an instance
  override surface — this is what lets `tonemap`'s `Hdr`/`RenderScale` be expressed). The node catalog
  is a fixed set of node types, no longer shaped from a loaded shader's reflected fields.
- **A material instance is a parameter override over a parent, not a flavor of `Material`.** Codegen
  makes the parent's exposed params a generated schema; Plan 05 makes that schema an instance's override
  surface. The split is parent (one pipeline, the expensive permutation) vs. instance (one cheap SSBO
  slot, no shader) — and the instance half is **relocated**, not invented: the per-material slot and the
  ring-buffered `SetParam`/`SetTexture` already live on `Material`. A bare parent doubles as a
  zero-override instance so existing material-id references keep loading. The term "material instance"
  thereby takes its standard cross-engine meaning, retiring the tree's old loose use of it for "a live
  `Material` object."

## What remains future

- **Pure-shader (graph) editing.** A fragment shader owns its graph, but the graph can only be edited
  by opening the **material** that references the shader. A standalone graph/shader editor — authoring a
  generated shader without a material wrapper, and sharing one generated shader across several materials —
  is a natural editor extension, deferred until the in-material flow proves the model.
- **Wired asset pins.** Textures stay node **properties** (`FieldClass::AssetHandle`), not wired
  pins — the topology core stays asset-agnostic, as planset-15 settled. A `Texture` *input pin*
  (sampling a texture produced by another node) is a later catalog extension.
- **Custom-expression / code nodes.** A node holding an authored Slang snippet (an escape hatch for
  shading the catalog does not cover) is a natural extension of the emitter, deferred until the fixed
  catalog proves insufficient.
- **Function/subgraph nodes** — reusable node groups compiled to Slang functions — behind the emit
  walk once a real reuse case appears.
- **Vertex-stage codegen.** Only the fragment is generated; the vertex stage stays the core
  `surface.vert`/`fullscreen.vert`. Author-controlled vertex deformation (a vertex domain output)
  is a far-future domain extension.
- **Static permutations / `#if` features.** Generated shaders are monolithic; a feature-toggle
  permutation system (static branches compiled out) is a separate codegen concern. A **static-switch
  parameter** (Unreal's, which forces a shader permutation) is the point where this meets material
  instances: it cannot be a cheap instance override — it changes the *parent's* compiled shader, so it
  is a parent-variant key the cooker would compile a permutation for. Deferred with the permutation
  system.
- **Draw-sort by parent pipeline.** Plan 05 ships correctness (bind the parent pipeline per instance,
  redundantly when consecutive draws share a parent). Sorting the draw plan by parent → bind once →
  iterate instance selectors is the performance payoff; the buffer-indexed indirect path (planset-25)
  already drives by per-draw `materialIndex`, so it is a sort key, not new machinery.
- **Instance-of-instance chains.** Plan 05 parents an instance to a `Material` only; nested instances
  (Unreal's MIC-of-MIC) that fold an override stack at load are deferred until a real layering case
  appears.
