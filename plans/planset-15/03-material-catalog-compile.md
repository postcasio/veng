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
| `MaterialOutput` | one pin per **authored param field** + one per **texture field** | — | — |
| `TextureSample` | `UV` (vec2, optional) | `Color` (vec4) | `Texture` : `AssetHandle<Texture>` |
| `Param` (constant) | — | `Value` (typed) | `Value` : the leaf (f32/vec2/vec3/vec4/uint) |

### Where the field schema comes from

The `MaterialOutput` node's input pins are derived from the **loaded cooked `Material`'s
reflected field table** — `Material::GetFields()` (added in plan 00). This is the concrete
source for a material's parameter schema: it is exactly the set `MaterialImporter` validated
and cooked, it carries each field's `Name`/`Offset`/`Size`/`Kind`, and it is reachable from
`libveng_editor` **without** re-reflecting Slang — so `libveng_cook` stays out of the editor
framework library. The panel loads the material (it already does, for the preview) and passes
its field table in:

```cpp
// editor/src/material/MaterialShaderInterface.h
struct MaterialShaderInterface
{
    std::span<const Veng::MaterialField> Fields;   // == Material::GetFields()
    Veng::AssetId VertexShader;
    Veng::AssetId FragmentShader;
};
```

Pin derivation walks `Fields` by `Kind`:

- **`Param` field** (`FieldKind::Param` — an authored scalar/vector/uint) → one `MaterialOutput`
  input pin typed to the field, fed by a `Param.Value` (or a math node).
- **`TextureHandle` field** (`FieldKind::TextureHandle`) → one `MaterialOutput` input pin a
  `TextureSample.Color` feeds.
- **`SamplerHandle` field** (`FieldKind::SamplerHandle`) → **no pin.** A sampler is paired to its
  texture by name (`<Texture>Sampler`) and is emitted implicitly by compile when the texture
  pin is connected; it is never authored independently.

A field the shader declares but the `.vmat` does not (so it is absent from `GetFields()`) gets
no pin in v1 — the editor edits the material's declared parameter set, matching decision 9
(bind params to the author's shader). Exposing undeclared shader fields would need the
reflected `MaterialParams` struct via a cook-side reflection request; that is out of scope.

`TextureSample.Texture` is a **property, not a wired pin** (decision 6): an
`AssetHandle<Texture>` field drawn by the asset picker (built in plan 05), compiled to a
`texture` field of the `.vmat`.

## Type compatibility — the coercion predicate

The `CanConnectFn` the material catalog supplies to `NodeGraph`. Beyond exact-`TypeId`
equality it permits the conventional scalar/vector coercions: `f32`→`vecN` (splat),
`vec4`→`vec3`/`vec2` (truncate). This is the one place coercion lives; Layer 1 stays a pure
DAG enforcer. Coercion is recorded on the link's compile so the emitter knows to splat /
truncate.

## Compile — graph → `.vmat` field list

The compile returns a **typed field list**, not a JSON value — keeping `nlohmann::json` out of
the Layer-3 header (the JSON is assembled in the `.cpp` and embedded by the panel):

```cpp
// editor/src/material/MaterialCompile.h
struct CompiledField                        // mirrors one .vmat "fields" entry
{
    Veng::string Name;
    Veng::string Type;                      // "texture" | "sampler" | "float" | "vec2..4" | "uint"
    Veng::u64 TextureId = 0;                // texture: the AssetId
    Veng::string SamplerTexture;            // sampler: the paired texture field's name
    Veng::vector<Veng::f32> Values;         // float/vecN: the components
    Veng::u32 UintValue = 0;                // uint
};

Veng::Result<Veng::vector<CompiledField>> CompileMaterialGraph(
    const VengEditor::NodeGraph&, const VengEditor::NodeCatalog&,
    const MaterialShaderInterface& shader);
```

Walk `TopoOrder` from the `MaterialOutput` node's connected inputs:

- A **texture** output-pin fed by a `TextureSample` → a `{Type:"texture", TextureId:<property's
  AssetId>}` field **and** an implicit paired `{Type:"sampler", SamplerTexture:<that field's
  name>}` field — matching the `"texture":"Albedo"` back-reference the importer expects
  (`brick.vmat.json`).
- A **param** field fed by a `Param` → a `{Type:"float"/"vecN"/"uint", Values/UintValue:…}`
  field, applying any recorded coercion (splat/truncate).
- An **unconnected** field → omitted; the importer's schema tolerance keeps its default.

The `.cpp` serializes `vector<CompiledField>` to the `.vmat`'s `fields` JSON array (the
`shaders` block carried unchanged from source); the panel patches it in and recooks. The
result is exactly the schema `MaterialImporter` cooks — **no importer change** — which a
test asserts by cooking the emitted JSON through the real importer (see Tests).

## Inverse — flat `.vmat` → graph (decision 8)

`BuildGraphFromMaterial(const MaterialShaderInterface&) → NodeGraph`: for a material with no
embedded `"_editor"` block, synthesize a default graph from the loaded material's field table
(`GetFields()`) — a `MaterialOutput`, a `TextureSample` per `texture` field (its `Texture`
property set to the field's `TextureId`), a `Param` per param field (sampler fields consumed by
their paired texture, never their own node) — laid out left-to-right. So `brick.vmat.json`
opens without manual migration. Opening does **not** rewrite `fields`; the first **explicit
edit + save** embeds the graph (decision 8). A round-trip identity check
(`CompileMaterialGraph(BuildGraphFromMaterial(iface))` vs. the source `fields`) guards against
a lossy synthesis silently rewriting a hand-authored material.

## Tests (`veng_editor_unit`, device-free; one cook-through in `tests/cooker`)

The device-free tests run in `veng_editor_unit` (set up in plan 01). They construct a
`MaterialShaderInterface` from a hand-built `vector<MaterialField>` rather than a loaded
`Material`, so they need no device — `BuildGraphFromMaterial` / `CompileMaterialGraph` take the
interface, not a GPU resource.

- `BuildGraphFromMaterial` on the brick field table → a graph with the expected nodes/links;
  `CompileMaterialGraph` of that graph reproduces an equivalent field list (import→compile
  round-trip is stable, the identity check above).
- **Cook-through (`tests/cooker`):** serialize the compiled field list into a `.vmat` and cook
  it through the real `MaterialImporter` — asserts the editor's emitted JSON (texture+sampler
  pairing, `uint`, vecN) is exactly what the importer accepts, not merely "an equivalent array".
  This test calls `CompileMaterialGraph` (editor) and the importer (cooker), so it lives in the
  cooker suite with `veng_editor::veng_editor` additionally linked in (plan 01's test-target
  note); the suite already runs with Slang present for the real cook.
- Coercion: a `Param`(f32) into a `vec4` output pin connects and compiles to a splatted value.
- Compile of a graph with an unconnected output field omits it.
- An incompatible connection (e.g. a texture color into a scalar-only field) is rejected by
  `CanConnect`.

## Acceptance

The material catalog, coercion predicate, `CompileMaterialGraph` (returning typed
`CompiledField`s, no `nlohmann::json` in the header), and `BuildGraphFromMaterial` build under
`editor/src/material/`; the round-trip, coercion, and cook-through tests are green device-free;
the compiled output cooks unchanged through `MaterialImporter` (no importer change);
`include_hygiene` green; smoke PPM unchanged. Commit:
`Plan 03: material node catalog + graph→.vmat compile + flat-material import`.
</content>
