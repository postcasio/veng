# Plan 09b — Shader & material asset data in their own source files

**Goal:** finish the "every asset type has a JSON source file" decision (planset
README, decision 6) for the two types that missed it. Shaders and materials
currently store their authoring data **inline in the pack JSON**; move it into
per-asset `*.shader.json` / `*.vmat.json` files so the pack is a pure
`{ id, type, source }` manifest, symmetric with `texture`/`mesh`. In the same pass,
make the material a **self-describing, explicitly-typed** asset and drop the
precompiled-inline shader path.

## Why this is a correction

Plan 09 landed the material/shader cook but stored their data in the pack entry,
unlike texture/mesh whose entries already point at an external
`*.tex.json` / `*.mesh.json`. Three problems fall out:

1. **Shaders** carry `entry` + `vertex_layout` in the pack entry.
2. **Materials** carry their whole body (`shaders`/`textures`/`params`) in the pack
   entry.
3. **Materials are a bag of data** — `textures`/`params` are bare name→value maps
   with no per-field type and no explicit ordering/index. The field schema is only
   recovered by reflecting the fragment shader's `MaterialData` struct, so the
   material is neither self-describing nor the authority over its own layout.

Today's pack entries (`examples/hello-triangle/assets/sample.vengpack.json`):

```jsonc
{ "id": 1004, "type": "shader", "source": "shaders/brick.vert.slang", "entry": "vsMain", "vertex_layout": 5603155022528551788 },
{ "id": 1005, "type": "shader", "source": "shaders/brick.frag.slang", "entry": "fsMain" },
{ "id": 1003, "type": "material",
  "shaders": { "vertex": 1004, "fragment": 1005 },
  "textures": { "Albedo": 1001 },
  "params":   { "Factors": [1.0, 1.0, 1.0, 1.0] } }
```

## Decisions

- **The material is the source of truth, but the cook still validates it against the
  shader.** Materials will be authored in a material editor that *generates* the
  shader, so `*.vmat.json` declares its fields explicitly (type + order). Hand-written
  Slang shaders remain supported, so the cook **keeps reflecting the fragment
  shader's `MaterialData`** and validates the declared fields against it (names,
  types, offsets) — catching drift between a hand-written shader and the material.
  Reflection supplies the byte offsets; the material supplies the semantic field
  list, types, values, and texture ids.
- **Shaders are always compiled from `source`.** Only the *precompiled* inline path
  (`spirv_b64` + hand-written `interface`) is dropped — the cooker always compiles
  Slang. Hand-written `.slang` source is still fully supported.

## The shader asset — `*.shader.json` (source-only)

Pack entry → `{ "id": 1004, "type": "shader", "source": "shaders/brick.vert.shader.json" }`.

```jsonc
// shaders/brick.vert.shader.json
{
  "source": "brick.vert.slang",          // .slang path, relative to THIS json's dir
  "entry": "vsMain",
  "vertex_layout": 5603155022528551788   // AssetId, optional (omit / 0 = no inputs)
}
```

`ShaderImporter::Cook` reads `entry["source"]` (the `.shader.json`), parses it,
resolves the `.slang` path **relative to the json's directory** (mirroring
`TextureImporter`'s `image` handling), and calls the existing `CookFromSource`.
**Remove** the inline path: `CookInline`, `DecodeBase64`, `ParseDescriptorType`,
`ParseShaderStageMask`, and the top-level `if (entry.contains("spirv_b64"))` branch.
The Slang compile + vertex-input/`vertex_layout` validation in `CookFromSource` is
unchanged.

## The material asset — `*.vmat.json` (explicit, typed, ordered)

Pack entry → `{ "id": 1003, "type": "material", "source": "materials/brick.vmat.json" }`.

```jsonc
// materials/brick.vmat.json
{
  "shaders": { "vertex": 1004, "fragment": 1005 },
  "fields": [
    { "name": "Albedo",        "type": "texture", "id": 1001 },
    { "name": "AlbedoSampler", "type": "sampler", "texture": "Albedo" },
    { "name": "Factors",       "type": "vec4",    "value": [1.0, 1.0, 1.0, 1.0] }
  ]
}
```

