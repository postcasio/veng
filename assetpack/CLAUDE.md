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
