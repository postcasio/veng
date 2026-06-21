# Plan 08 — Sample primitive prefab + docs

**Goal:** prove the whole chain end to end through the shipping path — author
hello-triangle's geometry as a `*.prefab.json` carrying `PrimitiveComponent`s, cook it
with module-reflected validation, spawn + resolve it at startup, and confirm the smoke
render is unchanged. Then update the docs. Depends on plan 07 (the component), plan 03
(cook the prefab), and plan 04 (select primitives in the editor).

## Why this is its own plan

It is the integration + migration + docs pass every planset closes with. It exercises the
cooker→loader→spawn→resolve→render path the runtime actually ships, and it is the first
real authored primitive recipe — so it doubles as the acceptance test for plans 01–07
working together.

## Migrate the sample to an authored primitive prefab

hello-triangle currently builds its sphere and plane at runtime (`Primitives::Icosphere`
/ `Plane` → `Mesh::Create` → `Adopt`, then writes the handle into a `MeshRenderer`).
Replace that with an authored prefab:

1. **Author `examples/hello-triangle/assets/*.prefab.json`** with entities carrying a
   `PrimitiveComponent` (an `IcosphereShape` with the brick material, a `PlaneShape` with
   the brick material) plus their `Transform`s and the existing light. Use the
   `{ "type": "IcosphereShape", "value": { … } }` variant shape from plan 03. The material
   is an `AssetHandle<Material>` field referencing the existing brick material id.
2. **Add the prefab to the pack manifest** (`{ id, type: prefab, source }`); the pack
   already declares `MODULE libhello_triangle` for prefab validation
   (`add_asset_pack(... MODULE …)`), so the cook reflects the real types — including the
   new `PrimitiveComponent` and shapes registered as builtins.
3. **Load + spawn at startup:** `LoadSync<Prefab>` the prefab, `SpawnInto` the app's
   `Scene`, then call `ResolvePrimitiveMeshes(scene, GetAssetManager(), m_PrimitiveCache)`
   (a `PrimitiveMeshCache` member held beside the `Scene`). Drop the hand-built
   `Primitives::`/`Adopt` code from `OnInitialize`. The orbit/rotation and camera stay as
   they are.
4. Keep `OnDispose` releasing the prefab handle, the scene, and `m_PrimitiveCache` like any
   other resource (the cache's handles retire its meshes when cleared).

The runtime `Primitives::` API stays public (tests and tools use it directly); the sample
simply stops being the demonstration of the *hand-built* path and becomes the
demonstration of the *authored-recipe* path.

## Smoke golden

The geometry is the same icosphere + plane in the same poses, so the `HT_SMOKE` capture
should be **byte-identical** and `smoke_golden` should pass untouched. If resolution
timing means the mesh is not resident on the captured frame, the smoke path must pump the
task system until the primitive is resident before capturing (the capture is a fixed pose,
so a deterministic "resolve then capture" is correct) — prefer making the smoke capture
wait for residency over regenerating the golden. Only if the rendered geometry genuinely
moves do you regenerate per the CLAUDE.md procedure:

```sh
HT_SMOKE=/tmp/ht.ppm build/examples/hello-triangle/hello_triangle-launcher
sips -s format png /tmp/ht.ppm --out tests/golden/hello_triangle_scene.png
```

`hello_triangle_launcher_smoke` (the full `dlopen` → register → `Run()` chain) must still
exit 0 — this is the test that proves the authored-prefab path works through the shipping
launcher, not just in-process.

## Docs

- **`engine/CLAUDE.md`** — in the assets section, note that a primitive mesh is now also
  a persistable, prefab-authored `PrimitiveComponent` (a `Variant` shape recipe resolved
  + async-streamed into `MeshRenderer.Mesh` by `ResolvePrimitiveMeshes`), alongside the
  existing runtime `Primitives::`/`Adopt` path. In the Scene & ECS / reflection section,
  document `FieldClass::Variant` as the tagged-union meta-kind (serialized as a `TypeId`
  tag + the active member's record; authored in JSON as `{ "type", "value" }`).
- **`cooker/CLAUDE.md`** — note the prefab importer validates variant fields against the
  alternative type names.
- **`editor/CLAUDE.md`** — note the inspector's variant widget (combo + recursed active
  member), shared by the entity and node-property inspectors.
- **`plans/README.md`** — add the planset-26 line with status.
- The per-shape param ranges and the `PrimitiveComponent` shape live in the public
  headers with full Doxygen (every public declaration documented).

## Acceptance

- Clean build; full `ctest` green; `hello_triangle_launcher_smoke` exits 0 and writes a
  correct-sized 1280×720 PPM.
- `smoke_golden` passes — unchanged golden (the capture waits for primitive residency);
  regenerate only if the geometry genuinely moved, and say so explicitly in the commit.
- The sample renders its geometry from an **authored prefab** carrying
  `PrimitiveComponent`s, cooked with module-reflected validation, spawned and resolved at
  startup — no hand-built `Primitives::`/`Adopt` in the sample's init.
- Opening the prefab in the editor shows the shapes selectable via the variant widget with
  editable per-shape parameters; editing re-resolves the mesh.
- `VE_DEBUG` validation gate clean; docs updated (`CLAUDE.md` ×3, `plans/README.md`).
