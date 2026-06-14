# Plan 07 — Mesh (JSON source + import settings + material overrides → assimp cook)

**Goal:** the mesh type. A mesh has its **own JSON source file** (like textures and
materials) that names the model binary, declares **import settings**, and assigns
**material overrides** (submesh/slot → material `AssetId`). The cooker imports the
model with **assimp** per those settings, flattens it to interleaved vertices +
indices in veng's vertex layout, and writes a `CookedMeshHeader` + buffers +
submesh table (each submesh carrying its resolved material id). The engine
registers a `Mesh` loader that creates the two buffers via `UploadSync`; the sample
draws a cooked mesh.

## Why this is its own plan

Mesh introduces the one heavy dependency (assimp), the vertex-layout-on-disk
question, and cross-asset references (submesh → material id). It copies plan 06's
JSON-source + cook/load shape, so the novelty is the data and assimp's options,
not the plumbing.

## The mesh JSON source

```jsonc
// pack entry
{ "id": 1002, "type": "mesh", "source": "meshes/cube.mesh.json" }
```
```jsonc
// meshes/cube.mesh.json
{
  "model": "cube.obj",            // binary, relative to this file
  "import": {
    "scale": 1.0,
    "flip_uv": false,
    "generate_normals": true,
    "generate_tangents": true,
    "join_identical_vertices": true
  },
  "materials": { "0": 1003 }       // submesh index (or assimp material slot) → material AssetId
}
```

Import settings map to assimp post-process flags at cook time; `materials` resolves
into each `CookedSubMesh`'s `MaterialId`. A submesh with no override gets
`MaterialId = 0` (unassigned).

## Cook side (`libveng_cook`)

- A `MeshImporter : AssetImporter`. `Cook`: parse the JSON, `aiImportFile` the
  model (relative to the JSON dir) with the post-process flags built from `import`
  → flatten meshes into one interleaved vertex buffer in **veng's canonical vertex
  layout** (position, normal, tangent, uv — fixed v1) + a u32 index buffer + a
  `CookedSubMesh[]` table, applying the `materials` overrides to each submesh's
  `MaterialId`.
- The on-disk layout descriptor in `CookedMeshHeader` records the interleaved
  format so the loader can validate it against the engine's `VertexBufferLayout`.
- assimp via `FetchContent` (pinned), **cooker-only** — never reaches the engine.

## Load side (`libveng`)

```cpp
struct SubMesh { u32 IndexOffset, IndexCount; AssetId Material; };
class Mesh
{
public:
    [[nodiscard]] Ref<Buffer>              GetVertexBuffer() const;
    [[nodiscard]] Ref<Buffer>              GetIndexBuffer() const;
    [[nodiscard]] const VertexBufferLayout& GetLayout() const;
    [[nodiscard]] span<const SubMesh>       GetSubMeshes() const;
};
```

- A `MeshLoader : AssetLoader`. `Load`: read header; **validate the cooked layout
  against the engine's canonical `VertexBufferLayout`** (a loud `Corrupt` error on
  mismatch, not silent UB); `Buffer::Create` ×2 (vertex/index) + `UploadSync`; build
  the `SubMesh` table from the cooked submesh entries. Submesh material ids stay as
  `AssetId` — the mesh does **not** eagerly load materials here (the sample/scene
  loads materials explicitly; eager dependency loading is available via
  `AssetManager` if a caller wants it).

## Work

1. Cooker: assimp `FetchContent` + `MeshImporter` (JSON source + import flags +
   material overrides) + register; a small `.obj`/glTF fixture + its `.mesh.json`
   under `tests/`.
2. Engine: `Mesh` + `SubMesh` types, `MeshLoader`, register; the layout validation.
3. Sample: author a `cube.mesh.json`, cook it into the sample pack;
   `LoadSync<Mesh>(id)`; draw it (bind vertex/index buffers, `DrawIndexed` per
   submesh) with the existing pipeline (material binding is plan 09).
4. Tests: cook a known-vertex-count fixture, mount, `LoadSync<Mesh>`, assert vertex/
   index counts, layout match, submesh material-id overrides applied, and a
   buffer-size sanity check (consistent with the typed-buffer GPU cases).

## Dependencies

Plans 03, 04, and the precedent of 06. Needed by 10 (the scene). Not blocked by 09
(submesh→material is a forward id reference).

## Acceptance

- Clean build, `ctest` green incl. the mesh cook + load tests.
- Smoke binary writes a correct-sized PPM with a cooked mesh drawn.
- **Validation-clean** under `VE_DEBUG` for the mesh draw path.

## Notes

- **Fixed canonical vertex layout v1** (pos/normal/tangent/uv). A
  shader-derived/validated layout is plan 08's reflection consequence; here the
  cooked layout is validated against the engine's single canonical layout. When 08
  lands, mesh-vs-shader validation tightens (the shader's `VertexInputs` vs the
  mesh's layout) — additive.
- assimp is large; keep it strictly behind `VENG_BUILD_TOOLS` so a consumer of
  `libveng` never pays for it.
- u32 indices v1 (the `IndexType` field allows u16 later).
- **Material overrides** are how a single model file feeds different materials per
  scene; v1 is a static per-mesh-asset assignment, not a per-instance one.
