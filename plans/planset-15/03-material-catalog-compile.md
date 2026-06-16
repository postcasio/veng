# Plan 03 — material catalog + compile (Layer 3)

**Goal:** the **material specialization** — the only layer that says "material". A concrete
node catalog for materials, the coercion-aware `CanConnect` predicate, the compile that turns
a graph into a `.vmat` field list, and the inverse import that synthesizes a graph from a
flat `.vmat`. Lives in `editor/src/material/` — a consumer of the generic `VengEditor/NodeGraph/`
surface, not part of the public surface.

## The material node catalog (v1)

Registered into a `NodeCatalog` at panel construction. v1 binds parameters to an
author-provided shader's reflected `MaterialData` (decision 9) — no codegen:

| Node | Inputs | Outputs | Properties |
|---|---|---|---|
| `MaterialOutput` | per `MaterialData` field (Albedo, Factors, …) | — | — |
| `TextureSample` | `UV` (vec2, optional) | `Color` (vec4) | `Texture` : `AssetHandle<Texture>` |
| `Param` (constant) | — | `Value` (typed) | `Value` : the leaf (f32/vec2/vec3/vec4) |

The `MaterialOutput` node's **input pins are derived from the fragment shader's reflected
`MaterialData`** — one pin per field, typed to the field (`texture` field → a pin a
`TextureSample.Color` feeds; scalar/vector field → a pin a `Param.Value` feeds). The shader's
reflected interface is already available to the editor through the cooked material / its
`*.shader.json`; the panel passes the resolved field list in so the output node matches the
material's actual shader (the same set `MaterialImporter` validates against).

`TextureSample.Texture` is a **property, not a wired pin** (decision 6): an
`AssetHandle<Texture>` field drawn by the reused asset-picker widget, compiled to a `texture`
field of the `.vmat`.

## Type compatibility — the coercion predicate

The `CanConnectFn` the material catalog supplies to `NodeGraph`. Beyond exact-`TypeId`
equality it permits the conventional scalar/vector coercions: `f32`→`vecN` (splat),
`vec4`→`vec3`/`vec2` (truncate). This is the one place coercion lives; Layer 1 stays a pure
DAG enforcer. Coercion is recorded on the link's compile so the emitter knows to splat /
truncate.

## Compile — graph → `.vmat` field list

```cpp
// editor/src/material/MaterialCompile.h
Veng::Result<nlohmann::json> CompileMaterialGraph(
    const VengEditor::NodeGraph&, const VengEditor::NodeCatalog&,
    const MaterialShaderInterface& shader);   // the resolved MaterialData fields + shader ids
```

Walk `TopoOrder` from the `MaterialOutput` node's connected inputs:

- A `MaterialData` output-pin fed by a `TextureSample` → a `{ "name", "type":"texture",
  "id": <the property's AssetId> }` field (plus the paired `sampler` field, mirroring the
  hand-authored `brick.vmat.json`).
- A field fed by a `Param` → a `{ "name", "type":"vecN"/"float", "value": [...] }` field,
  applying any recorded coercion.
- An unconnected `MaterialData` field → omitted (the importer's schema tolerance keeps its
  default), or emitted with the shader's default — omit for v1.

The output is the **`fields` array** (and the `shaders` block, carried unchanged from the
source); the panel patches it into the `.vmat.json` and recooks. The result is exactly the
schema `MaterialImporter` already cooks — **no importer change**.

## Inverse — flat `.vmat` → graph (decision 8)

`BuildGraphFromMaterial(const json& vmatFields, const MaterialShaderInterface&) → NodeGraph`:
for a material with no embedded `"_editor"` block, synthesize a default graph — a
`MaterialOutput`, a `TextureSample` per `texture` field (its `Texture` property set to the
field's id), a `Param` per value field — laid out left-to-right. So `brick.vmat.json` opens
without manual migration; the first save embeds the graph.

## Tests (`tests/unit` / `tests/cooker`, device-free)

- `BuildGraphFromMaterial` on the brick fields → a graph with the expected nodes/links;
  `CompileMaterialGraph` of that graph reproduces an equivalent `fields` array
  (import→compile round-trip is stable).
- Coercion: a `Param`(f32) into a `vec4` output pin connects and compiles to a splatted value.
- Compile of a graph with an unconnected output field omits it.
- An incompatible connection (e.g. a texture color into a scalar-only field) is rejected by
  `CanConnect`.

## Acceptance

The material catalog, coercion predicate, `CompileMaterialGraph`, and
`BuildGraphFromMaterial` build under `editor/src/material/`; the round-trip and coercion unit
tests are green device-free; the compiled output matches the `MaterialImporter` schema (no
importer change); `include_hygiene` green; smoke PPM unchanged. Commit:
`Plan 03: material node catalog + graph→.vmat compile + flat-material import`.
</content>
