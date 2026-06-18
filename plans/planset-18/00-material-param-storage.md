# Plan 00 — unified material parameter storage

**Goal:** delete the fixed engine `MaterialData` block. A material's GPU parameters become
**one reflection-sized block** holding both its bindless handle slots and its authored
scalar/vector params, byte-addressed at each field's reflected offset. A material gains an
arbitrary number of handle fields (zero, one, five) instead of being forced into a fixed
16-byte, two-slot, albedo-shaped struct. This is the storage foundation the rest of the
planset stands on: the PostProcess input handle (plan 04) and the runtime-bound handle idiom
(plan 05) both need a material's handle set to be shader-defined, not a fixed engine struct.

## Why the engine block has to go

A material's GPU parameters are two parallel SSBO entries today: a **fixed 16-byte engine
block** (`Renderer::MaterialData` — `Albedo`, `AlbedoSampler`, `Pad0`, `Pad1`; set 0 binding
3) and a variable-size **authored block** (`MaterialParams`; set 0 binding 4, stride 256).
The split is justified in
[BindlessRegistry.h](../../engine/include/Veng/Renderer/BindlessRegistry.h) by *"libveng
knows this block's layout without reflection."*

That justification does not hold. Every site that touches the engine block patches it
**purely by the cooked field offset**, never by a named member:

- `Material::Finalize` writes the resolved bindless index with
  `memcpy(reinterpret_cast<std::byte*>(&m_Params) + field.Offset, &index, sizeof(u32))`
  ([Material.cpp:71](../../engine/src/Asset/Material.cpp)).
- `Material::SetTexture` does the same at `field->Offset`
  ([Material.cpp:123](../../engine/src/Asset/Material.cpp)).
- A grep for `.Albedo` / `.AlbedoSampler` / `.Pad0` / `.Pad1` on a `MaterialData` value
  across `engine/` returns **zero reads**. (`SceneRenderer.cpp`'s `.AlbedoHandle` is the
  g-buffer `PassIO` / lighting push block — a different struct.)

So libveng already drives the block from the cooked offsets, which **are** the reflection
output, serialized into `CookedMaterialField`. The named struct buys nothing — it only
imposes a cost: the loader pins `EngineBytes == sizeof(MaterialData) == 16`
([MaterialLoader.cpp:175](../../engine/src/Asset/Loaders/MaterialLoader.cpp)), `m_MaterialBuffer`
has a fixed 16-byte stride, and `m_Params` is a `MaterialData` value — so **every material
must declare exactly a 16-byte handle block**, at most four `u32` slots, conventionally one
albedo pair plus two dead pads. An authored shader's texture needs are arbitrary; a fixed
albedo-shaped engine struct is the wrong shape for a shader-driven material system.

## What lands

### One unified per-material block

`Renderer::MaterialData` and the separate engine-block SSBO (set 0 binding 3,
`m_MaterialBuffer`) are **deleted**. The registry keeps a single per-material buffer — the
existing `m_MaterialParamBuffer` (set 0 binding 4), `MaxMaterials * MaterialParamStride` —
now holding a material's **whole** block. A material's fields, handles and params alike, lay
out in one struct at `idx * MaterialParamStride`:

- **Handle fields** (`Kind = TextureHandle` / `SamplerHandle`) are `uint` members the loader
  patches in place with the resolved bindless index, at the field's reflected offset.
- **Param fields** (`Kind = Param`) are the scalar/vector members the cooker bakes.

`CookedMaterialField::Kind` (the handle-vs-param seam) is unchanged — it still tells the
loader which fields to patch with a bindless index versus leave as the cooked value. What
goes away is the *second buffer* and the *fixed struct*: there is one block, one binding, one
reflected layout per material, sized by reflection and capped at `MaterialParamStride`.

### The shader declares one struct

A material fragment shader declares a single combined block instead of a fixed `MaterialData`
plus its own `MaterialParams`. Handle slots are `uint` members beside the authored params:

```hlsl
// brick.frag (illustrative): handles + params in one block
struct MaterialParams
{
    uint   Albedo;        // texture handle  (Kind = TextureHandle)
    uint   AlbedoSampler; // sampler handle  (Kind = SamplerHandle)
    float4 Factors;       // authored param  (Kind = Param)
};
```

The shader byte-loads the block from the one material buffer (binding 4) at
`idx * MaterialParamStride` and reads handles through `g_Textures[params.Albedo]` /
`g_Samplers[params.AlbedoSampler]` exactly as before — the indices are the same bindless
slots, just stored in the same block as the params rather than a separate SSBO. The set-0
`StructuredBuffer<MaterialData>` at binding 3 is removed from the layout and from every
material shader.

### The cooked format collapses to one block

[CookedBlobs.h](../../assetpack/include/Veng/Asset/CookedBlobs.h) `CookedMaterialHeader`
loses the two-block split and gains a **version field** (the format guard it lacks today —
unlike `CookedPrefabHeader`, a stale material blob is currently misread silently):

