# Plan 06 — material instances: cooker importer + editor inspector

**Goal:** the authoring surface over Plan 05's runtime. Plan 05 ships the `MaterialInstance` asset,
loader, cooked blob, and the tree-wide material-reference retype — but the only way to *make* an
instance there is a hand-built blob fixture or a runtime `Build<MaterialInstance>`. This plan adds the
two authoring paths: a **`MaterialInstanceImporter`** that cooks a `*.vmatinst.json` source into the
Plan 05 blob (validating overrides against the parent's reflected fields), and an **editor
material-instance inspector** (a parent picker + per-field override toggle over `Parent->GetFields()`).
It closes the offline-authoring and in-editor-authoring loops and exercises the path on GPU with a
real sample instance. Depends on **Plan 05**.

## Why it is its own plan

Plan 05 is the migration-critical runtime/format core that everything co-migrates against; this plan
is the *authoring* surface that consumes it and merges after. The importer and the inspector touch a
different file set (cooker + editor panels), so splitting keeps Plan 05's wide retype reviewable on its
own and lets the authoring tools land without re-touching the runtime types.

## What lands

- **The `*.vmatinst.json` source + `MaterialInstanceImporter`.** A `*.vmatinst.json` declares
  `"parent": <AssetId>` and a sparse `"overrides"` map (field name → value, or texture field →
  `AssetId`). It cooks to the Plan 05 `CookedMaterialInstance` blob through a new
  `MaterialInstanceImporter`. The cook **validates each override name + type against the parent's
  reflected field table** — the importer reflects the parent's fragment `MaterialParams` the same way
  `MaterialImporter` does, or reads the parent's already-cooked field table. An override naming a field
  the parent does not expose (or an engine-bound field, which is not an override surface), or a type
  mismatch, is a **located cook error** — the exact `.vmat`-against-shader validation that exists today,
  lifted one level to instance-against-parent. An omitted field inherits the parent default (schema
  tolerance).

- **The editor material-instance inspector.** The MIC authoring surface: a parent picker + a per-field
  override toggle driving the reflection inspector over `Parent->GetFields()` (the exposed subset
  only). Toggling a field on adds it to the sparse override set; off reverts it to the parent default.
  Saving writes the `*.vmatinst.json`.

- **A sample instance exercises the path on GPU.** hello-triangle's `brick` (now a parent, Plan 04)
  gets a `*.vmatinst.json` — a tinted brick overriding one exposed `vec4` — assigned to a submesh, so
  the parent-pipeline-shared / distinct-slot path runs on the GPU, not just in a fixture.

- **The doc cleanup.** The loose "material instance" usages (planset-7 prose, the CLAUDE.md files) are
  reworded to the asset sense ("the mesh's materials" / "a resident `Material`" for the old meaning, and
  the new `MaterialInstance` for the override asset).

## Decisions

1. **Validation is instance-against-parent, reusing existing reflection.** The override set is checked
   against `Material::GetFields()`' exposed subset with the same reflection the cooker/editor already
   run — the `.vmat`-against-shader check lifted one level. Engine-bound fields are not overridable.
2. **The inspector drives off the parent's schema.** The override surface the UI offers is exactly the
   parent's exposed `GetFields()`, so the authoring surface and the validated surface are the same set
   by construction.
3. **The sample instance is the GPU proof.** A fixture proves the loader; an authored tinted-brick
   instance assigned to a submesh proves the full authoring → cook → render path on a shipping sample.

## Files

| File | Change |
|---|---|
| `cooker/src/Importers/MaterialInstanceImporter.{h,cpp}` | Cook `*.vmatinst.json`: parse `parent` + sparse `overrides`, validate names/types against the parent's reflected exposed fields, emit the Plan 05 blob. |
| `cooker/src/Cooker.cpp` (importer registration) | Register the `MaterialInstance` importer for `*.vmatinst.json`. |
| `editor/src/panels/` (a material-instance inspector) | Parent picker + per-field override toggle over `Parent->GetFields()`; writes `*.vmatinst.json`. |
| `examples/hello-triangle/assets/` | Add a `brick` tinted-instance `*.vmatinst.json` + its manifest row; assign it to a submesh. |
| `engine/CLAUDE.md`, `cooker/CLAUDE.md`, `editor/CLAUDE.md`, `plans/planset-7` prose | Document the instance authoring surface; reword the loose "material instance" usages to the asset sense. |

## Verification

- Clean build; `ctest` green. A cooker test: a `*.vmatinst.json` overriding one exposed `vec4` and one
  texture cooks to a valid blob; an override naming a non-exposed (or engine-bound) field is a
  **located cook error**, mirroring the existing `.vmat`-against-shader test.
- The authored sample instance loads and renders: it binds `brick`'s pipeline, owns a distinct SSBO
  slot, and shows the tint — the GPU proof (a fixture/GPU test, not the golden).
- The editor inspector round-trips: author overrides → save `*.vmatinst.json` → reopen → the same
  overrides + parent are restored.
- `smoke_golden` is unchanged (the new tinted-brick instance is a fixture/GPU test, not in the golden
  scene). Validation gate clean.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
