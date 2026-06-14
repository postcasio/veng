# Plan 02 — Runtime mesh geometry + upload factory

**Goal:** the runtime-geometry foundation. Add a public, GPU-free `MeshData`
(canonical-layout vertices + u32 indices + a resident material list + an indexed
submesh table) and a `CanonicalVertex` struct, adopt the typed `IndexBuffer`
wrapper for `Mesh`'s index storage (the vertex buffer stays a layout-general
`Ref<Buffer>` + `VertexBufferLayout`), then add a `Mesh::Create` overload that
uploads a `MeshData` into the same `Ref<Mesh>` the cooked-asset loader returns. No
primitives yet — this plan is the data type, the index-storage cleanup, and the
upload path the generators (plan 03) build on. Builds on plan 01's material model.

## Why this is its own plan

It fixes the one contract everything else depends on: the CPU geometry struct and
how it becomes a GPU `Mesh`. Getting the layout/offset asserts and the
buffer/material/submesh defaults right here means plan 03 is pure math and plan 04
is a one-liner.

## Public surface — `Mesh.h`

Add beside the existing `CanonicalLayout()`:

```cpp
// One interleaved vertex in the canonical layout (48 bytes). Field order,
// sizeof, and offsets are statically asserted to match CanonicalLayout():
// position @0, normal @12, tangent @24, uv @40. Tangent is a vec4 — xyz the
// tangent, w the bitangent handedness sign (±1), reconstructed in-shader as
// cross(N, T.xyz) * T.w.
struct CanonicalVertex
{
    vec3 Position;
    vec3 Normal;
    vec4 Tangent;
    vec2 UV;
};

// CPU-side mesh geometry in the canonical layout. Plain data — primitive
// generators and tests build it with no GPU. Upload it into a GPU Mesh with
// Mesh::Create(context, data, name).
struct MeshData
{
    vector<CanonicalVertex> Vertices;
    vector<u32> Indices;
    // Resident materials the produced Mesh will own; submeshes index this list
    // (plan 01's model). Empty = the mesh has no materials.
    vector<AssetHandle<Material>> Materials;
    // Each submesh is a draw range + a MaterialIndex into Materials
    // (SubMesh::NoMaterial = unassigned). Empty → the factory synthesizes one
    // unassigned submesh over [0, Indices.size()).
    vector<SubMesh> SubMeshes;
};
```

And the new factory declaration on `Mesh`:

```cpp
[[nodiscard]] static Ref<Mesh> Create(
    Renderer::Context& context, const MeshData& data, const string& name);
```

The existing `Mesh::Create(const MeshInfo&)` (pre-built buffers, used by
`MeshLoader`) stays as the lower-level overload — but its `MeshInfo` index field
changes to the typed `IndexBuffer` (below).

### Mesh storage — typed index buffer, layout-general vertex buffer

`Mesh` hand-rolls its index buffer as a `Ref<Buffer>` plus a separate `IndexType` +
`IndexCount`. Adopt the typed `Renderer::IndexBuffer` wrapper there — index width is
the `IndexType` vocabulary enum (runtime, untemplated), so the wrapper is
layout-agnostic and fits any mesh:

- `MeshInfo`: `Renderer::IndexBuffer IndexBuffer;` replaces `Ref<Buffer> IndexBuffer`
  and drops `IndexType` + `IndexCount` — the `IndexBuffer` carries both.
- `Mesh`: `GetIndexBuffer()` returns `const Renderer::IndexBuffer&`;
  `GetIndexType()`/`GetIndexCount()` delegate to it (keep them; cheap, used by draw
  code).
- `Mesh.h` includes `Renderer/TypedBuffers.h` (pulls `Buffer`/`CommandBuffer`/
  `DescriptorSet`/`Types` — all public; `include_hygiene` stays green).

**The vertex buffer stays a raw `Ref<Renderer::Buffer>` described by the mesh's
existing `VertexBufferLayout Layout`** — *not* a `VertexBuffer<CanonicalVertex>`. A
`Mesh`'s vertex format is a **runtime** layout (`Mesh` already carries one, there is
a `VertexLayout` asset type, and shaders reference a `VertexLayoutId`), and games
can author custom layouts; `VertexBuffer<V>` exists to fix a *compile-time* vertex
type and would bake the canonical assumption into the mesh, precluding that. So:
`MeshInfo::VertexBuffer` and `Mesh::GetVertexBuffer()` keep their `Ref<Buffer>`
type; the layout travels in `MeshInfo::Layout` as today.

This is a change to the **existing** `Mesh`/`MeshInfo` index API, so it migrates
`MeshLoader` and the sample in the same pass (below).

### Layout assertions

In the header (or the new `.cpp`), lock the struct to the layout so a future field
or glm-alignment change is a loud compile error, not silent vertex corruption:

