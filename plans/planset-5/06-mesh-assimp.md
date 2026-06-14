# Plan 06 — Mesh (assimp cook → engine load)

**Goal:** the mesh type. The cooker imports a model with **assimp**, flattens it to
interleaved vertices + indices in veng's vertex layout, and writes a
`CookedMeshHeader` + buffers + submesh table (each submesh referencing a material
by `AssetId`). The engine registers a `Mesh` loader that creates the two buffers
via `UploadSync`; the sample draws a cooked mesh.

## Why this is its own plan

Mesh introduces the one heavy dependency (assimp), the vertex-layout-on-disk
question, and cross-asset references (submesh → material id) — each worth isolating
from the texture slice. It copies plan 05's cook/load shape exactly, so the novelty
is the data, not the plumbing.

## Cook side (`libveng_cook`)

```jsonc
{ "id": 1002, "type": "mesh", "source": "meshes/cube.obj" }
```

- A `MeshImporter : AssetImporter`. `Cook`: `aiImportFile` with
  `aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
  aiProcess_GenNormals | aiProcess_CalcTangentSpace | aiProcess_FlipUVs`
  (as needed) → flatten meshes into one interleaved vertex buffer in **veng's
  canonical vertex layout** (position, normal, tangent, uv — fixed v1) + a u32
  index buffer + a `CookedSubMesh[]` table. Material references: v1 stores
  `MaterialId = 0` (unassigned) unless the pack/material-binding is provided
  explicitly; assimp material → veng material mapping is a later refinement.
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
  the `SubMesh` table. Submesh material ids are kept as `AssetId` — the mesh does
  **not** eagerly load materials here (the sample loads materials explicitly in
  plan 08/09; eager dependency loading is available via `AssetManager` but a mesh
  with `MaterialId = 0` has none to load).

## Work

1. Cooker: assimp `FetchContent` + `MeshImporter` + register; a small `.obj`/glTF
   fixture under `tests/`.
2. Engine: `Mesh` + `SubMesh` types, `MeshLoader`, register; the layout validation.
3. Sample: cook a mesh into the sample pack; `LoadSync<Mesh>(id)`; draw it
   (bind vertex/index buffers, `DrawIndexed` per submesh) with the existing
   pipeline (material binding is plan 08).
4. Tests: cook a known-vertex-count fixture, mount, `LoadSync<Mesh>`, assert
   vertex/index counts, layout match, and a buffer-size sanity check (consistent
   with the typed-buffer GPU cases).

## Dependencies

Plans 03, 04, and the precedent of 05. Needed by 09 (the scene). Not blocked by 08
(submesh→material is a forward id reference).

## Acceptance

- Clean build, `ctest` green incl. the mesh cook + load tests.
- Smoke binary writes a correct-sized PPM with a cooked mesh drawn.
- **Validation-clean** under `VE_DEBUG` for the mesh draw path.

## Notes

- **Fixed canonical vertex layout v1** (pos/normal/tangent/uv). A
  shader-derived/validated layout is plan 07's reflection consequence; here the
  cooked layout is validated against the engine's single canonical layout. When 07
  lands, mesh-vs-shader validation tightens (the shader's `VertexInputs` vs the
  mesh's layout) — additive.
- assimp is large; keep it strictly behind `VENG_BUILD_TOOLS` so a consumer of
  `libveng` never pays for it.
- u32 indices v1 (the `IndexType` field allows u16 later).
