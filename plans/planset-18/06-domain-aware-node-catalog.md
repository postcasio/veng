# Plan 06 — domain-aware node catalog

**Goal:** fix the backwards part of the node material editor and lay the foundational
inversion toward codegen. Today `MaterialOutput`'s input pins are **derived from the loaded
shader's reflected `GetFields()`** — the graph conforms to a fragment shader that already
exists. This plan makes the node catalog **domain-aware**: `MaterialOutput`'s pins follow
the **domain's output contract**, and the editor selects the catalog by the loaded
material's domain. "Each domain has a different set of output pins."

## The current design and its inversion

[MaterialCatalog.cpp](../../editor/src/material/MaterialCatalog.cpp)
`RegisterMaterialNodeTypes` builds the `MaterialOutput` node by iterating
`shader.Fields` — i.e. one input pin per field the **hand-authored fragment shader**
declared, read back through `Material::GetFields()`. The output node is a mirror of a
shader that pre-exists; the graph is a wiring diagram over it. That is the inversion
[material-codegen.md](../future/material-codegen.md) calls out: under a real authoring
system the graph **is** the source, and the output node is the **domain's fixed contract**
(Albedo/Normal for Surface, Color for PostProcess), not a reflection of someone's shader.

## What lands

### The catalog takes the domain

`RegisterMaterialNodeTypes` gains a `MaterialDomain` parameter (alongside the existing
`MaterialShaderInterface`). `MaterialOutput`'s pins are built from the **domain's output
contract**, a small fixed table per domain:

- **PostProcess** → a single `Color` input pin (`vec4`). A postprocess graph authors one
  output: the final color. This is the clean, fully-inverted case — there is no per-shader
  field list to mirror; the sink is the domain's contract.
- **Surface** → the g-buffer contract pins (`Albedo` `vec4`, `Normal` `vec3`). The Surface
  sink expresses `GBufferOutput`, the same fixed contract
  [GBuffer.h](../../engine/include/Veng/Renderer/GBuffer.h) defines — not the loaded
  shader's reflected fields.

