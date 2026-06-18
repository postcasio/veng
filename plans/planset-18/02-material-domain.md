# Plan 02 — material domain concept

**Goal:** add a first-class **domain** to a material — the property that selects its
output contract, pipeline shape, standard vertex shader, and invocation site — across the
three layers it lives in: the `.vmat.json` source, the cooked blob format, and the runtime
`Material`. The default is `surface`, so this plan changes no rendered pixel; it adds the
tag and the cook-time contract check the rest of the planset reads.

## What lands

### The `MaterialDomain` vocabulary enum

A closed engine enum beside the other material vocabulary. It belongs with the runtime
material surface, not the renderer vocabulary enums, since it is an asset property:

```cpp
// engine/include/Veng/Asset/Material.h
namespace Veng
{
    // A material's domain selects its output contract, pipeline shape, standard
    // vertex shader, and invocation site. Surface writes the g-buffer and is drawn
    // per submesh by the geometry pass; PostProcess writes a single final color and
    // is invoked fullscreen by the post chain. The rest of the material system —
    // parameter schema, bindless handles, authoring, inspector — is shared across
    // domains.
    enum class MaterialDomain : u32
    {
        Surface = 0,
        PostProcess = 1,
    };
}
```

`MaterialInfo` gains `MaterialDomain Domain = MaterialDomain::Surface;`, `Material` stores
it, and `[[nodiscard]] MaterialDomain GetDomain() const` exposes it. `Surface = 0` so a
zero-initialised / legacy header reads as the existing behavior.

### The cooked-format field

[CookedBlobs.h](../../assetpack/include/Veng/Asset/CookedBlobs.h) `CookedMaterialHeader`
(already reworked by plan 00 into the single-block form with a `Version` field) gains the
domain, stored as its **underlying integer** per the file's cycle-avoidance rule (assetpack
does not include the engine's enum header). `Domain` is inserted after `Version`, and
`CookedMaterialVersion` is **bumped** so a plan-00-era blob without the domain field rejects
loudly rather than misreading:

```cpp
struct CookedMaterialHeader
{
    u64 VertexShaderId   = 0;
    u64 FragmentShaderId = 0;
    u32 Version          = 0;  // CookedMaterialVersion (bumped by this plan)
    u32 Domain           = 0;  // underlying MaterialDomain (0 = Surface)
    u32 FieldCount       = 0;
    u32 BlockBytes       = 0;
};
```

The loader asserts `Version == CookedMaterialVersion`, then casts `Domain` to
`MaterialDomain` guarded by a `VE_ASSERT` on an out-of-range value (the loud
one-line-fix-on-drift pattern the other underlying-int enum fields use).

**Every material blob in the tree is re-cooked** in this plan (the example pack, the core
pack once plan 05 adds the tonemap material to it, and the cooker test fixtures), as it was
under plan 00; the version guard catches any stale blob. The cook is build-wired
(`add_asset_pack`), so a clean build re-cooks; the `gpu`/cooker fixtures are cooked by their
own targets.

### The cook-time parse + contract validation

[MaterialImporter.cpp](../../cooker/src/Importers/MaterialImporter.cpp) reads the optional
lowercase `"domain"` key right after parsing the `.vmat.json` (before the existing shader
resolution at step 2):

- absent → `MaterialDomain::Surface` (the default — every existing material is unaffected);
- `"surface"` / `"postprocess"` → the matching enum;
- any other string, or a non-string value → a located cook error
  (`material importer: '<path>': unknown domain '<x>' (expected "surface" or "postprocess")`).

The importer already reflects the fragment shader's interface to build the material's
parameter-block layout and field table. This plan adds a **domain ↔ fragment-output contract
check** on that reflection, the material analogue of the existing field validation:

- **Surface** — the fragment entry must write the g-buffer MRT: a `float4 SV_Target0`
  (Albedo) and a `float4 SV_Target1` (Normal), matching `GBufferOutput`. A surface
  fragment shader that writes a single target is a located error.
- **PostProcess** — the fragment entry must write a **single** `float4 SV_Target0` and no
  `SV_Target1+`. A postprocess fragment shader that writes the MRT is a located error.

