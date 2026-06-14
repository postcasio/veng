# Plan 04 — Sample demo + roadmap/docs

**Goal:** exercise the feature end-to-end on GPU. Migrate `examples/hello-triangle`
to draw a **runtime primitive** carrying a **material instance**, proving
generation, upload, material assignment (through the mesh's resident list), and
the per-submesh draw all work together. Update the roadmap.

## Sample migration — `examples/hello-triangle/main.cpp`

After plan 01 the sample already binds its material **through the mesh's submesh
list**. This plan swaps the geometry source from the cooked cube to a runtime
primitive, passing the loaded brick material as an instance:

- Keep loading the brick material as an `AssetHandle<Material>` (`AssetId{1003}`).
- Drop the `LoadSync<Veng::Mesh>(AssetId{1002})` / `m_CubeMesh`
  `AssetHandle<Mesh>`; replace with a `Ref<Veng::Mesh>` built in `OnInitialize`,
  handing the material instance to the generator:

  ```cpp
  m_Mesh = Veng::Mesh::Create(
      context, Veng::Primitives::Sphere(0.8f, 24, 48, m_BrickMaterial), "Demo Sphere");
  ```

  The sphere's single submesh now indexes the mesh's resident material list (slot
  0 = brick) — the specified-material path, instances not ids.

- The draw loop is the per-submesh loop from plan 01, unchanged: registry `Bind`,
  `BindVertexBuffer`/`BindIndexBuffer`, then for each submesh bind
  `mesh.GetMaterials()[sm.MaterialIndex].Get()` and `DrawIndexed` its range. The
  brick material's pipeline declares `Mesh::CanonicalLayout()`, which the runtime
  primitive uses — so it just draws. (A sphere shows the brick normal/UV mapping
  better than a cube.)

- `OnDispose`: reset `m_Mesh` (a `Ref`). The mesh keeps the brick material handle
  resident; the sample can keep its own `m_BrickMaterial` handle or drop it once the
  mesh owns one — keep it if it still loads the material before generating.

- The cooked `cube.mesh.json` / `cube.obj` and their pack entry can stay (still
  cooked, still covered by the cooker/GPU tests) or be removed from
  `sample.vengpack.json` if the sample no longer references them. Prefer leaving the
  cook-path coverage in the test suite; if removing from the sample pack, keep at
  least one material-bearing mesh asset cooked somewhere a test mounts (plan 01's
  `mesh_loader` test depends on it).

Keep the `HT_SMOKE` smoke contract intact: the binary must still render and write a
correctly-sized 1280×720 PPM. The geometry change doesn't affect the capture path.

## Docs / roadmap

- `plans/README.md` — add the planset-7 line (runtime primitive meshes) with status.
- `plans/planset-7/README.md` — flip the status column to `done` per plan as they
  land; mark the planset complete here.
- Asset docs / `CLAUDE.md` — note under the mesh/asset section that (a) a runtime
  `Mesh` can be built from `MeshData` via `Primitives::*` + the
  `Mesh::Create(context, data, name)` factory, distinct from the cooked-asset path,
  and (b) the mesh→material model: a mesh owns a list of `AssetHandle<Material>` and
  each submesh indexes it (`SubMesh::MaterialIndex`), the cooked loader resolving
  serialized ids into that list. Remove the stale "the mesh does not load its
  materials" statement.
- If the editor/scene future docs (`plans/future/`) reference placing meshes, they
  may now cite `Primitives` as available groundwork (optional, non-blocking).

## Verification

- Clean build; `ctest` green (`unit` + the GPU suite + smoke).
- `HT_SMOKE=/tmp/ht.ppm build/examples/hello-triangle/hello_triangle` exits 0 and
  writes a 1280×720 RGB PPM (≈ 2,764,816 bytes).
- `VE_DEBUG` validation clean: run the smoke binary from `build-debug/` and confirm
  no unallowlisted `Vulkan validation` ERROR — the runtime primitive uses the same
  buffer/upload/draw path as cooked meshes, so none is expected.

## Acceptance

The sample renders a runtime-generated sphere whose submesh carries the brick
material instance, bound through the mesh's resident list; no cooked mesh is
required to put geometry on screen; the roadmap and asset docs record both the
runtime-primitive path and the new mesh→material model.