The `MaterialShaderInterface` (the loaded material's `GetFields()`) stays the source for
the **input-side** catalog — which `Param`/`TextureSample` nodes are available and how a
`Param`'s pin type is sized — because, under the still-current binding model (no codegen),
compile binds the graph's authored values to the cooked material's param/texture fields.
What inverts in this plan is the **output node**, the half the codegen design inverts first.

### The editor reads the domain

[MaterialEditorPanel.cpp](../../editor/src/panels/MaterialEditorPanel.cpp) reads the loaded
material's `GetDomain()` (plan 02) and passes it into `RegisterMaterialNodeTypes`, so
opening a `surface` material yields the g-buffer sink and a `postprocess` material yields
the Color sink. The node-property inspector and the per-`FieldClass` widgets are unchanged
(this plan touches the catalog's output-node construction and the panel's catalog setup, not
the widget layer).

### Compile honors the domain-driven sink

Two functions in [MaterialCompile.cpp](../../editor/src/material/MaterialCompile.cpp) both
iterate `MaterialOutput`'s input pins and match each against `shader.Fields` **by name**
(`CompileMaterialGraph`, and `BuildGraphFromMaterial`'s
`VE_ASSERT(field != nullptr, ...)`). Under the domain-driven output node those pins are
domain-contract names (`Color` for PostProcess; `Albedo`/`Normal` for Surface) that do **not**
appear in `shader.Fields`, so both would assert. This plan restructures both around the
domain contract:

- **`BuildGraphFromMaterial`** (the flat-`.vmat`→graph import that reconstructs a graph when a
  material is opened) seeds the output node from the **domain's pin table**, not by matching
  field names. Its existing per-field feeder seeding (textures/params) still reads
  `shader.Fields`, but the output-node construction no longer looks the sink pins up as
  fields.
- **`CompileMaterialGraph`** maps the domain sink pins to the compiled output by domain: for
  Surface the Albedo/Normal sinks drive the g-buffer field bindings as before; for PostProcess
  the single `Color` sink is the postprocess color path, and the emitted `fields` come from
  the graph's upstream `Param`/`TextureSample` topology (the authored params/handles), not
  from a sink pin named `Color`.
- **`WriteMaterialVmat`** emits the `"domain"` key (`"surface"`/`"postprocess"`) into the
  generated `.vmat.json` — without it a node-authored PostProcess material would cook as the
  default `surface` and fail plan 02's contract check. It takes the material's
  `MaterialDomain` to do so.

This stays a binding model (no codegen) — the seam codegen later replaces with "emit the
fragment entry that writes these domain outputs."

## Decisions

1. **Only the output node inverts in this planset; input nodes still bind.** Making
   `MaterialOutput` domain-driven is the foundational, codegen-shaped step the user asked
   for and the design names first. Making every node an **emitter** (so `TextureSample`
   emits `tex.Sample(...)` and math nodes emit real code), `Param` const-vs-exposed, and
   compile's target generated Slang — that is the codegen follow-on. This plan stops at the
   output-node inversion + domain selection, the clean boundary: the graph's *shape* becomes
   correct (domain contract out), its *body* still binds to a hand-authored fragment shader.

2. **Surface keeps its g-buffer sink even though binding still flows through fields.** A
   reader might ask why invert Surface's output node now when compile still binds to the
   shader's fields. Because the **contract** is the right source of truth regardless of
   codegen — `GBufferOutput` is fixed in the engine, not per-shader — and the editor should
   present it as the domain's, not as a reflection of whichever shader happens to be loaded.
   This also makes Surface and PostProcess symmetric (both sinks are domain contracts), the
   shape codegen needs.

3. **The input-side catalog still reads `MaterialShaderInterface`.** Which params/textures a
   graph can author still comes from the loaded material's `GetFields()` under the binding
   model. This plan does not touch that — only the output sink. The full inversion of the
   input side (params generated *by* the graph rather than read *from* a shader) is codegen.

4. **No runtime or cooker change.** This is an editor-only plan over the loaded material's
   domain (read via plan 02's `GetDomain()`). It compiles to the same `.vmat` field-list
   target (now with the `"domain"` key); a `postprocess` material authored in the node editor
   cooks and loads through plans 02/04/05's path.

## Files

| File | Change |
|---|---|
| `editor/src/material/MaterialCatalog.h` | `RegisterMaterialNodeTypes` gains a `MaterialDomain` parameter; declare the per-domain output-contract table. |
| `editor/src/material/MaterialCatalog.cpp` | Build `MaterialOutput`'s pins from the domain contract (Color for PostProcess; Albedo/Normal for Surface) instead of iterating `shader.Fields`. |
| `editor/src/material/MaterialCompile.cpp` | `CompileMaterialGraph`: map the domain sink pins to the compiled output by domain. `BuildGraphFromMaterial`: seed the output node from the domain pin table, not by matching `shader.Fields`. `WriteMaterialVmat`: emit the `"domain"` key. |
| `editor/src/panels/MaterialEditorPanel.cpp` | Read the loaded material's `GetDomain()`; pass it into the catalog. |

## Verification

- Clean build; the editor compiles a `surface` material's graph (g-buffer sink) and a
  `postprocess` material's graph (Color sink) through the existing compile→cook→hot-reload
  loop.
- Opening hello-triangle's `brick.vmat` (surface) shows the Albedo/Normal output sink and
  round-trips edits as before (the input-side `Param`/`TextureSample` behavior is unchanged).
- Opening the core `tonemap.vmat` (postprocess) shows a single `Color` output sink; the
  graph compiles to the postprocess `.vmat` and previews through `MaterialPreview`.
- No runtime/cooker test moves — this plan is editor-only over plan 00's domain.
