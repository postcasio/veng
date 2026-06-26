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
- **`.vengpack` is at format v5 and carries content hashes.** Each cooked blob has a
  content hash and the table of contents has a digest. `assetpack` **stores the raw
  16 bytes and computes nothing** — the hash function lives only in the cooker and
  `vengc verify`, so this library (and `libveng`) gain no hash dependency. The runtime
  loader **never verifies**; hashing is a tooling concern, checkable with `vengc
  verify`.
- **`CookedProject` (`.vengproj`) is the runtime entrypoint of a managed game.** A small
  hand-rolled binary file (`Veng/Asset/CookedProject.h`, format v1: magic `VENGPROJ`,
  version, pack count, startup-level `AssetId`, then length-prefixed pack mount names) the
  cook writes per build configuration. `ReadCookedProject`/`WriteCookedProject` are plain
  byte IO — no JSON in the runtime. The engine reads it to mount the named packs and load
  the startup level; the pack archive header itself carries **no** startup level (it is a
  project fact, not a pack fact).
- **Blobs are stored zstd-compressed or raw, per blob.** Each TOC entry carries an
  `ArchiveCodec` (`Stored`/`Zstd`) and an `UncompressedSize`. The **cooker** compresses
  each blob and stores whichever of the raw or compressed bytes is smaller; `assetpack`
  **chooses nothing** — it stores the codec the cooker passed and inflates a `Zstd` entry
  **lazily on the first `Find`** into a stable-address, id-keyed, never-evicted cache, so a
  span stays valid for the reader's lifetime. zstd is the one runtime codec dependency
  (linked PUBLIC); the content hash and the TOC digest cover the **stored** bytes, so
  `vengc verify` re-hashes the on-disk bytes with no decode. `Find` is main-thread-only;
  the lazy inflate is not synchronized.
- A version number the format actually checks (the on-disk `v5`) is rejected loudly on
  mismatch — a stale/foreign archive does not load silently. The version bump is **global** —
  every `.vengpack`, including the **embedded core pack** the engine ships via `#embed`. A
  format change re-cooks the core pack, but ccache does **not** track an `#embed`-ed file as a
  dependency, so a stale core-pack object can mount an older pack into a `v5` runtime and fail
  every run with "asset … not found"; rebuild the embed with `CCACHE_DISABLE=1` (or delete the
  embed `.o`) after a format bump.
- **`AssetType::Texture` carries a mipped, block-compressed image.** A **`CookedTextureHeader`**
  (`Format`, `Width`, `Height`, `MipCount`) is followed by the mip levels **tightly packed,
  largest-first** — no offset table, since each level's byte size derives from its halved
  dimensions and the format's block geometry. `Format` is a `Renderer::Format` integer that may
  be a block-compressed codec (BC7 / ASTC 4×4) as well as an uncompressed format; `assetpack`
  treats the level bytes as opaque and computes nothing from the format. A single-mip texture is
  the degenerate one-level case.
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
- **`AssetType::Environment` carries an equirectangular HDR panorama for image-based lighting.**
  A **`CookedEnvironmentHeader`** (`CookedEnvironmentVersion`) is a `Format` (always the
  `RGBA16Sfloat` ordinal), `Width`, and `Height`, followed by `Width * Height` half-float texels
  (row-major, top-to-bottom). The runtime uploads it as an HDR panorama texture and generates the
  IBL cubemaps from it on the GPU; the loader rejects a `Version` mismatch.
