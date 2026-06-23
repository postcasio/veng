# libveng_assetpack — the archive & cooked-blob format

`assetpack/` is `libveng_assetpack`, the shared on-disk format that sits between the
offline cooker and the runtime: the `.vengpack` archive and the cooked-blob layout
(`Veng/Asset/`: `AssetId`, `AssetType`, `Archive`, `CookedBlobs`). It is
**Vulkan-free and importer-free**, and is linked **PUBLIC by both `engine` and
`cooker`** — the one type both sides agree on. Project-wide conventions live in the
[root CLAUDE.md](../CLAUDE.md).

The cook that *writes* an archive is in [cooker/CLAUDE.md](../cooker/CLAUDE.md); the
runtime that *mounts and resolves* one (the `AssetManager`, `AssetHandle`, mounts) is
in [engine/CLAUDE.md](../engine/CLAUDE.md). This library is just the format and its
serialization — neither importer nor loader.

- **`AssetId` is an opaque `u64`.** Assets are addressed by id, never by path, at
  runtime. The same id is byte-identical across the cooker/runtime boundary (and
  across the module boundary for `TypeId`), so the cooker writes the ids the runtime
  reads.
- **An archive is built from a pure `{ id, type, source }` manifest.** The format
  carries no per-asset settings — those live in the per-asset JSON sources the
  manifest points at, consumed by the cooker, not by this library.
- **`.vengpack` is at format v2 and carries content hashes.** Each cooked blob has a
  content hash and the table of contents has a digest. `assetpack` **stores the raw
  16 bytes and computes nothing** — the hash function lives only in the cooker and
  `vengc verify`, so this library (and `libveng`) gain no hash dependency. The runtime
  loader **never verifies**; hashing is a tooling concern, checkable with `vengc
  verify`.
- A version number the format actually checks (the on-disk `v2`) is rejected loudly on
  mismatch — a stale/foreign archive does not load silently.
- **`AssetType::Level` is a world prefab by reference plus level-scoped wiring.** Its
  blob is a **`CookedLevelHeader`** (`CookedLevelVersion`, currently `1`) — `WorldPrefabId`
  (the world prefab's `AssetId`, resolved as a load-time dependency), `SystemCount`, and the
  two record sizes — followed by the ordered `u64[SystemCount]` `SystemId` set, then the
  game-mode config record, then the render-settings record. Each config record is the
  reflection serializer's name-keyed `WriteFields` encoding, which `assetpack` treats as
  **opaque bytes** exactly as a prefab blob treats a component record — so this library
  gains no reflection dependency, and a new game-mode or render-settings field evolves
  tolerantly within the fixed `CookedLevelVersion` (no bump). The loader rejects a blob whose
  `Version` mismatches.
- **`AssetType::Skeleton` and `AssetType::Animation` carry skinned-character rigs.** A
  **`CookedSkeletonHeader`** (`CookedSkeletonVersion`) is a `BoneCount` plus a column-major
  `GlobalInverse` mat4, followed by `CookedBone[BoneCount]` in topological (parent-before-child)
  order — each a parent index, a 64-byte name, an inverse-bind matrix, and a local bind TRS,
  all stored as raw `f32`/`i32` (no glm dependency). A **`CookedAnimationHeader`**
  (`CookedAnimationVersion`) is a `Duration` plus `CookedAnimChannel[]` (one per bone, with
  position/rotation/scale key counts + byte offsets) and a trailing key region of timed
  `CookedVec3Key`/`CookedQuatKey` runs. A **skinned mesh** sets `CookedMeshHeader.SkeletonId`
  (0 = static) and is written in the skinned vertex layout (the canonical attributes plus
  `RGBA16Uint` bone indices and `RGBA32Sfloat` weights); the attribute table is self-describing
  so the loader validates it against the engine's canonical *or* skinned layout by `SkeletonId`.
