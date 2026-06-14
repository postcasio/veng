# Plan 08 — Material: JSON asset, inline/external shader, engine `Material`

**Goal:** the headline. A JSON **material asset** references a shader (external
Slang path **or** inline base64 SPIR-V), a set of textures (by `AssetId`), and
scalar params. The cooker resolves the shader, validates params/textures against
the reflected `ShaderInterface`, and writes a `CookedMaterialHeader` blob. The
engine registers a `Material` loader that loads the shader + textures, builds its
pipeline/descriptor layouts from reflection, binds its resources, and exposes
`Material::Bind(cmd)` — the new draw interface. **Pre-bindless**: binding goes
through today's per-set `DescriptorSet`; the bindless backing replaces it later.

## Why this is its own plan

Material is the payoff and the most coupled type — it pulls together the shader
(07), textures (05), reflection-driven layouts, and param validation. Keeping it
last and alone means everything it needs is already proven, and the binding model
(per-set now, bindless later) is a contained decision.

## The material JSON

```jsonc
// materials/brick.vmat.json
{
  "shader": { "path": "shaders/brick.slang" },   // OR { "spirv_b64": "…" } (editor/inline)
  "textures": { "albedo": 1001, "normal": 1005 }, // name → AssetId (names resolved via reflection)
  "params":   { "tint": [1.0, 0.9, 0.8, 1.0], "roughness": 0.4 }
}
```

- **Shader reference, two forms** — `path` (the cooker compiles it via the plan 07
  Slang path) or `spirv_b64` (precompiled, base64-decoded then reflected via
  SPIRV-Reflect). Inline is how editor-produced materials carry their compiled
  shader without a separate file. Either way the cooker produces/embeds a shader by
  `AssetId`: a `path` shader is cooked as its own shader asset and referenced by id;
  an inline shader is cooked into a shader blob and either embedded or assigned a
  synthetic id within the pack (decide in implementation — **lean: always materialize
  the shader as a normal shader asset with its own id**, so the loader path is
  uniform and `Material` always references a `ShaderId`).
- **`textures` / `params` validated against the reflected interface at cook time** —
  an unknown texture name, a missing required binding, or a wrong-typed param fails
  the **cook** with a located error, not at runtime.

## Cooked layout & engine type

```cpp
struct CookedMaterialHeader { u64 ShaderId; u32 TextureBindingCount; u32 ParamBytes; /* … */ };
// + [TextureBinding { u32 nameHashOrIndex; u64 TextureId }] + packed param block (per interface)

class Material
{
public:
    void Bind(CommandBuffer& cmd) const;   // the new primary draw interface (pre-bindless)
    void SetTexture(string_view name, AssetHandle<Texture>);  // name-based (reflection)
    void SetParam(string_view name, const vec4& value);
};
```

- A `MaterialLoader : AssetLoader`. `Load`: read header → `LoadSync<ShaderAsset>`
  the `ShaderId` (eager dependency) → `LoadSync<Texture>` each bound texture id →
  build `PipelineLayout`/`DescriptorSetLayout` from the shader's `ShaderInterface`
  (plan 07) → write the textures + a uniform/param buffer into a `DescriptorSet`
  (sets ≥ 1; set 0 reserved) → assemble the `Material`.
- **`Bind`** binds the material's pipeline + its descriptor set(s) (today's
  `BindDescriptorSets`). This is the per-set, `O(draws)` model — correct and
  simple; bindless removes the per-draw rebinds later
  ([bindless-descriptors.md](../future/bindless-descriptors.md)). The
  `Material::Bind` *interface* is stable across that change; only its internals
  swap.
- **`MissingDependency`** if a referenced shader/texture id isn't mounted.

## Work

1. Cooker: `MaterialImporter` — parse the material JSON, resolve/compile the shader
   (path → plan 07 shader cook; `spirv_b64` → decode + reflect), validate textures
   + params against the interface, emit the cooked material (+ ensure the shader is
   present as a shader asset by id). Register it.
2. Engine: `Material` type + `MaterialLoader`; reflection-driven layout build
   (reuse plan 07's builder); per-set descriptor write; `Bind`; name-based
   `SetTexture`/`SetParam`.
3. Sample: a material in the pack bound to a mesh; `LoadSync<Material>(id)` (pulls
   shader + textures), bind it, draw the mesh with it.
4. Tests: cook a material with an external shader + a texture; load it; assert the
   shader/texture dependencies resolved, the param block packed per the interface,
   and a deliberately-wrong param/texture name fails the **cook**. A
   `MissingDependency` case for an unmounted texture id.

## Dependencies

Plans 05 (textures), 07 (shader + reflection + layout builder), 04 (manager/loaders),
03 (importer table). The capstone type. Feeds 09.

## Acceptance

- Clean build, `ctest` green incl. the material cook + load tests.
- Smoke binary writes a correct-sized PPM with a mesh rendered through a cooked
  material (shader + texture + params all from the pack).
- **Validation-clean** under `VE_DEBUG` for the material's pipeline + descriptor
  binding (do not widen the known descriptor-pool gap).

## Notes

- **Pre-bindless is a deliberate scope line.** The material doc's "thin material =
  handles + an SSBO entry" is the bindless end-state; here a material owns a real
  pipeline + descriptor set. The `Bind` interface is chosen so the bindless rework
  is an internal swap, not an API break.
- **Param packing** follows the reflected uniform-block layout (std140/std430 as the
  shader declares) — computed at cook time from the interface, so the runtime just
  uploads bytes.
- Decide the inline-shader id strategy in implementation; the recommendation
  (materialize every shader as a normal asset with an id) keeps one loader path and
  lets two materials share a shader by id.