```cpp
static_assert(sizeof(CanonicalVertex) == 48);
static_assert(offsetof(CanonicalVertex, Position) == 0);
static_assert(offsetof(CanonicalVertex, Normal)   == 12);
static_assert(offsetof(CanonicalVertex, Tangent)  == 24);
static_assert(offsetof(CanonicalVertex, UV)       == 40);
```

veng does not force glm aligned gentypes, so `vec3`/`vec4`/`vec2` pack to 4-byte
alignment and these offsets hold. If a build ever enables aligned gentypes the
asserts fire first; the fallback is `f32 Position[3]; …` (what the cooker's private
`CanonicalVertex` already uses) — but keep the glm spelling unless an assert forces
the change.

## Impl — new `engine/src/Asset/Mesh.cpp`

Mesh is header-only today; this plan adds its first `.cpp` (mirroring
`Asset/Texture.cpp` / `Asset/Material.cpp`).

`Mesh::Create(context, data, name)`:

1. `VE_ASSERT` non-empty vertices and indices, every index `< Vertices.size()`, and
   every submesh `MaterialIndex` either `NoMaterial` or `< Materials.size()` (a
   generator/app bug is misuse → fatal, per the error policy — not a `Result`).
2. Build the submesh table: if `data.SubMeshes` is empty, synthesize one
   `SubMesh{ .IndexOffset = 0, .IndexCount = (u32)data.Indices.size(),
   .MaterialIndex = SubMesh::NoMaterial }`; otherwise copy as given —
   **preserving each submesh's `MaterialIndex`**. Carry `data.Materials` straight
   into `MeshInfo::Materials`; the factory never invents or drops a material
   reference.
3. Vertex buffer (raw, layout-general): `Buffer::Create(context, { .Name = name +
   " Vertices", .Size = Vertices.size() * sizeof(CanonicalVertex), .Usage =
   BufferUsage::Vertex | BufferUsage::TransferDst })`, then `UploadSync` the vertex
   bytes (`reinterpret` the `vector<CanonicalVertex>` as a byte span).
4. Index buffer (typed): `auto ib = Renderer::IndexBuffer::Create(context, name +
   " Indices", Indices.size());` (defaults `IndexType::U32`) then
   `ib.UploadSync(std::span<const u32>(Indices));` — no usage flags, no byte math.
5. `return Mesh::Create(MeshInfo{ .Name = name, .VertexBuffer = vertexBuffer,
   .IndexBuffer = std::move(ib), .Layout = Mesh::CanonicalLayout(),
   .Materials = data.Materials, .SubMeshes = … });`

The materials arrive as resolved handles rather than cooked ids; the layout is set
to `CanonicalLayout()` (generated geometry is canonical), so there is no header
parsing or attribute validation.

### Migrate `MeshLoader.cpp` to the typed index buffer

`MeshLoader` keeps parsing/validating the cooked blob and creating the **vertex**
buffer the same way (raw `Ref<Buffer>` + the validated canonical `Layout`). Only its
**index** buffer switches to `IndexBuffer::Create` + `UploadSync` of the u32 index
span, and the `MeshInfo` it fills drops the separate `IndexType` / `IndexCount`.
(Plan 01's material resolution is unaffected.)

### Migrate the sample binds

The vertex bind is unchanged (`cmd.BindVertexBuffer(mesh.GetVertexBuffer())` — the
raw `Ref<Buffer>` overload). The index bind uses the typed `CommandBuffer` overload:
`cmd.BindIndexBuffer(mesh.GetIndexBuffer())` — the explicit `mesh.GetIndexType()`
argument goes away (the `IndexBuffer` supplies it).

Add `src/Asset/Mesh.cpp` to `engine/CMakeLists.txt`.

## Tests

No new test file required this plan — plan 03's generators exercise the factory on
GPU through the unit/gpu suites. Optionally add a tiny GPU case asserting buffer
sizes and material pass-through from a hand-built two-triangle `MeshData`, but the
generator tests cover it.

## Acceptance

- Clean build; `ctest` green.
- `include_hygiene` still builds (the new public symbols pull in only glm + existing
  public headers `Mesh.h` already includes, incl. `Material.h` from plan 01 and
  `TypedBuffers.h` for `IndexBuffer`).
- A hand-built `MeshData` → `Mesh::Create(context, data, "x")` yields a `Ref<Mesh>`
  with correct `GetIndexCount()`, one submesh, its materials carried onto
  `GetMaterials()`, and the canonical layout.
- `Mesh` exposes a typed `IndexBuffer` (vertex stays `Ref<Buffer>` +
  `VertexBufferLayout`); `MeshLoader` and the sample build/bind through it; no
  hand-computed index `IndexType`/`IndexCount` bookkeeping remains in the mesh path.
- `VE_DEBUG` validation clean for any GPU exercise of the new path.
