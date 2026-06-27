# planset-38 — node→Slang material codegen (the graph generates the shader)

**Phase goal:** make the node material editor **author shading**, not wire a pre-existing
shader. Today a `.vmat` graph is a binding diagram over a hand-authored fragment shader: a
`TextureSample` picks which texture fills a reflected field, a `Param` fills a value field, and
`MaterialOutput`'s pins mirror the loaded shader's `Material::GetFields()`
([MaterialCompile.cpp](../../editor/src/material/MaterialCompile.cpp) matches feeders to fields
positionally and ignores the graph's topology — note its `(void)domain;`). This planset inverts
that: every node becomes an **expression emitter**, the graph is topologically walked into
**generated Slang fragment source**, and the cooked material references that generated shader.
The hand-authored fragment shader goes away — the graph *is* the source.

This is [future area 13](../future/README.md#13-material-domains--shader-graph-codegen--prioritized)'s
**prioritized follow-on** (node→Slang codegen), built on the material-domains slice
(planset-18) and the node-graph surface (planset-15). It folds in
[future area 14](../future/README.md#14-engine-owned-material-shader-header--cross-pack-slang-includes--prioritized)
(engine-owned material shader header + cross-pack Slang includes) as its **precursor** (Plan 00),
because a generated fragment shader must be able to `#include` the engine's material contract —
which nothing can do today. **Design overview:** [material-codegen.md](../future/material-codegen.md).

## The shape — the graph is the AST; Slang is the compiler

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

**The generated fragment shader is a sub-asset.** The editor's emit step writes a
`<name>.gen.frag.slang` + `<name>.gen.frag.shader.json` pair beside the `.vmat`, with a **derived**
`AssetId` (a deterministic transform of the material's id — stable across regenerations, never
minted), adds its `{id, type, source}` row to the pack manifest, and the `.vmat` references it by
`shaders.fragment`. The cooker compiles it through the **existing** `ShaderImporter` path (with
Plan 00's cross-pack include resolving `#include "Veng/material.slang"`); the runtime resolves the
fragment by id exactly as it does a hand-authored shader today. **No cooked-material format change
and no runtime change** — the generated files are checked in, so an offline `vengc cook` with no
editor running compiles them like any shader.

**The `.vmat` still carries a field list**, but it is now *generated from the graph* (the exposed
params' defaults + the texture nodes' `AssetId`s) rather than *matched against a loaded shader's
reflection*. Because the same emitter produces the generated `MaterialParams` struct **and** the
field list in one walk, they are guaranteed to agree — which is exactly the invariant the cooker's
offset-patching relies on, so `WriteMaterialVmat`/`CompiledField` largely survive.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Cross-pack Slang includes + the material header split (area 14) | A shared cooker Slang-session helper adds the engine core shader dir to `searchPaths` (the three `searchPathCount = 1` sites), so any `.slang` can `#include "Veng/material.slang"`. `material.slang` splits: the **engine contract** stays (bindless declarations, `g_ViewConstants`, `GBufferOutput`, `DrawData`, `PushConstants`, `ComputeMotionVector`, and the per-domain fragment-input struct), **`MaterialParams` moves to the authoring shader** (it is per-shader by definition). Both vendored `material_data.slang` copies are deleted; `brick`/`character`/`flat` `#include` the engine header; the `Veng/Renderer/MaterialParams.h` C++ mirror is retired. Hand-authored shaders, same SPIR-V — `smoke_golden` does not move. | proposed |
| 01 | The emit walk — expression-emitting nodes + `EmittedValue` | `CompileMaterialGraph` is rewritten from feeder-matching to a **topological emit**: a per-node-type emit-fn (registered by the material catalog) maps input `EmittedValue`s + property bytes to an output `EmittedValue`; the walk assigns one SSA temp per output pin and substitutes downstream, applying link-recorded coercion (splat/truncate) by type. `MaterialOutput` emits the domain entry point (`GBufferOutput` writes for Surface, `SV_Target0` for PostProcess) from its connected sinks, with sensible defaults for unconnected ones. Output is a Slang source string. Still only the three existing node types. Depends on 00. | proposed |
| 02 | const/exposed `Param`, generated `MaterialParams`, the sub-asset pipeline | A `Param` gains a **const-vs-exposed** flag (const folds inline; exposed becomes a generated `MaterialParams` field + a `p.<Name>` read). The emitter generates the `MaterialParams` struct + the `.vmat` field list together. The editor writes the `<name>.gen.frag.{slang,shader.json}` sub-asset with the derived id, adds the manifest row, repoints `.vmat` `shaders.fragment`, and the cook-on-demand loop compiles it → hot-reload → `MaterialPreview`. The full graph→cook→reload→preview loop on generated source. Depends on 01. | proposed |
| 03 | Expand the node catalog to real shading math | The catalog grows real emitter nodes — `Constant`, `Multiply`/`Add`/`Subtract`/`Divide`, `Lerp`, `Dot`/`Normalize`/`Saturate`, `Split`/`Combine`, `ScalarParam` — each a `NodeType` with reflected properties, an emit-fn, and JSON round-trip. The graph becomes genuinely expressive (these nodes are inert in the binding model — the tell the catalog was half-built for codegen). Depends on 02. | proposed |
| 04 | Migrate the samples onto generated graphs | hello-triangle's `brick.vmat` becomes a generated graph (delete `brick.frag.slang`, author the equivalent graph, regenerate the sub-asset); `tonemap.vmat` becomes a generated PostProcess graph (the domain-sink proof); template co-migrated. The image is preserved by construction where possible; `smoke_golden` may move **once** → regenerate. Depends on 03. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is the precursor (area 14) — it stands alone (cooker + shaders + examples, no editor), and
  every generated shader in 01–04 `#include`s the engine header it lands. Engine/cooker-only; it
  is the one plan that touches no editor codegen.
- **01** depends on 00 — the emit walk targets the split engine header (the generated entry point
  `#include`s `material.slang` and writes its `GBufferOutput`/`SV_Target0`).
- **02** depends on 01 — the sub-asset pipeline ships the *output* of the emit walk; const/exposed
  `Param` is the first feature the generated `MaterialParams` needs.
- **03** depends on 02 — new emitter nodes ride the emit walk + the generated-struct machinery 01/02
  establish; nothing new in the cook/preview loop.
- **04** depends on 03 — the sample graphs use the expanded catalog, and the migration is the
  end-to-end proof of the whole chain.

The plans **chain** (`00 → 01 → 02 → 03 → 04`): 01–04 all touch the editor's material codegen files
(`MaterialCompile`, `MaterialCatalog`), so they merge in number order. Per
[[project_megaexec_worktree_base]], `isolation: "worktree"` branches from `origin/main`: only
**00** (on `origin/main`) can use it directly; **01 → 02 → 03 → 04** each dispatch against a manual
worktree cut from the prior plan's integration commit.

## The decisions this planset settles

- **The graph is the AST; there is no parsed-AST intermediate.** The node graph is already a typed,
  acyclic DAG with coercion on its links — re-parsing emitted text into an AST would be a
  structure→text→structure round trip that buys nothing. The only intermediate is a thin typed
  **`EmittedValue`** (the code-chunk model of Unreal/Unity), threaded by the emit walk.
- **Slang is the compiler; we only generate text.** No multi-backend IR, no graph-level
  optimization layer — Slang parses, type-checks, folds, and lowers the generated source to SPIR-V.
  veng's job is "structured graph → correct Slang." A deeper IR would reimplement the Slang
  front-end veng already links.
- **The generated fragment shader is a checked-in sub-asset, referenced by a derived id.** The
  editor writes `<name>.gen.frag.{slang,shader.json}` beside the `.vmat`; its `AssetId` is a
  deterministic transform of the material's id (stable, never minted), and it cooks through the
  existing `ShaderImporter` path. This keeps the cooker **graph-agnostic** (it compiles stored Slang
  text, never traverses the graph), keeps an offline `vengc cook` working with no editor, and means
  **no cooked-material format change and no runtime change**. The cost — generated files in the tree
  and pack manifest — is the accepted, conventional one (Unreal/Unity generate shader source too).
- **`MaterialParams` is generated, and lives in the authoring shader.** There is no fixed engine
  param struct (planset-18 already made the runtime block reflection-sized); area 14 moves the struct
  out of the engine header because it is per-shader by definition, and codegen generates it per
  material. The cooker reflects the generated struct exactly as it reflects a hand-authored one — the
  offset-patching loader and the ring-buffered block are untouched.
- **The emitter lives in the editor, never the cooker.** The emit walk consumes `NodeGraph`/
  `NodeCatalog`, which live in `libveng_editor` + editor src; the cooker does not (and must not) link
  the editor. So generation runs on save in the editor and writes Slang text; the cooker only
  compiles text. This mirrors the existing `_editor` graph + regenerated artifact split.
- **Const-vs-exposed is a `Param` property, not two node types.** One `Param` node with a flag —
  const folds inline as a Slang literal, exposed contributes a `MaterialParams` field — so the
  generated uniform set is exactly the exposed params, and the field list/defaults follow.
- **No module-ABI, on-disk-material-format, or runtime change.** The only on-disk additions are the
  graph's existing `"_editor"` block growing the `Param` flag and the new generated shader source
  files; the cooked material blob, the loader, and `VengModuleHost` are untouched.

## What remains future

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
  permutation system (static branches compiled out) is a separate codegen concern.