- `fields` is an **ordered** array — array position *is* the field's index. Each
  field carries an explicit `type`: `texture`, `sampler`, `float`, `vec2`, `vec3`,
  `vec4`, `uint`.
  - `texture` → `id` (AssetId; validated to exist and be `AssetType::Texture`) → cooked `Kind 1`.
  - `sampler` → `texture` (name of a declared texture field) → cooked `Kind 2`, reusing that texture's id.
  - scalar/vector → `value` (number or number-array, arity validated against the
    type) → cooked `Kind 0`, packed into the param block.
- Authors declare only meaningful fields — **not** `Pad0`/`Pad1`. Padding is supplied
  by the reflected struct, so undeclared reflected fields become `Kind 0` zero in the
  cooked table.

**Validation against the shader (kept):** `MaterialImporter` resolves the fragment
shader id, reads its `*.shader.json` to find the `.slang` path, and reflects
`MaterialData` via the existing `ReflectStructLayout`. It walks the **reflected**
fields in order:
- a declared material field matching by name → validate its `type` against the
  reflected field (`vec4` ↔ 4-component float; `texture`/`sampler` ↔ scalar uint),
  classify its `Kind`, resolve/validate its texture id, pack its value at the
  reflected offset;
- an undeclared reflected field (e.g. a pad) → `Kind 0` zero.

Every declared field must match some reflected field (an unmatched declared
field/typo fails the cook with the field name). Offsets/sizes come from reflection,
so the cooked layout cannot disagree with the shader.

The cooked-blob format (`CookedMaterialHeader` + `CookedMaterialField[]` + param
block, one entry per reflected field) is **unchanged**, so the engine
`MaterialLoader` needs no change and the runtime
`ParamBytes == sizeof(Renderer::MaterialData)` drift guard still holds (32 B).

## Work

1. Cooker — `ShaderImporter`: read the external `.shader.json`; delete the inline
   `spirv_b64`/`interface` path and its helpers. `MaterialImporter`: read the
   external `.vmat.json`; parse `shaders` + ordered typed `fields`; reflect the
   fragment shader's `MaterialData` (read its `.shader.json` to find the `.slang`),
   validate each declared field's `type`, pack at reflected offsets, build the same
   cooked blob. `SlangReflect.{h,cpp}` is **kept**.
2. Example pack — `sample.vengpack.json` entries become `{ id, type, source }`; add
   `shaders/brick.vert.shader.json`, `shaders/brick.frag.shader.json`,
   `materials/brick.vmat.json`. `.slang` files unchanged.
3. Tests — convert `fixtures/{shader,material,shader_mismatch}_pack.json` to the
   external form; remove the inline `spirv_b64` entry (4002) and its `shader_cook.cpp`
   case, replacing it with a source-based fragment shader. `material_cook.cpp`
   expectations are unchanged (`FieldCount == 5` incl. reflected pads, offsets
   0/4/8/12/16, `ParamBytes == 32`) — only the fixture form changes. Repoint
   `material_bad_param.json` / `material_bad_texture.json` to the `.vmat.json` form;
   their meaning (a declared field naming no `MaterialData` field fails the cook) is
   preserved. `vertex_layout_cook.cpp` assertions unchanged.

## Dependencies

Refines plan 09 (material) and plan 08 (shader). No engine-side or cooked-format
change; touches the cooker importers, the example pack, and the cooker test
fixtures only.

## Acceptance

- Clean build, `ctest` green (cooker material/shader/vertex_layout suites).
- Smoke binary writes a correct-sized PPM (1280×720 RGB ≈ 2,764,816 B) with the mesh
  rendered through the cooked material.
- `sample.vengpack.json` is a pure `{ id, type, source }` manifest; the cooked
  archive still contains the material, both shaders, the texture, and the mesh.
- Validation-clean under `VE_DEBUG`.

## Notes

- `material_data.slang` and `Renderer::MaterialData` (BindlessRegistry.h) remain
  hand-written mirrors of the v1 forward layout. The cook's reflection cross-check
  plus the runtime `ParamBytes == sizeof(MaterialData)` guard keep shader, material,
  and engine honest; once the editor generates shaders from materials the match is
  additionally guaranteed by construction.
- Commit as a single fix correcting plan 09 (a `09b:` correction, not a new
  `Plan NN`), with a `Co-Authored-By` trailer.
