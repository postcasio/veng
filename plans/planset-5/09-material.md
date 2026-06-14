# Plan 09 — Material: JSON asset, inline/external shader, bindless `Material`

**Goal:** the headline, on bindless. A JSON **material asset** references a shader
(external Slang path **or** inline base64 SPIR-V), a set of textures (by `AssetId`),
and scalar params. The cooker resolves the shader, validates params/textures
against the reflected `ShaderInterface`, and writes a `CookedMaterialHeader` blob.
The engine `Material` loader loads the shader + textures and assembles the
**thin bindless material**: shader handle + texture **handles** + a `MaterialData`
entry in a per-material SSBO array. `Material::Bind(cmd)` writes the material's
index for the per-draw selector — it does **not** swap descriptor sets per draw
(set 0 is bound once per frame by the registry).

## Why this is its own plan

Material is the payoff and the most coupled type — it pulls together the shader
(08), textures (06), the bindless registry (05), reflection-driven layouts, and
param validation. Keeping it last and alone means everything it needs is already
proven, and it lands as the end-state model (handles + an SSBO entry), not a per-set
stopgap.

## The material JSON

```jsonc
// materials/brick.vmat.json
{
  "shader": { "path": "shaders/brick.slang" },   // OR { "spirv_b64": "…" } (editor/inline)
  "textures": { "albedo": 1001, "normal": 1005 }, // name → AssetId (names resolved via reflection)
  "params":   { "tint": [1.0, 0.9, 0.8, 1.0], "roughness": 0.4 }
}
```

- **Shader reference, two forms** — `path` (the cooker compiles it via the plan 08
  Slang path) or `spirv_b64` (precompiled, base64-decoded then reflected via
  SPIRV-Reflect). Inline is how editor-produced materials carry their compiled
  shader without a separate file. Either way the cooker **materializes the shader as
  a normal shader asset with its own `AssetId`** (synthetic for inline) so the
  loader path is uniform and two materials can share a shader by id.
- **`textures` / `params` validated against the reflected interface at cook time** —
  an unknown texture name, a missing required binding, or a wrong-typed param fails
  the **cook** with a located error, not at runtime.

## The bindless data layout (this plan's render-side contribution)

Building on plan 05's image/sampler arrays, the material plan adds the **buffer**
side of the bindless data layout (the table from
[bindless-descriptors.md](../future/bindless-descriptors.md)):

| Data | Lives in | Indexed by | Frequency |
|---|---|---|---|
| `MaterialData { u32 Albedo, Normal, …; u32 Sampler; vec4 Factors; }` | per-material **SSBO array** | material index | per material |
| `DrawData { u32 materialIndex; (objectIndex) }` | **push constant** (~4–8B) | — | per draw |

A material's texture references become `u32` handles (the texture's
`TextureHandle`/`SamplerHandle` from plan 06) packed into its `MaterialData` SSBO
entry. The shader reads `materials[pc.materialIndex]`, then
`texture(textures[nonuniformEXT(m.Albedo)], …)`. (Per-object transforms in an
object SSBO are a renderer concern beyond this planset's sample — the cube uses a
push-constant model matrix or a single object entry; the per-material array is the
piece materials own.)

## Cooked layout & engine type

```cpp
struct CookedMaterialHeader { u64 ShaderId; u32 TextureBindingCount; u32 ParamBytes; /* … */ };
// + [TextureBinding { u32 nameIndex; u64 TextureId }] + packed param block (per interface)

class Material
{
public:
    void Bind(CommandBuffer& cmd) const;   // writes the per-draw material selector; no set swap
    void SetTexture(string_view name, AssetHandle<Texture>);  // name-based (reflection); updates SSBO entry
    void SetParam(string_view name, const vec4& value);       // updates SSBO entry
    [[nodiscard]] u32 GetIndex() const;    // its slot in the per-material SSBO array
};
```

