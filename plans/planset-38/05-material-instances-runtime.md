# Plan 05 — material instances: runtime core (parent owns the shader, instance owns the slot)

**Goal:** split today's fused `Material` into the two things every production engine separates — a
**parent material** (`Material`: the shader → pipeline + the exposed-param *schema*: the expensive,
permutation-owning half) and a **`MaterialInstance`** (a cheap override over a parent: its own
parameter-block slot + texture set, no shader of its own). Codegen (Plans 01–04) makes the parent's
**exposed params** a first-class generated surface; this plan makes that surface the thing instances
override. Many instances share one parent's pipeline and differ only by a per-material SSBO slot — so
30 tinted bricks become 1 generated shader + 30 instances, not 30 identical pipelines. This plan lands
the **runtime + format**: the `Material` slim-down, the new `MaterialInstance` asset/loader/blob, and
the pervasive material-reference retype. The cooker importer and editor inspector are Plan 06.
Depends on **Plan 02** (the exposed-param schema) and lands after **Plan 04** (the migrated parents).

## Why it belongs in this planset

Codegen reframes a material as a shader **author**, not a shader **binding** — the graph generates the
fragment source *and* the exposed `MaterialParams` schema (Plan 02). That generated schema is, by
definition, *exactly the set of parameters a caller may tweak without touching the shader* — which is
the precise definition of a material instance's override surface. Codegen and instances are two halves
of one idea: codegen **defines what is tweakable**, an instance **tweaks it cheaply across many copies
that share the generated shader and pipeline**. Landing it apart would ship a parameter schema with no
cheap consumer.

It is split from Plan 06 because the runtime/format change (a new asset type, a cooked blob, and the
`AssetHandle<Material>` → `AssetHandle<MaterialInstance>` retype that ripples across mesh/components/
primitives/prefab resolution) is the migration-critical core everything co-migrates against; the
cooker importer + editor inspector that *author* instances ride on top and merge after.

## Terminology note (the word is already taken)

