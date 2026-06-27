# Plan 01 — the emit walk: expression-emitting nodes + `EmittedValue`

**Goal:** replace `CompileMaterialGraph`'s feeder-matching with a **topological emit walk** that
turns the node graph into generated Slang fragment source. Each node type gains an **emit-fn**
mapping its input expressions + property bytes to an output expression; the walk threads a thin typed
**`EmittedValue`** (one SSA temp per output pin), applies the link-recorded coercion, and
`MaterialOutput` emits the domain entry point. This plan keeps the **three existing node types**
(`TextureSample`/`Param`/`MaterialOutput`) and produces a Slang *string* — the sub-asset pipeline
that ships it is Plan 02. Depends on **Plan 00** (the generated entry `#include`s the split header).

## Why it is its own plan

The emit walk is the heart of codegen and is **pure logic** (graph → string), testable device-free
exactly as today's `CompileMaterialGraph` is. Settling the emitter contract, the `EmittedValue`
layer, the SSA-temp scheme, the coercion application, and the domain-driven `MaterialOutput` entry —
against only the three known node types — keeps the catalog expansion (Plan 03) and the cook/preview
plumbing (Plan 02) from entangling with the codegen core. It is the same foundation-first move: land
the mechanism with the minimal node set, then widen.

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
  ([NodeType.h](../../editor/include/VengEditor/NodeGraph/NodeType.h)) is data, not a subclass, so the
  emit-fn is a `function<...>` carried on the material-catalog side (a table keyed by `NodeTypeId`,
  populated by `RegisterMaterialNodeTypes`) — the topology core stays generic and emit-free. The
  signature is roughly `EmittedValue emit(std::span<const EmittedValue> inputs, std::span<const std::byte> propertyBytes, EmitContext&)`, where `EmitContext` owns the temp counter and the
  growing source body.

- **The topological emit walk in `CompileMaterialGraph`.** From `MaterialOutput`, walk the graph in
  dependency order (the topology core guarantees acyclicity, so the sort is well-defined; an
  unreached node never emits → free dead-code elimination). For each node, gather its inputs'
  `EmittedValue`s (an unconnected input yields the pin's default `EmittedValue`), call the emit-fn,
  **assign an SSA temp** per output pin (`float4 n<NodeIndex>_<PinName> = <expr>;` appended to the
  body), and record the temp as the node-output's `EmittedValue` for downstream substitution. A
  value used once *may* inline; a value used more than once is always a temp (so a `TextureSample`
  feeding two sinks samples once).

- **Coercion applied by the walk, from the link record.** The splat/truncate planset-15 records on
  each link (`f32→vecN`, `vec4→vec3/vec2`, via `MaterialCanConnect`) is applied when an upstream
  `EmittedValue` is substituted into a downstream input of a different arity — wrapping the source
  expr by type (`.xxx`, `float4(v, 0)`, `v.xyz`). `EmittedValue::Type` makes this a typed operation,
  not a guess at the text.

- **`MaterialOutput` emits the domain entry point.** Its sink pins are already the domain contract
  (`DomainOutputContract` in [MaterialCatalog.h](../../editor/src/material/MaterialCatalog.h):
  Surface → Albedo vec4 + Normal vec3; PostProcess → Color vec4). The walk emits the entry function:
  for Surface, a `GBufferOutput fsMain(VSOutput input)` writing `o.Albedo`/`o.Normal`/`o.ORM`/
  `o.Velocity` from the connected sink `EmittedValue`s (unconnected sinks emit sensible defaults —
  Albedo `float4(0,0,0,1)`, Normal the geometric `input.v_WorldNormal`, ORM/Velocity the standard
  expressions); for PostProcess, a single `float4 fsMain(...) : SV_Target0`. The generated source is
  prefixed with `#include "Veng/material.slang"` (Plan 00) and the (Plan 02) `MaterialParams` struct.

- **The three existing nodes become emitters.** `TextureSample` emits the bindless sample
  (`g_Textures[NonUniformResourceIndex(p.<Handle>)].Sample(g_Samplers[...], <uv>)`, the UV input
  defaulting to `input.v_UV` when unconnected); `Param` emits its value (this plan: always a uniform
  read `p.<Name>` — const-vs-exposed arrives in Plan 02); `MaterialOutput` is the sink above. The
  generated `MaterialParams` field set is *implied* by the texture + param nodes (Plan 02 materializes
  the struct; this plan can emit a provisional struct to keep the source compilable in tests).

- **`CompileMaterialGraph`'s return type changes** from `vector<CompiledField>` to a
  `GeneratedFragment { string Source; MaterialDomain Domain; … }` (the param-schema/field-list halves
  fill in Plan 02). `WriteMaterialVmat`/`CompiledField` are left in place for Plan 02 to rework.

## Decisions

1. **The graph is the AST; the only intermediate is `EmittedValue`.** No parsed-AST step — the graph
   is already a typed acyclic DAG. `EmittedValue` is the code-chunk layer that makes coercion,
   const-folding (Plan 02), and temp-vs-inline first-class; nothing more.
2. **Emit is registered by the material catalog, off the topology core.** The emit-fn lives where
   `MaterialCanConnect` and the domain contract already live, so the generic `NodeGraph`/`NodeCatalog`
   stays asset- and codegen-agnostic.
3. **One SSA temp per output pin; multi-use forces a temp.** A clean, debuggable generated body and
   correct single-evaluation of a shared `TextureSample`. Slang's own CSE handles any redundancy
   beyond that, so the walk does not over-optimize.
4. **`MaterialOutput` drives the entry point from the domain contract.** The domain decides the
   function signature and the writes; an unconnected sink gets a defined default, never invalid Slang.
   This is the inversion the area began (planset-18 made `MaterialOutput` domain-driven) carried to
   its conclusion.
5. **Slang does the compiling.** The walk emits text and stops — type-checking, folding, and lowering
   are Slang's. veng builds no optimizer.

## Files

| File | Change |
|---|---|
| `editor/src/material/MaterialCompile.h` / `.cpp` | Rewrite `CompileMaterialGraph` as the topological emit walk; introduce `EmittedValue`, `EmitContext`, and the `GeneratedFragment` return type; the entry-point assembly per domain. |
| `editor/src/material/MaterialCatalog.h` / `.cpp` | Carry an emit-fn per material node type; emit-fns for `TextureSample`/`Param`/`MaterialOutput`; the per-domain entry-point/default-sink helpers. |
| `tests/…` (cooker/unit band, device-free) | Unit-test the emit walk: a small fixed graph → expected generated Slang (golden string / structural assertions), coercion cases (splat/truncate), an unconnected-sink default, a shared-`TextureSample` single-temp case. |
| `editor/CLAUDE.md` | Document the emit walk + `EmittedValue` (the catalog now emits Slang, not a field list). |

## Verification

- Clean build; `ctest` green. The new device-free emit-walk tests pass (generated source matches the
  expected structure; coercion + defaults + single-evaluation hold).
- `smoke_golden` does **not** move — this plan produces a string in the editor and ships nothing into
  the cook path yet (no `.vmat`/sub-asset change), so the rendered output is unchanged.
- `include_hygiene` unaffected — `EmittedValue`/`GeneratedFragment` are editor-src types naming only
  `string`/`PinType`/glm, no backend leak.
- The existing material-editor open/edit/preview still works (it falls back to the prior path until
  Plan 02 repoints the cook — or, if 01 already routes preview through the generated source, the
  preview renders correctly against the provisional struct).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
