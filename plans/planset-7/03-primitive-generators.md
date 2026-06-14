# Plan 03 — Primitive generators + unit tests

**Goal:** the generators. A new `Primitives` namespace builds `MeshData` for a cube,
plane, and UV sphere — analytic positions, normals, tangents (with handedness `w`),
and UVs in the canonical layout — each accepting an optional material **instance**
(`AssetHandle<Material>`) recorded on its submesh. Pure CPU geometry; unit-tested
with no GPU.

## Why this is its own plan

Plan 02 fixed the data type and upload; this plan is the geometry math. It is
self-contained, has no Vulkan, and its geometry correctness is fully checked by CPU
unit tests — a clean unit of work and a good `model: sonnet` delegation once the
`MeshData` contract is fixed.

## Public surface — new `engine/include/Veng/Asset/Primitives.h`

```cpp
#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>   // MeshData, CanonicalVertex

// Runtime primitive-mesh generators. Each returns CPU-side MeshData in the
// canonical vertex layout (Mesh::CanonicalLayout()); upload it with
// Mesh::Create(context, data, name). Geometry is generated with analytic
// normals, tangents (xyz + handedness w), and UVs. A valid `material` handle is
// recorded on the produced submesh (the mesh owns it; the draw loop binds it);
// an empty handle leaves the submesh unassigned (SubMesh::NoMaterial).
namespace Veng::Primitives
{
    // Axis-aligned cube centered at the origin, `extent` units across the full
    // width (so ±extent/2 per axis). 24 vertices (4 per face, hard normals),
    // 36 indices, one submesh. Per-face UVs span [0,1].
    [[nodiscard]] MeshData Cube(f32 extent = 1.0f, AssetHandle<Material> material = {});

    // Flat plane in the XZ plane (+Y normal) centered at the origin, `size`
    // units wide/deep, tessellated into `subdivisions` quads per axis (min 1).
    // UVs span [0,1] across the plane.
    [[nodiscard]] MeshData Plane(vec2 size = vec2(1.0f),
                                 uvec2 subdivisions = uvec2(1),
                                 AssetHandle<Material> material = {});

    // UV sphere of `radius`, `rings` latitude bands and `segments` longitude
    // bands (min 3 each). Smooth normals; UVs are (longitude, latitude). One
    // submesh.
    [[nodiscard]] MeshData Sphere(f32 radius = 0.5f,
                                  u32 rings = 16, u32 segments = 32,
                                  AssetHandle<Material> material = {});
}
```

Each generator finishes its single submesh by material state: a **valid** `material`
handle → `data.Materials = { material }` and `SubMesh{ …, .MaterialIndex = 0 }`; an
**empty** handle → `data.Materials` empty and `.MaterialIndex = SubMesh::NoMaterial`.
Either way there is exactly one explicit submesh, and a specified material rides
through plan 02's factory onto the mesh's resident list unchanged.

## Impl — new `engine/src/Asset/Primitives.cpp`

Geometry notes (all in the canonical layout, CCW front faces, Y-up, right-handed —
matching the sample's `glm::lookAt`/`perspective` and `projection[1][1] *= -1`):

- **Cube** — 6 faces × 4 unique verts (hard edges: each face has its own normal and
  tangent). Per face: normal = face axis, tangent = the face's U direction with
  `w = +1` (UVs are not mirrored, so handedness is uniform), UV the unit square.
- **Plane** — `(subdivisions+1)²` grid verts on XZ; normal `+Y`; tangent `+X`,
  `w = +1`; UV = grid position normalized. Two triangles per cell, CCW from above.
- **Sphere** — standard latitude/longitude parametrization. Position from
  (theta, phi); normal = normalized position; tangent = d(position)/d(longitude)
  normalized (the `+U` direction), `w = +1`; UV = (phi/2π, theta/π). Pole rings
  handled so no degenerate triangles slip in.

Clamp `subdivisions`/`rings`/`segments` to their minimums with `VE_ASSERT`-free
`std::max` (a 0 here is a caller convenience, not misuse). Add
`src/Asset/Primitives.cpp` to `engine/CMakeLists.txt`.

## Tests — new `tests/unit/primitives.cpp` (label `unit`, no GPU)

Add to the `veng_unit` source list in the top-level `CMakeLists.txt`. The geometry
is fully CPU-checkable; the *populated*-material path needs a real
`AssetHandle<Material>` (manager + GPU), so it's covered by plan 04's demo (and
optionally a GPU case) — unit tests exercise the default (no-material) wiring. For
each primitive:

- **Counts** — vertices and indices match the formula; `Indices.size() % 3 == 0`.
- **In-bounds** — every index `< Vertices.size()`.
- **Submesh** — exactly one; `IndexOffset == 0`; `IndexCount == Indices.size()`.
- **No-material default** — with no material arg, `Materials` is empty and the
  submesh's `MaterialIndex == SubMesh::NoMaterial`.
- **Normals** — unit length (within epsilon).
- **Tangents** — `xyz` unit length and roughly orthogonal to the normal; `w` is ±1.
- **Bounds** — the position AABB matches the requested size (cube `extent`,
  plane `size`, sphere `radius`).
- **Sphere/plane params** — vertex count scales with `rings`/`segments` /
  `subdivisions`.

These don't need the GPU and don't touch the factory; they pin the math.

## Acceptance

- Clean build; `ctest -L unit` green (new `primitives.*` cases discovered).
- `include_hygiene` builds with `Primitives.h` added.
- `Mesh::Create(context, Primitives::Sphere(), "s")` produces a drawable
  `Ref<Mesh>` (proven by the plan 04 demo).
