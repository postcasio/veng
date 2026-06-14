# planset-7 — runtime primitive meshes

**Phase goal:** let an app build a `Mesh` **at runtime** — a cube, plane, or UV
sphere — without authoring a model file or running the cooker. A small generator
produces CPU-side geometry in veng's canonical vertex layout; a new upload factory
turns that geometry into the same `Ref<Mesh>` the cooked-asset loader returns, so a
runtime primitive and a cooked mesh are interchangeable to every pipeline and draw
call.

It opens by **fixing the mesh's runtime material model** — submeshes index a list
of resident material instances the mesh owns, instead of carrying a serialized
`AssetId` — then builds the generators on the clean model.

This is **not** part of any future-area chain. It is a small, self-contained
utility planset, useful in three places: standing up geometry in tests and tools
without the cook step, giving the future editor/scene work ready-made primitives to
place, and giving sample/demo code a one-liner mesh. It is self-contained: it
depends on nothing from any other planset and shares no work with them.

## The decisions that shape this planset

1. **Two layers: pure CPU geometry, then GPU upload.** A `MeshData` struct
   (canonical-layout vertices + u32 indices + a resident material list + a submesh
   table) is plain data with no Vulkan in sight; primitive generators return it. A
   separate factory uploads a `MeshData` into a GPU `Mesh`. This keeps all the
   primitive *math* GPU-free and unit-testable (vertex/index counts, in-bounds
   indices, unit-length normals, AABB extents, winding) and lets an app tweak
   geometry before upload.

2. **The canonical layout, reused verbatim.** Generated meshes use
   `Mesh::CanonicalLayout()` (position/normal/tangent/uv, 48-byte stride) — the
   exact layout the cooker writes and `MeshLoader` validates. A runtime primitive is
   therefore drawable by the same pipeline as a cooked mesh, with no special-casing.
   A public `CanonicalVertex` struct is added beside the layout so callers and the
   generators share one definition; its `sizeof`/field offsets are statically
   asserted against the layout's stride and offsets.

3. **Analytic tangents, not UV-reconstructed.** Each primitive has a known
   parametrization, so the tangent (and its handedness `w`) is computed exactly per
   vertex rather than re-derived from UV deltas. The cooker's assimp path needs
   `aiProcess_CalcTangentSpace` because arbitrary models have no closed form; a
   generated primitive does, so it gets exact tangents for free.

4. **A synchronous factory uses the blocking `UploadSync`.** `Mesh::Create(context,
   data, name)` returns a resident `Ref<Mesh>` directly — not a `Task<Ref<Mesh>>` —
   so it uploads with the blocking `UploadSync`, exactly as `MeshLoader` and the
   smoke path do. (Async `Upload(TaskSystem&) → Task<void>` is the engine default
   since planset-6; `UploadSync` is its blocking sibling.) `MeshData` and the
   generators are upload-policy-agnostic, so an async overload of the factory is a
   later addition with no change to either.

5. **No new asset type, no cook path.** A runtime primitive is *not* an
   `AssetId`-addressable asset and never touches an archive. It is constructed
   directly, owned by whoever calls the factory, and retires through the normal
   per-frame deferred-destruction path like any other `Mesh`.

6. **The mesh owns its materials; submeshes index them.** A `SubMesh` carries a
   `u32 MaterialIndex` into a list of resident `AssetHandle<Material>` the `Mesh`
   owns — **not** a serialized `AssetId`. The cooked on-disk format keeps its u64
   `MaterialId` (ids are the right serialized form); `MeshLoader` resolves those ids
   into material instances eagerly and builds the list, exactly as `Material`
   already resolves its `AssetHandle<Texture>` / `AssetHandle<Shader>` deps. This
   **reverses** planset-5's "the mesh does not load its materials" rule — the
   deliberate point being to remove the one asset in veng that didn't eager-load its
   dependencies, making the asset graph uniform. Generators take an optional
   `AssetHandle<Material>` instance (not an id); an empty handle leaves the submesh
   `SubMesh::NoMaterial`. The bind model is unchanged — the app draws each submesh's
   range, binding the material the submesh indexes.

7. **Materials are asset-sourced instances, not hand-built.** `Material::Create`
   exists but `MaterialInfo` needs a fully-built pipeline + reflected fields, so the
   practical material a runtime mesh carries is a loaded asset (`AssetHandle<Material>`).
   A runtime-authored (non-asset) `Material` ergonomic path is out of scope.

8. **`Mesh` stays vertex-layout-general; only indices get a typed wrapper.** veng
   ships one canonical layout, but a `Mesh` already carries a runtime
   `VertexBufferLayout`, and games can author **custom** vertex layouts (there is a
   `VertexLayout` asset type, and shaders reference a `VertexLayoutId`). So the
   mesh's vertex buffer stays a raw `Ref<Buffer>` described by that
   `VertexBufferLayout` — **not** a `VertexBuffer<CanonicalVertex>`, which would bake
   a compile-time vertex type into the mesh and preclude custom layouts. The index
   buffer, however, adopts the typed `IndexBuffer` wrapper: index width is the
   `IndexType` vocabulary enum (runtime, untemplated), so it is layout-agnostic and
   fits any mesh. Generating primitives in the canonical layout is this planset's
   job; cooking/loading meshes in *custom* layouts (importer emitting an arbitrary
   layout, loader validating against a referenced `VertexLayout` asset, material↔mesh
   layout agreement) is a separate future planset the `Mesh` type already
   accommodates.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| Mesh material model: `SubMesh::MaterialIndex` + a resident `AssetHandle<Material>` list on `Mesh` | A scene/level model that places meshes |
