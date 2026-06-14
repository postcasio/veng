# Plan 01 — Mesh runtime material model (resident materials, indexed submeshes)

**Goal:** fix the mesh's runtime material representation before building anything on
it. A `SubMesh` stops carrying a raw material `AssetId` and instead carries a `u32`
**index** into a list of resident material instances the `Mesh` owns
(`AssetHandle<Material>`). `MeshLoader` resolves the cooked submesh ids into that
list eagerly — exactly as `Material` already resolves its textures/shaders. The
sample's draw loop binds the per-submesh material. No primitives yet.

## Why this is its own plan, and first

Today `SubMesh::Material` is an `AssetId` and the mesh "does not load its
materials" — a serialized id leaking into the in-memory model, and the *only* asset
in veng that doesn't eager-load its dependencies. Every consumer (the runtime
`MeshData`, the generators, the demo) wants the fixed model, so it lands first and
alone: a focused, behaviour-correct redesign of the existing cooked-mesh path with
no new feature riding on it.

This **reverses a planset-5 decision** (`Mesh.h`: "the mesh does not load its
materials; submesh ids are forward references the caller resolves explicitly"). The
reversal is deliberate and makes the asset graph uniform — `Material` already
eager-loads its `AssetHandle<Texture>` / `AssetHandle<Shader>` deps and holds them
resident; the mesh now does the same for its materials.

## API change — `Mesh.h`

```cpp
struct SubMesh
{
    u32 IndexOffset = 0;
    u32 IndexCount  = 0;
    // Index into the owning Mesh's material list; NoMaterial = unassigned
    // (the caller binds its own material, as the bind model already allows).
    u32 MaterialIndex = NoMaterial;
    static constexpr u32 NoMaterial = ~0u;
};
```

`SubMesh` loses its `AssetId Material` field. `Mesh` gains a resident material list
and accessor:

```cpp
[[nodiscard]] std::span<const AssetHandle<Material>> GetMaterials() const;
```

`MeshInfo` gains `vector<AssetHandle<Material>> Materials;`, stored on the mesh.
`Mesh.h` already includes `AssetHandle.h`; add `#include <Veng/Asset/Material.h>`
(all-public, `include_hygiene` stays green) or forward-declare `Material` and keep
the handle methods out-of-line — prefer the include for simplicity.

## Loader change — `MeshLoader.cpp`

The cooked format is unchanged: `CookedSubMesh.MaterialId` stays a `u64`. The
loader now bridges ids → resident handles:

1. While reading the submesh table, collect the distinct non-zero `MaterialId`s.
2. For each distinct id, `manager.LoadSync<Material>(AssetId{id})` (the `manager`
   parameter, currently ignored, becomes used) — mirroring `MaterialLoader`'s
   texture/shader dependency loads. Build a `vector<AssetHandle<Material>>` and a
   `u64 id → u32 index` map.
3. Each `SubMesh.MaterialIndex` = the mapped index, or `SubMesh::NoMaterial` when
   the cooked id is `0` (unassigned).
4. Pass the material list into `MeshInfo`.

A failed material dependency load propagates as the load error (the mesh fails to
load if a declared material id is missing), same contract as a material whose
texture is missing.

## Draw change — `examples/hello-triangle/main.cpp`

The sample currently binds one material for the whole cube. With submeshes carrying
material indices, the draw loop binds per submesh:

```cpp
registry.Bind(cmd);
for (const Veng::SubMesh& sm : mesh.GetSubMeshes())
{
    if (sm.MaterialIndex != Veng::SubMesh::NoMaterial)
        mesh.GetMaterials()[sm.MaterialIndex].Get()->Bind(cmd);
    cmd.DrawIndexed(sm.IndexCount, 1, sm.IndexOffset, 0, 0);
}
```

The cooked cube's submesh references material `1003` (brick), so the mesh now owns
that handle and the loop binds it — the sample can drop its standalone
`m_BrickMaterial` handle (the mesh keeps the material resident), or keep it; prefer
binding through the mesh to prove the model. `Bind` must still precede the
registry `Bind` per frame? No — registry binds set 0 once; the per-submesh
`material->Bind` swaps the pipeline + pushes the selector, and the registry bind
targets the bound pipeline's layout, so keep the existing order: per draw,
`material->Bind` then ensure set 0 is bound for that pipeline. Match the current
sample's bind sequencing; only the *source* of the material changes.

## Tests

- `tests/gpu/mesh_loader.cpp` — update to the new `SubMesh` shape; assert the loaded
  mesh's `GetMaterials()` is populated and each submesh's `MaterialIndex` resolves
  to the expected material (the cooker fixture `mesh_pack.json` already assigns a
  material). If the fixture has no material, add one so the resolution path is
  covered.
- Any other reader of `SubMesh::Material` (grep the tree) migrates to
  `MaterialIndex` + `GetMaterials()`.

## Acceptance

- Clean build; `ctest` green; `VE_DEBUG` validation clean.
- Smoke binary writes a correct-sized PPM; the sample renders the cube with its
  material resolved *through the mesh*.
- No `AssetId` remains on a runtime `SubMesh`; the cooked blob format is untouched.
