# Plan 05 — material instances: the parent owns the shader, the instance owns the slot

**Goal:** split today's fused `Material` into the two things every production engine separates — a
**parent material** (the generated shader → pipeline + the exposed-param *schema*: the expensive,
permutation-owning half) and a **material instance** (a cheap override over a parent: its own
parameter-block slot + texture set, no shader of its own). Codegen (Plans 01–03) makes the parent's
**exposed params** a first-class generated surface; this plan makes that surface the thing instances
override. Many instances share one parent's pipeline and differ only by a per-material SSBO slot —
so 30 tinted bricks become 1 generated shader + 30 instances, not 30 identical pipelines. Depends on
**Plan 02** (the exposed-param schema is the override surface) and lands after **Plan 04** (the
migrated sample parents are what sprout instances).

## Why it belongs in this planset

Codegen reframes a `.vmat` as a shader **author**, not a shader **binding** — the graph generates
the fragment source *and* the exposed `MaterialParams` schema in one walk (Plan 02). That generated
schema is, by definition, *exactly the set of parameters a caller may tweak without touching the
shader* — which is the precise definition of a material instance's override surface. Codegen and
instances are two halves of one idea: codegen **defines what is tweakable**, an instance **tweaks it
cheaply across many copies that share the generated shader and pipeline**. Landing instances in the
same planset is what makes the generated parameter surface earn its keep; landing it apart would
ship a parameter schema with no cheap consumer.

It is also the planset's one **runtime/cook/asset-format** plan: 01–03 are editor codegen, 04 is a
sample migration. Plan 05 touches the runtime `Material`, a new cooked blob, the mesh material list,
and the cooker — largely orthogonal to the editor codegen files, so it does not chain through their
merge order; it depends only on the *concept* Plan 02 establishes and the *parents* Plan 04 ships.

## Terminology note (the word is already taken)

