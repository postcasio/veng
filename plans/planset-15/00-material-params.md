# Plan 00 — variable-size material params: engine block + authored block

**Goal:** give a material two parallel GPU parameter blocks — a fixed **engine-supplied**
block and a variable-size **authored** block — so a material can declare custom uniforms
beyond today's single `vec4 Factors`. This is the precursor the node editor's compile (03)
and panel (05) build on: a node graph authors parameters, and a fixed 32-byte `MaterialData`
has nowhere to put them. It depends **only on the engine + cooker + shaders** — no node graph
— so it runs in **Wave 1** alongside plans 01 and 04, on disjoint files.

## The problem

Today `MaterialData` is a fixed 32-byte struct, mirrored byte-for-byte in C++
(`Renderer::MaterialData`, `BindlessRegistry.h`) and Slang (`material_data.slang`), pinned by
`static_assert(sizeof(MaterialData) == 32)` and a loader guard
(`header.ParamBytes != sizeof(MaterialData)` → `Corrupt`). Its only authored value is the one
`vec4 Factors`; the rest is the albedo texture/sampler handle slots. Adding any new authored
uniform (`float Roughness`, `vec3 Tint`) is an **engine-source** change — there is no
data-driven per-material uniform set. A material editor that authors parameters needs one.

## The split — engine block + authored block

A material's parameters split into two SSBO entries, **both indexed by the material's
`MaterialHandle.Index`**, along the seam that already exists in `CookedMaterialField::Kind`
(handle fields vs. param-value fields):

| | Engine block (`MaterialData`) | Authored block (`MaterialParams`) |
|---|---|---|
| **Owner** | the engine writes it | the cook/author writes it |
| **Contents** | bindless texture/sampler handle slots (`Kind 1/2`) the loader patches | scalar/vector uniforms the shader declares (`Kind 0`) |
| **Size** | fixed `sizeof`, `static_assert`-pinned; `libveng` knows it without reflection | variable, reflected per-shader, `≤ MaterialParamStride` |
| **Binding** | set 0, binding 3, `StructuredBuffer<MaterialData>`, fixed stride | set 0, binding 4, `ByteAddressBuffer`, fixed byte stride |
| **Shader read** | `g_Materials[MaterialIndex]` | `g_MaterialParams.Load<MaterialParams>(MaterialIndex * MaterialParamStride)` |

`Renderer::MaterialData` is the engine block — **handle slots only.** `Factors` leaves the
engine entirely: it is **not** an engine concept, just the brick example's demo tint, so it
moves out of this struct and becomes an authored parameter the *example's* shader declares (see
Shaders). The engine ships no value params here:

```cpp
// BindlessRegistry.h — the engine-supplied block: bindless handle slots the loader patches.
struct MaterialData
{
    u32 Albedo = 0;        // bindless sampled-image index (loader-patched)
    u32 AlbedoSampler = 0; // bindless sampler index (loader-patched)
    u32 Pad0 = 0;
    u32 Pad1 = 0;
};
static_assert(sizeof(MaterialData) == 16,
    "MaterialData is the engine-supplied block — handle slots; std430 16-byte stride");
```

**`MaterialParams` is not an engine type.** `libveng` has no C++ mirror of the authored block —
it stores it as opaque bytes (`vector<std::byte>`, size `ParamBytes`) and never names a field
in it. `MaterialParams` (and `Factors`) live **only** in the author's Slang (`material_data.slang`
in the example + test fixtures) and the `.vmat` field declarations; a different material declares
a different `MaterialParams`. The engine's sole contribution to the authored block is the ABI
constant `MaterialParamStride` (the buffer stride), not its contents.

**Why a `ByteAddressBuffer` for the authored block, not a second `StructuredBuffer`.** A
`StructuredBuffer<MaterialParams>` has stride `sizeof(MaterialParams)`, which differs per
shader — but one buffer is shared across every material, so it needs one uniform stride. A
`ByteAddressBuffer` addressed at `MaterialIndex * MaterialParamStride` with `Load<T>` lets
each shader read its own `MaterialParams` layout from a shared, uniformly-strided buffer.
`MaterialParamStride` is an engine constant (256 bytes, 16-byte aligned for vector loads),
shared in `material_data.slang` and mirrored as `BindlessRegistry::MaterialParamStride`.
Bounded waste: `256 × MaxMaterials (256) = 64 KiB`.

A shader that declares no `MaterialParams` struct (a handles-only material) gets a zero-size
authored block; its buffer slot is still allocated, the `Load` is never reached.

## Cooker — reflect both structs, route fields by block