| `MeshLoader` eager-resolving cooked material ids → the list | A runtime mesh *editor* / CSG / mesh ops |
| `CanonicalVertex` + `MeshData` (CPU geometry, public) | A runtime-authored (non-asset) `Material` ergonomic path |
| Mesh index storage on the typed `IndexBuffer`; vertex stays raw `Ref<Buffer>` + `VertexBufferLayout` (layout-general) | **Custom vertex layouts end-to-end** (cooker emit + loader validate against a referenced `VertexLayout` asset) |
| `Mesh::Create(context, MeshData, name)` GPU upload factory | An `AssetId`-addressable "primitive asset" type |
| `Primitives::Cube` / `Plane` / `Sphere` generators (take a material instance) | Cylinder, cone, torus, capsule (easy follow-ons) |
| Pure-logic unit tests for the generators | LOD / index optimization / vertex welding |
| Sample demo drawing a runtime primitive with a material | Changing the cooked **on-disk** mesh format (stays id-based) |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Mesh runtime material model](01-mesh-material-model.md) | Replace `SubMesh`'s `AssetId Material` with a `u32 MaterialIndex` into a resident `vector<AssetHandle<Material>>` on `Mesh`; migrate `MeshLoader` to eager-resolve cooked submesh ids → that list (reversing planset-5's lazy rule); update the sample to a per-submesh material draw. Cooked on-disk format unchanged. | done |
| 02 | [Runtime mesh geometry + upload factory](02-mesh-data-upload.md) | Add public `CanonicalVertex` + `MeshData` (geometry + material list + indexed submeshes); adopt the typed `IndexBuffer` for `Mesh`'s index storage (vertex stays layout-general `Ref<Buffer>` + `VertexBufferLayout`), migrating `MeshLoader` + sample binds; a new `Mesh::Create(Renderer::Context&, const MeshData&, const string&)` overload (impl in a new `src/Asset/Mesh.cpp`) that uploads via `UploadSync`, carries the materials, defaults a whole-range unassigned submesh, returns `Ref<Mesh>`. | proposed |
| 03 | [Primitive generators + unit tests](03-primitive-generators.md) | New public `Primitives.h` / `src/Asset/Primitives.cpp`: `Cube`, `Plane`, `Sphere` returning `MeshData` with analytic normals/tangents/UVs and an optional `AssetHandle<Material>`. Pure-logic unit suite (`tests/unit/primitives.cpp`); add the header to `include_hygiene`. | proposed |
| 04 | [Sample demo + roadmap/docs](04-sample-demo-docs.md) | Migrate `examples/hello-triangle` to draw a runtime primitive (e.g. `Primitives::Sphere`) carrying the brick material instance, exercised end-to-end on GPU; update `plans/README.md`, `CLAUDE.md`, and the asset docs. | proposed |

## Dependency & order

01 → 02 → 03 → 04, strictly: 01 fixes the mesh material model the rest assumes; 02's
`MeshData`/factory + index-storage cleanup build on it; 03's generators return the
`MeshData` 02 defines and work through 02's factory; 04 demos what 03 produces. No
edges to planset-6.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the
same pass (plans 01 and 04 touch it) → verify (clean build, `ctest` green, smoke
binary writes a correct-sized PPM) → update this table → one commit per plan,
`Plan NN: <summary>` with a `Co-Authored-By` trailer (`planset-7:` for roadmap-only
edits).

- **Plans 01, 02, 04 create or draw GPU resources** and must pass the `VE_DEBUG`
  validation check (run the relevant binary from `build-debug/`, grep stderr for
  `Vulkan validation` ERROR). The upload/draw path reuses `Buffer`/`UploadSync` and
  the existing material bind, already validation-clean, so no allowlist change is
  expected — no plan may widen it.
- **Plan 03 is pure CPU** — its tests run under `ctest -L unit`, driver-free.
- **Delegation.** The generator bodies and their unit tests (03) are good
  `model: sonnet` subagent work once 02's `MeshData` shape is fixed; keep the mesh
  material-model change (01), the `MeshData`/factory + index-storage contract (02),
  and the sample migration (04) on the main thread.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

Runtime geometry construction exists alongside the cooked-mesh path, and the mesh's
runtime material model is uniform with the rest of the asset graph (resident
instances, indexed submeshes). Update [plans/README.md](../README.md) with the
planset-7 line and its status, and note in the asset docs / `CLAUDE.md` that the
planset-5 "mesh does not load its materials" rule is superseded — the mesh now
eager-loads and owns its materials like every other asset owns its dependencies. No
future-area re-cut is needed — this planset is not a chain item — but the
editor/scene future docs may reference `Primitives` as available groundwork.