```cpp
struct CookedMaterialHeader
{
    u64 VertexShaderId   = 0;
    u64 FragmentShaderId = 0;
    u32 Version          = 0;  // CookedMaterialVersion; loader asserts on mismatch
    u32 FieldCount       = 0;
    u32 BlockBytes       = 0;  // the single param block size, <= MaterialParamStride
};
```

`EngineBytes` / the `EngineBytes == sizeof(MaterialData)` drift guard are removed; the loader
validates `BlockBytes <= MaterialParamStride` and asserts `Version == CookedMaterialVersion`
(a stale blob is now a loud reject, not silent corruption). `MaterialImporter` reflects the
single combined struct, lays out one block image, and emits one `CookedMaterialField` per
declared field with its offset within that block. The `.vmat.json` `fields` array is
unchanged — `type` still picks the field kind (`texture`/`sampler` → handle, scalar/vector →
param); the cooker just maps all of them into one reflected struct instead of two.

### `Material` holds one block

`Material::m_Params` (the `MaterialData` value) is removed; the material holds one
`vector<u8>` block. `Finalize` and `SetTexture` patch handle indices into that block at
`field.Offset` (the offset bound now lives against `BlockBytes`, not `sizeof(MaterialData)`).
`SetParam` writes param bytes into the same block. `BindlessRegistry::RegisterMaterial`/
`UpdateMaterial` take one `std::span<const std::byte>` block instead of `(engine, authored)`.

## Decisions

1. **One block, not two.** The two-buffer split exists only to serve the fixed engine
   struct, which serves nothing (every patch is offset-driven). Folding to one
   reflection-sized block per material removes the fixed-capacity / fixed-naming constraint
   and is a net simplification — fewer bindings, one upload path, one offset space. A
   material with no textures declares no handle fields; one with five declares five.

2. **The handle/param seam stays at `CookedMaterialField::Kind`.** The loader still needs to
   know which `uint` members are bindless handles (patch with a registered index) versus
   plain `uint` params (keep the cooked value). That distinction is the cooked field kind,
   driven by the `.vmat` `type` and cross-checked against the reflected member — exactly as
   today. Only the storage merges, not the cook-time validation.

3. **Add `CookedMaterialVersion` now.** The header gains the version field this change
   forces anyway (the layout breaks), so a stale blob rejects loudly. `CookedPrefabHeader`
   already sets this precedent. Plan 02 (domain) bumps the version again when it inserts
   `Domain`; the guard covers both format changes.

4. **Every material blob re-cooks.** The format change is binary-incompatible by design; the
   `Version` guard catches any stale blob. The example pack, the cooker fixtures, and (once
   plan 05 adds it) the core `tonemap.vmat` all re-cook under the build-wired cook. There is
   no in-place migration — re-cook is the migration.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/BindlessRegistry.h` | Delete `MaterialData` + `MaterialBinding` (binding 3) + `m_MaterialBuffer`; `RegisterMaterial`/`UpdateMaterial` take one block span; doc the unified block. |
| `engine/src/Renderer/Backend/BindlessRegistry.cpp` | Drop the engine-block buffer + its set-0 write; one upload path into the single material buffer. |
| `engine/include/Veng/Asset/Material.h` | `m_Params` (`MaterialData`) → one `vector<u8>` block; `RegisterMaterial`/`SetTexture`/`SetParam` signatures follow. |
| `engine/src/Asset/Material.cpp` | Patch handles + params into the one block at `field.Offset`; bounds against `BlockBytes`/`MaterialParamStride`. |
| `assetpack/include/Veng/Asset/CookedBlobs.h` | `CookedMaterialHeader`: add `Version`, drop `EngineBytes`, rename to single `BlockBytes`; `CookedMaterialVersion` constant. |
| `engine/src/Asset/Loaders/MaterialLoader.cpp` | Assert `Version`; validate `BlockBytes <= MaterialParamStride`; build one block; drop the `sizeof(MaterialData)` guard; patch by offset+kind into one block. |
| `cooker/src/Importers/MaterialImporter.cpp` | Reflect one combined struct; lay out one block; emit one `CookedMaterialField` per field with its in-block offset; write `Version`/`BlockBytes`. |
| `examples/hello-triangle/assets/shaders/{material_data.slang,brick.frag.slang}` | Fold the fixed `MaterialData` struct into the one declared block (handles as `uint` members beside params); drop the binding-3 `StructuredBuffer`. |
| `tests/cooker/fixtures/materials/*`, `tests/cooker/*`, `tests/unit/*` | Re-cook fixtures under the new format; assert the version guard rejects a stale blob; cover a 0-handle and a multi-handle material. |

## Verification

- Clean build; `libveng`, `vengc`, the example, and the cooker compile with the unified
  block and no `MaterialData` symbol.
- `ctest` cooker band: a 0-texture material and a multi-texture (>1) material both cook,
  proving handle count is shader-driven; a blob with a wrong `Version` is rejected with a
  loud message.
- `gpu` band + `smoke_golden`: the example's brick material renders identically — the same
  albedo sampled through the same bindless slot, now stored in one block. No pixel moves
  (the fold is a storage change, not a shading change), so the golden is unchanged.
- `validation_gate` (under `build-debug`) clean — set 0 has one fewer binding; no descriptor
  is left dangling.