`MaterialImporter` + `SlangReflect`:

- Reflect **both** `MaterialData` (engine, required) and `MaterialParams` (authored,
  **optional** — a missing struct reflects as empty) from the fragment shader's `.slang`.
  `ReflectStructLayout` already reflects any named struct; add a tolerant "struct not found →
  empty `ReflectedStruct`" return for `MaterialParams` (distinct from a compile error).
- Route each declared `.vmat` field by its `type`: `texture` / `sampler` validate against and
  patch the **engine** block; `float` / `vec2` / `vec3` / `vec4` / `uint` validate against and
  pack into the **authored** block. The field's `Offset` is within its own block.
- **Emit one `CookedMaterialField` per *declared* `.vmat` field, not per reflected struct
  member.** Today the importer walks the reflected `MaterialData` layout and emits an entry for
  every member, so the engine struct's `Pad0`/`Pad1` land in the table as `Kind 0` (param)
  fields. Under the split that is wrong twice over: a `Kind 0` entry must mean "authored-block
  param," and `Material::GetFields()` (the editor's parameter schema, plan 03) would surface the
  pads as authored params — the `MaterialOutput` node would then sprout spurious `Pad0`/`Pad1`
  pins. So the field table carries only declared fields: declared handle fields (`Kind 1/2`)
  matched against the engine block, declared param fields (`Kind 0`) matched against the authored
  block. Undeclared engine-block members (the pads) are validated/zeroed in the block image but
  **not** emitted as field entries.
- With pads gone from the table, a `CookedMaterialField`'s block is implied by its `Kind`
  (handle fields → engine block, param fields → authored block), so the field table needs no new
  column and every `Kind 0` field is genuinely an authored param.
- The authored block's reflected size exceeding `MaterialParamStride` is a located cook error
  (`material importer: authored params block N bytes exceeds stride 256`).

`CookedMaterialHeader` carries both sizes:

```cpp
struct CookedMaterialHeader
{
    u64 VertexShaderId = 0;
    u64 FragmentShaderId = 0;
    u32 FieldCount = 0;
    u32 EngineBytes = 0;   // == sizeof(engine MaterialData mirror); the drift guard
    u32 ParamBytes = 0;    // authored block bytes (0 for a handles-only material), <= stride
};
```

Blob order becomes: header, `CookedMaterialField[]`, engine block (`EngineBytes`), authored
block (`ParamBytes`). The format change is contained to the material blob; every pack
re-cooks from source at build, so there is no on-disk migration (the engine ships no prebuilt
packs). brick re-cooks; its render is byte-identical (`albedo * Factors` is unchanged), so the
smoke golden is unchanged.

## Engine — second SSBO, two-block register/update

`BindlessRegistry`:

- Add binding 4 to the set-0 layout: a storage buffer of `MaxMaterials * MaterialParamStride`
  bytes, written into set 0 at construction (a `ByteAddressBuffer` on the shader side; a plain
  storage `Buffer` here).
- `RegisterMaterial` / `UpdateMaterial` take **two** spans — the engine block
  (`== sizeof(MaterialData)`) and the authored block (`<= MaterialParamStride`). The engine
  block uploads at `index * sizeof(MaterialData)` into binding 3's buffer; the authored block
  at `index * MaterialParamStride` into binding 4's buffer.
- Add `static constexpr u32 MaterialParamBinding = 4;` and
  `static constexpr u32 MaterialParamStride = 256;`.

The set-0 layout is reserved in every `PipelineLayout` via `GetSet0Layout()`, so author-
declared sets already shift to 1+ regardless of binding count — adding binding 4 is internal
to the registry. Confirm nothing hardcodes the set-0 binding count outside the registry.

## Loader — read two blocks, patch handles into the engine block

`MaterialLoader`:

- The drift guard moves to the engine block: `header.EngineBytes != sizeof(Renderer::MaterialData)`
  → `Corrupt`. Add `header.ParamBytes > BindlessRegistry::MaterialParamStride` → `Corrupt`.
- memcpy the engine block into a `Renderer::MaterialData params{}`; copy the authored block
  into a `vector<std::byte>` sized to `ParamBytes`.
- Patch handle fields (`Kind 1/2`) into the engine block by offset (bounds vs.
  `sizeof(MaterialData)`), exactly as today. Param fields are already packed into the authored
  block at cook.
- Pass both blocks into `MaterialInfo`.

## Material — two blocks; expose the field table

`Material` (`Material.h` / `.cpp`):

- Hold `m_Params` (the engine block, `Renderer::MaterialData`) **and** `m_AuthoredParams`
  (`vector<std::byte>`, sized to the authored block).
