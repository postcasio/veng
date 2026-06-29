# Plan 01 — the emit walk: a shared graph lib, `EmittedValue`, a schema-independent catalog

**Goal:** replace `CompileMaterialGraph`'s feeder-matching with a **topological emit walk** that
turns the node graph into generated Slang fragment source, and **relocate** the machinery it needs
into a library the cooker can link. Each node type gains an **emit-fn** mapping its input expressions
+ property bytes to an output expression; the walk threads a thin typed **`EmittedValue`** (one SSA
temp per output pin), applies the link-recorded coercion, and `MaterialOutput` emits the domain entry
point. The catalog stops being shaped from a loaded shader's reflected fields and becomes a fixed,
schema-independent node set. This plan keeps the **three existing node types**
(`TextureSample`/`Param`/`MaterialOutput`) and produces a Slang *string* — the cook wiring that
compiles it is Plan 02. Depends on **Plan 00** (the generated entry `#include`s the split header).

## Why it is its own plan

The emit walk is the heart of codegen and is **pure logic** (graph → string), testable device-free
exactly as today's `CompileMaterialGraph` is. Settling the emitter contract, the `EmittedValue`
layer, the SSA-temp scheme, the coercion application, the schema-independent catalog, and the
domain-driven `MaterialOutput` entry — against only the three known node types — keeps the catalog
expansion (Plan 03) and the cook plumbing (Plan 02) from entangling with the codegen core. Landing it
in the shared lib here is what lets Plan 02's cooker importer call the same walk the editor does.

## The relocation