The tree currently uses "material instance" loosely to mean **a live `Material` object** ("the
resident material instances the mesh owns," planset-7). This plan gives the phrase its standard
cross-engine meaning — a **parameter override over a parent material** — and introduces a
`MaterialInstance` asset for it. A runtime-built instance + per-frame `SetParam` is the **MID**
(Material Instance Dynamic). Existing prose using the loose sense is reworded in Plan 06's doc
cleanup; no code carried the loose term in an identifier.

## The split

| | Parent — **`Material`** (Unreal *Material*) | Instance — **`MaterialInstance`** (Unreal *Material Instance*) |
|---|---|---|
| Owns | pipeline, pipeline layout, shader handles, the reflected `MaterialField` **schema**, the **default** param block | one **`MaterialHandle`** SSBO slot, its resident **texture override** set, a sparse override record |
| Cost | expensive — one pipeline per *(shader set + domain)* | cheap — one SSBO slot; **no** pipeline, **no** shader |
| Authored as | graph-sourced shader → `AssetType::Material` | `*.vmatinst.json` → new `AssetType::MaterialInstance` (Plan 06) |
| Bind | binds the parent's pipeline | pushes *this* instance's selector (parent's pipeline already bound) |
| Runtime-built (MID) | — | built from a parent handle; `SetParam`/`SetTexture` per frame |

The machinery for the instance half **already exists** on `Material` — the per-material slot
(`m_Handle`), the stall-free ring-buffered `SetParam`/`SetTexture`/`SetTextureHandle` writes, and the
selector-push in `Bind`. This plan **moves** that half onto `MaterialInstance` and leaves `Material`
owning the pipeline + schema + defaults.

## What lands

- **`Material` becomes the parent.** It keeps the pipeline, layout, shader handles, the reflected
  `MaterialField` table (`GetFields()` — the schema an instance validates against), and a **default
  param block** (the cooked defaults, held as bytes, *not* a live SSBO slot). It loses the per-draw
  SSBO slot and the per-instance mutators. `GetPipeline()`/`GetPipelineLayout()`/`GetDomain()`/
  `GetSelectorOffset()`/`GetFields()` stay; `SetParam`/`SetTexture`/`GetMaterialSelector()` move to
  the instance.

- **`MaterialInstance` is the new asset** (`engine/include/Veng/Asset/MaterialInstance.h`,
  `AssetType::MaterialInstance`). It holds an `AssetHandle<Material> Parent` (kept resident, an
  ordinary load-time dependency), one `Renderer::MaterialHandle` (its SSBO slot, seeded from the
  parent's default block + its overrides), its resident `vector<AssetHandle<Texture>>` overrides, and
  the `MaterialField` table borrowed from the parent. It exposes `Bind(cmd)` (binds the parent's
  pipeline, pushes its own selector), `GetMaterialSelector()`, and the
  `SetParam`/`SetTexture`/`SetTextureHandle`/`SetSamplerHandle` per-field mutators — the stall-free
  ring-buffered writes, unchanged in mechanism. A runtime instance built from a parent handle
  (`AssetManager::Build<MaterialInstance>` / `Adopt`) is the **MID**; the per-frame `SetParam` it
  already supports *is* the MID write path.

- **A new cooked blob.** `AssetType::MaterialInstance` + a `CookedMaterialInstanceHeader`
  (`{ parentId, override records }`) + a version constant in `CookedBlobs.h`. (The `*.vmatinst.json`
  source and the importer that emits this blob are Plan 06; this plan defines the blob and the loader
  that reads it.)

- **`MaterialInstanceLoader`.** Resolves the parent `AssetHandle<Material>`, allocates the instance's
  own SSBO slot, **copies the parent's default block**, applies the override records by reflected
  offset, registers the instance's texture overrides into bindless, and patches their handle slots —
  the same Prepare(worker) → Finalize(render-thread, bindless) two-phase seam `Material` uses. Domain
  and pipeline come from the parent; the instance builds no pipeline.

- **A parent is usable directly as a zero-override instance.** An `AssetHandle<Material>` used where an
  instance is expected resolves to the parent's **implicit default instance** (one instance owning the
  parent's default block, created lazily). So a pack that references a bare `Material` id keeps loading;
  authoring an explicit `*.vmatinst.json` is opt-in.

- **The material references retype across the tree.** `AssetHandle<Material>` → `AssetHandle<MaterialInstance>`
  is broader than the mesh list — it is enumerated and migrated wholesale:
  - **`Mesh`** — `MeshData::Materials`, `Mesh::m_Materials`, `GetMaterials()`, and the cooked-mesh
    `MaterialId` resolution (a parent id resolves to its default instance, so existing cooked meshes
    still load).
  - **`MeshSource` shape variants** in `Components.h` — every `AssetHandle<Material> Material` field
    across `CubeShape`/`PlaneShape`/`SphereShape`/… (eight fields) plus `MeshRenderer::Material`.
  - **`Primitives`** — the nine `Cube`/`Plane`/`Sphere`/`Icosphere`/`Cylinder`/`Cone`/`FinishSubMesh`/…
    factory signatures (and `BuildShapeMeshData`/`BuildPrimitiveMesh`) that take a material handle.
  - **`AssetManager`** — `Build`/`Adopt`/`BuildPrimitiveMesh` material wiring take instance handles.
  - **Prefab/level reflection** — `MeshRenderer::Material` is a reflected `AssetHandle` field cooked
    into prefabs/levels; the `AssetHandle` type-match / `AssetSourceIndex` resolution must accept a
    `Material`-typed source id for a `MaterialInstance` field (the default-instance rule applied to the
    reflected-field path, not only the cooked-mesh path).

  The draw path binds `GetMaterials()[MaterialIndex]` — now an instance — exactly as today.

## Decisions

1. **`MaterialInstance` is a distinct asset, not a flag on `Material`.** The parent owns a pipeline;
   the instance owns a slot. A flag on one type would fuse the two lifetimes the whole point is to
   split (one expensive pipeline, `N` cheap slots), and would not give the mesh list a cheap-handle
   type to point at.
2. **The override surface is the parent's exposed-param schema — codegen's output.** An instance may
   set exactly the **exposed** fields `Material::GetFields()` reports; engine-bound fields (Plan 02)
   are not an override surface. The validation reuses the reflection the cooker and editor already run.
3. **A parent doubles as its own zero-override instance.** The implicit default instance keeps the
   migration bounded (existing material-id references — cooked meshes *and* reflected prefab fields —
   still load) and matches the Unreal/Unity ergonomic where a bare material is directly assignable.
4. **The instance half is a move, not a rewrite.** The SSBO slot, the ring-buffered stall-free
   `SetParam`/`SetTexture`, and the selector-push already live on `Material`; this plan relocates them
   to `MaterialInstance` and leaves the pipeline/schema/defaults on `Material`. No new GPU mechanism.
5. **No draw-sort yet — correctness without the optimization.** Binding the parent pipeline per
   instance (even redundantly when consecutive draws share a parent) is correct; sorting by parent →
   bind once → iterate selectors is the **payoff**, deferred to *what remains future*. The
   buffer-indexed indirect path (planset-25) already drives by per-draw `materialIndex`, so the future
   sort is a key, not new machinery.
6. **Single-level parent.** A `MaterialInstance` parents a `Material`, not another instance.
   Instance-of-instance chains are deferred until a real layering case appears.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Asset/Material.h` / `engine/src/Asset/Material.cpp` | Slim to the parent: keep pipeline/layout/shader handles/`GetFields()`/domain + a held default block; remove the per-instance slot + mutators. |
| `engine/src/Asset/Loaders/MaterialLoader.{h,cpp}` | Parent loader builds the pipeline + default block, no per-draw slot; the default-instance lazily wraps the parent block. |
| `engine/include/Veng/Asset/MaterialInstance.h` / `engine/src/Asset/MaterialInstance.cpp` | New asset: parent handle + own `MaterialHandle` slot + texture overrides + `Bind`/`SetParam`/`SetTexture`/…; Prepare→Finalize two-phase. |
| `engine/src/Asset/Loaders/MaterialInstanceLoader.{h,cpp}` | Resolve parent, copy default block, apply overrides, register texture overrides, allocate the slot. |
| `assetpack/include/Veng/Asset/AssetType.h`, `CookedBlobs.h` | Add `AssetType::MaterialInstance`; `CookedMaterialInstanceHeader` + override-record layout + a version constant. |
| `engine/include/Veng/Asset/Mesh.h` + mesh loader | `MeshData::Materials`/`m_Materials`/`GetMaterials()` → `AssetHandle<MaterialInstance>`; cooked-mesh `MaterialId` resolves to instances (parent → default instance). |
| `engine/include/Veng/Scene/Components.h` | `MeshRenderer::Material` + every `MeshSource` shape variant's `Material` field → `AssetHandle<MaterialInstance>`; the reflected-field `AssetHandle` resolution accepts a `Material` id for a `MaterialInstance` field. |
| `engine/include/Veng/Asset/Primitives.h` / `engine/src/Asset/Primitives.cpp` | The nine factory signatures + `BuildShapeMeshData` take `AssetHandle<MaterialInstance>`. |
| `engine/include/Veng/Asset/AssetManager.h` + Build/Adopt | `Build<MaterialInstance>` / `Adopt` (the MID path) and `BuildPrimitiveMesh`'s material wiring take instance handles. |
| `examples/template/` | Co-migrated: its cube's material reference resolves through the default-instance rule (no authored instance needed). |
| `engine/CLAUDE.md` | Document the parent/instance split + the default-instance rule. |

## Verification

- Clean build; `ctest` green. A round-trip test: cook a parent `Material` + a `MaterialInstance` blob
  (a hand-built fixture; the JSON importer is Plan 06) overriding one exposed `vec4` and one texture →
  load both → assert the instance binds the **parent's** pipeline, owns a **distinct** SSBO slot from
  the parent default, and packs the overridden value at the reflected offset while inheriting the rest.
- A second instance of the same parent shares the parent's `Ref<GraphicsPipeline>` (pointer-equal) and
  owns a different `MaterialHandle` — the pipeline-sharing invariant.
- The default-instance rule: a cooked mesh **and** a cooked prefab referencing a bare `Material` id
  both load and render unchanged (the migration-safety path, both resolution paths).
- Game/runtime code assigning an `AssetHandle<Material>` to a renderer compiles and runs via the
  implicit-default-instance conversion (the C++ assignment ergonomic, not just the cooked-load path).
- The MID path: a runtime `Build<MaterialInstance>(parent)` + per-frame `SetParam` writes the current
  frame-in-flight region stall-free (no `WaitIdle`), reusing the existing ring-buffer test.
- `smoke_golden` is preserved by construction (the migrated `brick` parent + its default instance pack
  the same bytes the fused material did). Validation gate clean (the instance binds set-0 bindless +
  the parent's pipeline, identical to a material today).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