- `Finalize` patches handles into `m_Params` and registers **both** blocks; `UploadParams`
  re-uploads both.
- `SetTexture` writes a handle offset in the **engine** block; `SetParam` writes a param
  offset in the **authored** block (bounds vs. the authored size, not `sizeof(MaterialData)`).
- **Add `[[nodiscard]] std::span<const MaterialField> GetFields() const`** — the reflected
  field table the material already stores. This is the editor's source for a material's
  parameter schema (consumed by plans 03/05), so the editor never re-reflects Slang and
  `libveng_cook` stays out of `libveng_editor`. Each `MaterialField` already carries
  `Name`/`Offset`/`Size`/`Kind`/`TextureId`; `Kind` tells the editor whether a field is a
  texture/sampler (engine block) or an authored param.

## Shaders (example + test assets, not engine)

`material_data.slang` is an **example/test asset**, not engine code. The engine block struct
`MaterialData` mirrors `Renderer::MaterialData` (handles only); `MaterialParams` is the
author's own — here, the brick example keeps `Factors` as its authored param. Split the file
into the two structs and bind both buffers:

```slang
struct MaterialData   { uint Albedo; uint AlbedoSampler; uint Pad0; uint Pad1; };  // mirrors the engine block
struct MaterialParams { float4 Factors; };                                          // brick's authored params

static const uint MaterialParamStride = 256;   // == BindlessRegistry::MaterialParamStride

[[vk::binding(3, 0)]] StructuredBuffer<MaterialData> g_Materials;
[[vk::binding(4, 0)]] ByteAddressBuffer             g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}
```

`brick.frag.slang` reads both — handles from the engine block, `Factors` from its authored
block:

```slang
MaterialData   m = g_Materials[g_PC.MaterialIndex];
MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
float4 albedo = g_Textures[NonUniformResourceIndex(m.Albedo)].Sample(
    g_Samplers[NonUniformResourceIndex(m.AlbedoSampler)], input.v_UV);
output.Albedo = albedo * p.Factors;
```

Update all three `material_data.slang` copies: `examples/hello-triangle/assets/shaders/`,
`tests/gpu/assets/shaders/`, `tests/cooker/fixtures/shaders/`. The `.vmat` sources are
**unchanged** — `brick.vmat.json` still declares `Factors` as a `vec4` field; it now cooks into
the authored block instead of the engine block. So `Factors` is removed from the engine yet
stays an authored parameter of the example material, end to end.

## Tests (`tests/cooker`, device-free; `tests/gpu` for render)

- **Authored param beyond `Factors`:** cook a material whose fragment shader declares
  `MaterialParams { float4 Factors; float Roughness; }`, set both in the `.vmat`; assert the
  cooked authored block carries `Roughness` at its reflected offset and `ParamBytes` matches
  the larger struct.
- **Handles-only material:** a shader with no `MaterialParams` struct → `ParamBytes == 0`, the
  field table has only handle fields, load succeeds.
- **Over-stride:** an authored block whose reflected size exceeds `MaterialParamStride` → a
  located cook error.
- **Drift guard:** an engine block whose `EngineBytes != sizeof(MaterialData)` → `Corrupt` at
  load.
- **Existing cooker test updated:** `tests/cooker/material_cook.cpp` hardcodes the old
  single-block layout (5 reflected fields incl. `Pad0`/`Pad1` as `Kind 0`, `Factors` at offset
  16, one 32-byte `ParamBytes`). Re-point it at the two-block layout: the table now holds the
  **3 declared fields** — `Albedo` (`Kind 1`, engine offset 0), `AlbedoSampler` (`Kind 2`, engine
  offset 4), `Factors` (`Kind 0`, authored offset 0) — with `Pad0`/`Pad1` **no longer emitted as
  field entries**, `EngineBytes == 16`, and `ParamBytes == 16` (a single `vec4`). Both sizes
  asserted.
- **Render unchanged:** the `gpu` brick render and the `smoke_golden` capture are byte-stable
  (brick's `Factors` are `[1,1,1,1]`, so `albedo * Factors == albedo`).

## Acceptance

brick re-cooks and renders identically (golden unchanged); a material can declare a custom
authored uniform beyond `Factors`, cooked into the variable authored block; the engine block
stays fixed and `static_assert`/`Corrupt`-guarded; the authored block is variable and
stride-bounded; `Material::GetFields()` exposes the reflected field table; `ctest` green;
validation gate clean; smoke PPM unchanged. Commit:
`Plan 00: variable-size material params — engine-supplied block + authored block split`.