`CompileMaterialGraph`, the material node catalog (`MaterialCatalog`), and the generic node-graph
topology core (`NodeGraph`/`NodeType`/`NodeGraphSerialize`) move out of editor-only code into a new
library — **`libveng_graph`** (`veng::graph`) — linked PUBLIC by both `libveng_editor` and
`libveng_cook`. The topology core is already dependency-clean (`NodeGraph.h`: *"Knows nothing of
ImGui, Vulkan, or any domain"*; it includes only `Veng.h` + reflection), and `MaterialCompile.cpp`
pulls only `Veng` reflection + `nlohmann/json`, which `libveng_cook` already links — so the move is a
relocation, not a detangle. The editor's node-graph **UI** (panels, imnodes) stays in the editor and
links `veng::graph`. No dependency cycle: `veng::graph → veng::veng`; `editor → graph`; `cooker →
graph`. `include_hygiene` is unaffected — the lib's public headers name only `string`/`PinType`/glm.

## What lands

- **`EmittedValue` — the thin typed code-chunk.** A small value type the walk threads:
  ```cpp
  struct EmittedValue {
      Veng::string Expr;     // "n2_Color" (an SSA temp) or "float4(0.8,0.2,0.1,1)" (inlined)
      PinType      Type;     // carried so coercion composes by type, not by string-munging
      bool         IsConst;  // a const source folds inline; a non-const reads its temp
  };
  ```
  It is **not** an AST — it is the code-chunk model (Unreal's translator chunks, Unity's slot vars).
  No parsing, no typed expression tree: the graph already *is* the typed DAG.

- **An emit-fn per node type, registered by the material catalog.** `NodeType`
  ([NodeType.h](../../editor/include/VengEditor/NodeGraph/NodeType.h), relocating to `veng::graph`) is
  data, not a subclass, so the emit-fn is a `function<...>` carried on the material-catalog side (a
  table keyed by `NodeTypeId`, populated by `RegisterMaterialNodeTypes`) — the topology core stays
  generic and emit-free. The signature is roughly `EmittedValue emit(std::span<const EmittedValue> inputs, std::span<const std::byte> propertyBytes, EmitContext&)`, where `EmitContext` owns the temp
  counter and the growing source body.

- **The catalog is fixed and schema-independent.** Today `RegisterMaterialNodeTypes(catalog, shader,
  domain)` shapes `TextureSample`/`Param` from the loaded shader's reflected `Fields`
  ([MaterialCatalog.h](../../editor/src/material/MaterialCatalog.h)), and the panel `LoadSync<Material>`s
  to seed those fields. Codegen inverts that: node types become a **fixed set** keyed by domain only
  (`RegisterMaterialNodeTypes(catalog, domain)`), shaped by pin **leaf types**, not by any loaded
  shader's fields — `TextureSample` always has a UV input + a color output, a `Param` is sized by its
  own property, `MaterialOutput`'s sinks come from the domain contract. The first-author path is then
  graph → generate → cook → load, never "load a material to define the catalog." (The material's
  field-list-vs-shader *validation* still uses the generated shader's reflection — that survives in
  Plan 02; only the catalog stops depending on it.)

- **The topological emit walk in `CompileMaterialGraph`.** From `MaterialOutput`, walk the graph in
  dependency order (the topology core guarantees acyclicity, so the sort is well-defined; an
  unreached node never emits → free dead-code elimination). For each node, gather its inputs'
  `EmittedValue`s (an unconnected input yields the pin's default `EmittedValue`), call the emit-fn,
  **assign an SSA temp** per output pin (`<type> n<StableNodeKey>_<PinName> = <expr>;` appended to the
  body), and record the temp as the node-output's `EmittedValue` for downstream substitution. A value
  used more than once is always a temp (so a `TextureSample` feeding two sinks samples once); a
  single-use value **always inlines** (one rule, not "may", so the output is a pure function of the
  graph). Temp names derive from a **stable per-node key** (not raw iteration index) so the same
  logical graph emits byte-identical text across two walks and across a save/load round-trip.

- **Coercion applied by the walk, from the link record.** The splat/truncate planset-15 records on
  each link (`f32→vecN`, `vec4→vec3/vec2`, via `MaterialCanConnect`) is applied when an upstream
  `EmittedValue` is substituted into a downstream input of a different arity — wrapping the source
  expr by type (`.xxx`, `float4(v, 0)`, `v.xyz`). `EmittedValue::Type` makes this a typed operation,
  not a guess at the text.

- **`MaterialOutput` emits the domain entry point.** Its sink pins are already the domain contract
  (`DomainOutputContract` in `MaterialCatalog.h`: Surface → Albedo vec4 + Normal vec3;
  PostProcess → Color vec4). The walk emits the entry function:
  - **Surface:** `GBufferOutput fsMain(VSOutput input)` writing `o.Albedo`/`o.Normal`/`o.ORM`/
    `o.Velocity` from the connected sink `EmittedValue`s. Unconnected sinks emit defined defaults:
    Albedo `float4(0,0,0,1)`, Normal the geometric `input.v_WorldNormal`, ORM `float3(1,1,0)`
    (occlusion 1, roughness 1, metallic 0), and **Velocity always
    `ComputeMotionVector(input.v_CurClip, input.v_PrevClip)`** (the always-on velocity channel TAA
    depends on — never authorable away).
  - **PostProcess:** `float4 fsMain(VSOutput input) : SV_Target0` from the connected `Color` sink
    (default: pass through the screen-UV sample), reading its selector from the PostProcess push block
    (`g_PC.MaterialIndex`, Plan 00).

  The generated source is prefixed with `#include "Veng/material.slang"` (Plan 00) and the (Plan 02)
  `MaterialParams` struct.

- **The three existing nodes become emitters.** `TextureSample` emits the bindless sample
  (`g_Textures[NonUniformResourceIndex(p.<Handle>)].Sample(g_Samplers[...], <uv>)`, the UV input
  defaulting to `input.v_UV` when unconnected); `Param` emits its value (this plan: always a uniform
  read `p.<Name>` — the const/exposed/engine-bound provenance arrives in Plan 02); `MaterialOutput` is
  the sink above. The generated `MaterialParams` field set is *implied* by the texture + param nodes
  (Plan 02 materializes the struct; this plan emits a provisional struct so the source is compilable
  in tests).

- **`CompileMaterialGraph`'s return type changes** from `vector<CompiledField>` to a
  `GeneratedFragment { string Source; MaterialDomain Domain; … }` (the param-schema/field-list halves
  fill in Plan 02). `WriteMaterialVmat`/`CompiledField` are left in place for Plan 02 to rework.

## Decisions

1. **The graph is the AST; the only intermediate is `EmittedValue`.** No parsed-AST step — the graph
   is already a typed acyclic DAG. `EmittedValue` is the code-chunk layer that makes coercion,
   const-folding (Plan 02), and temp-vs-inline first-class; nothing more.
2. **The walk lives in `veng::graph`, linked by editor and cooker.** The emit-fn lives where
   `MaterialCanConnect` and the domain contract live, now relocated to the shared lib so the cooker can
   run the identical walk in Plan 02. The generic `NodeGraph`/`NodeCatalog` stays asset- and
   codegen-agnostic within it.
3. **The catalog is schema-independent.** Node types are a fixed set keyed by domain, shaped by pin
   leaf types — not by a loaded shader's reflected fields. This is the inversion's bootstrap: a
   from-scratch material defines its schema by authoring a graph, never by loading a cooked material.
4. **One SSA temp per output pin; multi-use forces a temp, single-use always inlines.** A clean,
   debuggable generated body, correct single-evaluation of a shared `TextureSample`, and a
   deterministic (pure-function-of-graph) output. Slang's own CSE handles redundancy beyond that.
5. **`MaterialOutput` drives the entry point from the domain contract.** The domain decides the
   function signature and the writes; an unconnected sink gets a defined default, never invalid Slang.
   Velocity is always `ComputeMotionVector(...)`, not defaultable away.
6. **Slang does the compiling.** The walk emits text and stops — type-checking, folding, and lowering
   are Slang's. veng builds no optimizer.

## Files

| File | Change |
|---|---|
| `engine/`/`editor/` → new `libveng_graph` (`veng::graph`) | Relocate `NodeGraph`/`NodeType`/`NodeGraphSerialize` + `MaterialCatalog` + `MaterialCompile` into the shared lib; CMake target linked PUBLIC by editor and cooker. |
| `MaterialCompile.h` / `.cpp` (in `veng::graph`) | Rewrite `CompileMaterialGraph` as the topological emit walk; introduce `EmittedValue`, `EmitContext`, the `GeneratedFragment` return type; the entry-point assembly + unconnected-sink defaults per domain. |
| `MaterialCatalog.h` / `.cpp` (in `veng::graph`) | Schema-independent registration (`RegisterMaterialNodeTypes(catalog, domain)`); an emit-fn per node type; emit-fns for `TextureSample`/`Param`/`MaterialOutput`; per-domain entry/default-sink helpers. |
| `editor/src/panels/MaterialEditorPanel.cpp` | Stop seeding the catalog from a `LoadSync<Material>`'d field set; register the fixed catalog by domain. |
| `tests/…` (cooker/unit band, device-free) | Unit-test the emit walk: a fixed graph → expected generated Slang (golden string / structural assertions), coercion cases, an unconnected-sink default (incl. Velocity), a shared-`TextureSample` single-temp case, and a determinism check (two walks + a round-trip → byte-identical). |
| `editor/CLAUDE.md`, `cooker/CLAUDE.md` | Document the emit walk + `EmittedValue` + the shared `veng::graph` lib (the catalog now emits Slang, not a field list). |

## Verification

- Clean build; `ctest` green. The new device-free emit-walk tests pass (generated source matches the
  expected structure; coercion + defaults + single-evaluation + determinism hold).
- `smoke_golden` does **not** move — this plan produces a string in the shared lib and ships nothing
  into the cook path yet (no `.vmat`/shader-source change), so the rendered output is unchanged.
- `include_hygiene` unaffected — `EmittedValue`/`GeneratedFragment` and the relocated headers name only
  `string`/`PinType`/glm, no backend leak.
- The existing material-editor open/edit/preview still works: it stays on the prior cook path until
  Plan 02 routes the cook through the generated source. (Plan 01 does not reroute preview.)

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