- A `MaterialLoader : AssetLoader`. `Load`: read header → `LoadSync<ShaderAsset>`
  the `ShaderId` (eager dependency) → `LoadSync<Texture>` each bound texture id
  (each already registered into bindless by plan 06) → build the pipeline from the
  shader's reflected layout (set 0 = registry, sets ≥ 1 from reflection) → allocate
  a `MaterialData` slot in the per-material SSBO array and write the texture/sampler
  handles + packed params into it → assemble the `Material`.
- **`Bind`** binds the material's pipeline and pushes its `materialIndex`; set 0 is
  already bound once per frame by `BindlessRegistry::Bind`. No per-draw descriptor
  set swap — the thing bindless removes.
- **`MissingDependency`** if a referenced shader/texture id isn't mounted.

## Work

1. Cooker: `MaterialImporter` — parse the material JSON, resolve/compile the shader
   (path → plan 08 shader cook; `spirv_b64` → decode + reflect), validate textures +
   params against the interface, emit the cooked material + the materialized shader
   asset. Register it.
2. Engine: `Material` type + `MaterialLoader`; the per-material `MaterialData` SSBO
   array + slot allocator (deferred free via the retire queue, like the registry);
   pipeline from reflected layout; `Bind` (push selector); name-based
   `SetTexture`/`SetParam` (rewrite the SSBO entry).
3. Sample: a material in the pack bound to the cube mesh; `LoadSync<Material>(id)`
   (pulls shader + textures), `Bind`, draw the mesh with it through set 0.
4. Tests: cook a material with an external shader + a texture; load it; assert the
   shader/texture dependencies resolved, the `MaterialData` entry holds the right
   handles, the param block is packed per the interface, and a deliberately-wrong
   param/texture name fails the **cook**. A `MissingDependency` case for an
   unmounted texture id.

## Dependencies

Plans 05 (bindless registry + handles), 06 (textures → handles), 08 (shader +
reflection + layout builder), 04 (manager/loaders), 03 (importer table). The
capstone type. Feeds 10.

## Acceptance

- Clean build, `ctest` green incl. the material cook + load tests.
- Smoke binary writes a correct-sized PPM with a mesh rendered through a bindless
  cooked material (shader + texture handles + params all from the pack), set 0
  bound once per frame.
- **Validation-clean** under `VE_DEBUG` for the material's pipeline + the
  per-material SSBO + bindless sampling.

## Notes

- **The material binding model here is not final — veng has no deferred renderer
  yet.** For this phase the example loads a **simple forward-rendering shader
  material** (one pass, a basic lit/unlit shader sampling the albedo), and the
  bindless data layout above is the forward-shaped subset of the
  [bindless doc's table](../future/bindless-descriptors.md#per-draw-data-layout-push-constants-vs-buffers--for-a-deferred-renderer)
  (which is written for a deferred pipeline — G-buffer geometry pass, a lighting
  pass reading it). The per-material SSBO + handle model is right regardless, but
  **what a material *is* (its passes, which params/handles it carries, where the
  per-object data lives) will change with the renderer architecture** when a
  deferred path lands. Treat the `Material`/`MaterialData` shape as v1-forward, not
  the eventual contract — the asset/cook side (JSON, validation, the cooked blob)
  is the durable part; the runtime binding is expected to evolve.
- **The material is thin by construction** — handles + an SSBO entry, not a bundle
  of descriptor sets. This is the bindless end-state, reached directly because
  plan 05 came first.
- **Param packing** follows the reflected uniform/SSBO layout (std140/std430 as the
  shader declares), computed at cook time, so the runtime just uploads bytes.
- **Buffers: arrayed SSBO vs. BDA** (the bindless doc's open call) — decide here for
  the `MaterialData` array. Lean arrayed-SSBO-in-set-0 for v1 (one bound array,
  index by `materialIndex`); BDA is the sharper tool if profiling wants it.
- Inline-shader id strategy: materialize as a normal shader asset (above) — one
  loader path, shareable by id.