This output-target reflection does **not** exist in
[SlangReflect](../../cooker/src/Importers/SlangReflect.cpp) today — it only implements
`ReflectStructLayout` (struct field layout by name). Reading a fragment entry's render
targets is new reflection work this plan adds: a `ReflectFragmentOutputs(slangPath, entry)`
that walks the entry point's result via Slang's `IEntryPointReflection` —
`getResultVarLayout()` / iterating the result type's fields — and collects each `SV_TargetN`
semantic with its scalar/vector type. The domain check then compares that collected set
against the contract (Surface → `SV_Target0` + `SV_Target1`, both `float4`; PostProcess →
`SV_Target0` only). This is a distinct reflection entry point from the struct-layout one,
exercised by the new cooker fixtures (a surface shader writing one target, a postprocess
shader writing the MRT — each a located cook error).

## Decisions

1. **The domain lives on `Material`, not `Renderer::Types.h`.** It is an asset-authoring
   property (which output template, which standard VS), parsed by the cooker and read by
   the loader and editor — not a backend vocabulary enum the `TypeMapping` switch consumes.
   It maps to no `vk::` enum. So it sits in `Veng/Asset/Material.h` beside `MaterialField`,
   and the cooked form stores its underlying int per the assetpack cycle rule, mirroring
   how `MaterialField::FieldKind` is mirrored as `CookedMaterialField::Kind`.

2. **`Surface = 0`, default-on-absence.** Existing `.vmat.json` files omit the key and cook
   identically; a zero-initialised or legacy `CookedMaterialHeader.Domain` reads as
   `Surface`. The planset adds no migration to packs it does not touch — the contract check
   for a surface material is exactly the MRT layout those materials already satisfy.

3. **The contract check is at cook, not load.** Validating the fragment outputs against the
   domain is offline tooling work (it has the Slang reflector), consistent with veng's
   "the loader trusts its packs" rule — the runtime never re-reflects. A mismatched shader
   is a located cook error, not a runtime assert.

4. **No pipeline/render change in this plan.** The loader reads the domain and stores it on
   `Material`; nothing yet branches on it (the surface path is unconditional today and stays
   so). Plan 04 is the first reader that builds a different pipeline from the domain. This
   keeps this plan a pure format+contract addition with a green `smoke_golden`.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Asset/Material.h` | New `MaterialDomain` enum; `MaterialInfo::Domain`; `Material::m_Domain` + `GetDomain()`. |
| `assetpack/include/Veng/Asset/CookedBlobs.h` | Insert `CookedMaterialHeader.Domain` (underlying int) after `Version`; bump `CookedMaterialVersion`; doc the field + default. |
| `engine/src/Asset/Loaders/MaterialLoader.cpp` | Assert `Version`; read `Domain`, cast guarded by `VE_ASSERT`, pass into `MaterialInfo`. |
| `cooker/src/Importers/SlangReflect.{h,cpp}` | New `ReflectFragmentOutputs` (entry-point `SV_TargetN` reflection via `IEntryPointReflection`). |
| `cooker/src/Importers/MaterialImporter.cpp` | Parse the `"domain"` key; reflect the fragment output targets; validate against the domain's contract; write `Domain` + bumped `Version` into the header. |
| `cooker/src/Importers/MaterialImporter.h` | Helper signature if the domain parse/validate is factored out. |
| `tests/cooker/fixtures/materials/*.vmat.json` | A `postprocess` fixture + a domain/output-mismatch negative fixture; existing fixtures stay domain-less (surface). |
| `tests/cooker/*` | Assertions: domain round-trips into the header; a surface shader writing one target and a postprocess shader writing the MRT each fail the cook with the located message. |

## Verification

- Clean build; `vengc` and `libveng` compile with the new field and enum.
- `ctest` cooker band: the new fixtures cook (postprocess) / fail-to-cook (mismatch) with
  the expected messages; existing material fixtures still cook unchanged (domain absent →
  surface).
- The `gpu`/example material re-cooks with `Domain = 0` in its header; `smoke_golden` is
  unchanged (no render path reads the domain yet).
- A blob cooked at plan 00's `Version` (no domain field) is rejected by the loader's version
  assert — the format break is loud, not silent.