The tree currently uses "material instance" loosely to mean **a live `Material` object** ("the
resident material instances the mesh owns," planset-7). This plan gives the phrase its standard
cross-engine meaning — a **parameter override over a parent material** — and introduces a
`MaterialInstance` asset for it. Existing prose using "material instance" in the old loose sense is
reworded to "the mesh's materials" / "a resident `Material`" as a doc-cleanup line item below; no
code carried the loose term in an identifier.

## The split

| | Parent — **`Material`** (Unreal *Material*) | Instance — **`MaterialInstance`** (Unreal *Material Instance*) |
|---|---|---|
| Owns | pipeline, pipeline layout, shader handles, the reflected `MaterialField` **schema**, the **default** param block | one **`MaterialHandle`** SSBO slot, its resident **texture override** set, a sparse override record |
| Cost | expensive — one pipeline per *(shader set + domain)* | cheap — one SSBO slot; **no** pipeline, **no** shader |
| Authored as | `*.vmat` (now graph-generated) → `AssetType::Material` | `*.vmatinst.json` → new `AssetType::MaterialInstance` |
| Bind | binds the parent's pipeline | pushes *this* instance's selector (parent's pipeline already bound) |
| Runtime-built (MID) | — | built from a parent handle; `SetParam`/`SetTexture` per frame |

The economic point is the one codegen sets up: a parent owns the permutation, an instance owns a row
in the already-`N`-buffered per-material SSBO. The machinery for the instance half **already exists**
on `Material` — the per-material slot (`m_Handle`), the stall-free ring-buffered `SetParam`/
`SetTexture`/`SetTextureHandle` writes, and the selector-push in `Bind`. This plan **moves** that
half onto `MaterialInstance` and leaves `Material` owning the pipeline + schema + defaults.

## What lands

- **`Material` becomes the parent.** It keeps the pipeline, layout, shader handles, the reflected
  `MaterialField` table (`GetFields()` — the schema an instance validates against), and a **default
  param block** (the cooked defaults, held as bytes, *not* a live SSBO slot). It loses the per-draw
  SSBO slot and the per-instance mutators. `GetPipeline()`/`GetPipelineLayout()`/`GetDomain()`/
  `GetSelectorOffset()`/`GetFields()` stay; `SetParam`/`SetTexture`/`GetMaterialSelector()` move to
  the instance.

- **`MaterialInstance` is the new asset** (`engine/include/Veng/Asset/MaterialInstance.h`,
  `AssetType::MaterialInstance`). It holds an `AssetHandle<Material> Parent` (kept resident, an
  ordinary load-time dependency exactly as a `Material` keeps its shaders/textures), one
  `Renderer::MaterialHandle` (its SSBO slot, seeded from the parent's default block + its overrides),
  its resident `vector<AssetHandle<Texture>>` overrides, and the `MaterialField` table borrowed from
  the parent. It exposes `Bind(cmd)` (binds the parent's pipeline, pushes its own selector),
  `GetMaterialSelector()`, and the `SetParam`/`SetTexture`/`SetTextureHandle`/`SetSamplerHandle`
  per-field mutators — the stall-free ring-buffered writes, unchanged in mechanism. A runtime
  instance built from a parent handle (`AssetManager::Build<MaterialInstance>` / `Adopt`) is the
  **MID**; the per-frame `SetParam` it already supports *is* the MID write path.

- **A new cooked blob + importer.** `*.vmatinst.json` declares `"parent": <AssetId>` and a sparse
  `"overrides"` map (field name → value, or texture field → `AssetId`). It cooks to a
  `CookedMaterialInstance` blob — `{ parentId, override records }` — through a new
  `MaterialInstanceImporter`. The cook **validates each override name + type against the parent's
  reflected field table** (the importer reflects the parent's fragment `MaterialParams` the same way
  `MaterialImporter` does today, or reads the parent's already-cooked field table): an override
  naming a field the parent does not expose, or a type mismatch, is a located cook error — the exact
  `.vmat`-against-shader validation that exists today, lifted one level to instance-against-parent.
  An omitted field inherits the parent default (schema tolerance).

- **`MaterialInstanceLoader`.** Resolves the parent `AssetHandle<Material>`, allocates the instance's
  own SSBO slot, **copies the parent's default block**, applies the override records by reflected
  offset, registers the instance's texture overrides into bindless, and patches their handle slots —
  the same Prepare(worker) → Finalize(render-thread, bindless) two-phase seam `Material` uses. Domain
  and pipeline come from the parent; the instance builds no pipeline.

- **A parent is usable directly as a zero-override instance.** To bound the migration, an
  `AssetHandle<Material>` used where an instance is expected resolves to the parent's **implicit
  default instance** (one instance owning the parent's default block, created lazily). So a pack that
  references a bare `Material` id keeps loading; authoring an explicit `*.vmatinst.json` is opt-in.

- **The mesh material list points at instances.** `Mesh`'s `vector<AssetHandle<Material>>` and
  `MeshRenderer::Material` become `AssetHandle<MaterialInstance>`; `SubMesh::MaterialIndex` indexes
  the instance list. The cooked mesh stores instance ids (a parent id resolves to its default
  instance via the rule above, so existing cooked meshes still load). The draw path binds
  `GetMaterials()[MaterialIndex]` — now an instance — exactly as today.

## Decisions

1. **`MaterialInstance` is a distinct asset, not a flag on `Material`.** The parent owns a pipeline;
   the instance owns a slot. A flag on one type would fuse the two lifetimes the whole point is to
   split (one expensive pipeline, `N` cheap slots), and would not give the mesh list a cheap-handle
   type to point at.
2. **The override surface is the parent's exposed-param schema — codegen's output.** An instance may
   set exactly the fields `Material::GetFields()` reports. This is why the plan lives here: Plan 02
   *generates* that schema; this plan *consumes* it. The validation reuses the reflection the cooker
   and editor already run.
3. **A parent doubles as its own zero-override instance.** The implicit default instance keeps the
   migration bounded (existing material-id references still load) and matches the Unreal/Unity ergonomic
   where a bare material is directly assignable. Authoring an explicit instance is the opt-in cheap-reuse
   path.
4. **The instance half is a move, not a rewrite.** The SSBO slot, the ring-buffered stall-free
   `SetParam`/`SetTexture`, and the selector-push already live on `Material`; this plan relocates them
   to `MaterialInstance` and leaves the pipeline/schema/defaults on `Material`. No new GPU mechanism.
5. **No draw-sort yet — correctness without the optimization.** Binding the parent pipeline per
   instance (even redundantly when consecutive draws share a parent) is correct; sorting the draw plan
   by parent pipeline → bind once → iterate instance selectors is the **payoff**, deferred to *what
   remains future* so this plan stays a clean split. The buffer-indexed indirect path (planset-25)
   already drives by per-draw `materialIndex`, so the future sort is a key, not new machinery.
6. **Single-level parent.** A `MaterialInstance` parents a `Material`, not another instance. Instance-of-instance
   chains (Unreal's nested MICs) fold an override stack at load; deferred until a real layering case appears.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Asset/Material.h` / `engine/src/Asset/Material.cpp` | Slim to the parent: keep pipeline/layout/shader handles/`GetFields()`/domain + a held default block; remove the per-instance slot + mutators. |
| `engine/src/Asset/Loaders/MaterialLoader.{h,cpp}` | Parent loader builds the pipeline + default block, no per-draw slot; the default-instance lazily wraps the parent block. |
| `engine/include/Veng/Asset/MaterialInstance.h` / `engine/src/Asset/MaterialInstance.cpp` | New asset: parent handle + own `MaterialHandle` slot + texture overrides + `Bind`/`SetParam`/`SetTexture`/…; Prepare→Finalize two-phase. |
| `engine/src/Asset/Loaders/MaterialInstanceLoader.{h,cpp}` | Resolve parent, copy default block, apply overrides, register texture overrides, allocate the slot. |
| `assetpack/include/Veng/Asset/AssetType.h` | Add `AssetType::MaterialInstance`. |
| `assetpack/include/Veng/Asset/CookedBlobs.h` | Add `CookedMaterialInstanceHeader` + override-record layout + a version constant. |
| `cooker/src/Importers/MaterialInstanceImporter.{h,cpp}` | Cook `*.vmatinst.json`: parse `parent` + sparse `overrides`, validate names/types against the parent's reflected fields, emit the blob. |
| `engine/include/Veng/Asset/Mesh.h` + mesh loader | The resident material list + `MeshRenderer::Material` become `AssetHandle<MaterialInstance>`; the cooked mesh's material ids resolve to instances (parent → default instance). |
| `engine/include/Veng/Scene/Components.h` | `MeshRenderer`/the material-carrying components hold `AssetHandle<MaterialInstance>`. |
| `engine/include/Veng/Asset/AssetManager.h` + Build/Adopt | `Build<MaterialInstance>` / `Adopt` (the MID path) and `BuildPrimitiveMesh`'s material wiring take instance handles. |
| `editor/src/panels/` (a material-instance inspector) | The MIC authoring surface: a parent picker + a per-field override toggle driving the reflection inspector over `Parent->GetFields()`. |
| `examples/hello-triangle/assets/` | `brick` becomes a parent; add a `*.vmatinst.json` (a tinted brick) and assign it to a submesh to exercise the path on GPU. |
| `examples/template/` | Co-migrated: its cube's material reference resolves through the default-instance rule (no authored instance needed). |
| `engine/CLAUDE.md`, `cooker/CLAUDE.md`, `editor/CLAUDE.md`, `plans/planset-7` prose | Document the parent/instance split; reword the loose "material instance" usages to the asset sense. |

## Verification

- Clean build; `ctest` green. A round-trip test: cook a parent `Material` + a `*.vmatinst.json`
  overriding one exposed `vec4` and one texture → load both → assert the instance binds the **parent's**
  pipeline, owns a **distinct** SSBO slot from the parent default, and packs the overridden value at the
  reflected offset while inheriting the rest from the parent default block.
- A second instance of the same parent shares the parent's `Ref<GraphicsPipeline>` (pointer-equal) and
  owns a different `MaterialHandle` — the pipeline-sharing invariant.
- An override naming a non-exposed field is a **located cook error** (the instance-against-parent
  validation), mirroring the existing `.vmat`-against-shader test.
- The default-instance rule: a cooked mesh referencing a bare `Material` id loads and renders
  unchanged (the migration-safety path).
- The MID path: a runtime `Build<MaterialInstance>(parent)` + per-frame `SetParam` writes the current
  frame-in-flight region stall-free (no `WaitIdle`), reusing the existing ring-buffer test.
- `smoke_golden` is preserved by construction (the migrated `brick` parent + its default instance pack
  the same bytes the fused material did); the *new* tinted-brick instance is exercised by a fixture/GPU
  test, not the golden. Validation gate clean (the instance binds set-0 bindless + the parent's pipeline,
  identical to a material today).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
