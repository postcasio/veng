# Plan 04 — zstd-compressed archive blobs

**Goal:** compress each cooked blob in the `.vengpack` with **zstd**, and have the runtime
`ArchiveReader` transparently inflate on resolve. Shrinks the on-disk pack across **every** asset type
— textures, meshes, prefabs — with no per-asset settings and no format-specific work. The archive
container bumps to **format v3**. Independent of the texture-format track (Plans 05–07); can land in
parallel.

## Why it is its own plan

It is a **container-level** change, orthogonal to texture formats: it touches only `assetpack` (the
writer and the reader) and the cooker's blob-emit step, and it benefits meshes and prefabs as much as
textures. Isolating it keeps the on-disk format bump *and* the one new **runtime** dependency (a
decoder must live in `libveng`, unlike the cooker-only hash function) reviewable on their own, before
any `Renderer::Format` work begins. It mirrors the existing content-hash design — written by the
cooker, stored opaquely by `assetpack` — with the one unavoidable asymmetry that decompression is a
runtime concern.

## What lands

- **zstd as a pinned `FetchContent` dependency**, declared in the top-level `CMakeLists.txt` shared
  deps and linked **PUBLIC by `assetpack`** (the reader inflates at runtime, so the dependency reaches
  `libveng`; the cooker gets the compressor transitively). `assetpack` stays **Vulkan-free and
  importer-free** — zstd is a codec, not a source importer — and the public `Archive.h` gains **no**
  `<zstd.h>` include (the codec is a plain `u32`/enum field; all zstd calls live in `Archive.cpp`), so
  `include_hygiene` is unaffected.

- **Archive format v3** (`ArchiveFormatVersion = 3`). The per-TOC-entry **`flags`** field (today a
  reserved `0`) becomes the **compression codec** (`0` = stored, `1` = zstd), and the TOC entry gains
  a **`u64 UncompressedSize`**. The blob region stores the **compressed** bytes; the entry's existing
  `Size` is the compressed length and `UncompressedSize` the inflated length. A v2 archive is rejected
  loudly by the existing version check — a stale pack must be re-cooked, which is fine for a build
  artifact.

- **Write side: the cooker compresses.** When emitting the archive, the cooker zstd-compresses each
  blob, **picks the smaller** of stored-vs-compressed (an already-incompressible blob — a future
  already-BC texture — stores raw), and passes the codec + both sizes to `ArchiveWriter::Add`.
  `assetpack` stores the bytes and the codec and **computes nothing** on write, exactly as it does for
  the hash. The content hash and the TOC digest are taken over the **stored** (on-disk) bytes, so
  `vengc verify` re-hashes precisely what is on disk with **no** decode path.

- **Read side: `assetpack` decompresses.** `ArchiveReader::Find` returns a `std::span<const u8>` into
  reader-owned storage. A **stored** entry keeps today's zero-copy span into `m_Storage`; a
  **compressed** entry is inflated **lazily on first `Find`** into a reader-owned, id-keyed cache and
  the span points into that cache (never evicted, so the `ArchiveEntry` lifetime contract — valid for
  the reader's lifetime — holds). Lazy-on-resolve keeps mounting cheap and inflates only what a run
  actually loads.

## Decisions

1. **Compress in the cooker, decompress in `assetpack`.** The split mirrors the hash design (the hash
   function is cooker-only) but cannot be symmetric: the runtime *must* inflate to use a blob, so zstd
   lands in `assetpack` as the one new runtime dependency. That is justified because decoding is
   genuinely a runtime concern; hashing is not.

2. **Per-blob, not whole-region.** Each blob compresses independently, so the reader inflates only the
   entries it resolves, random-access lookup is preserved, and the **stored-vs-zstd choice is per
   entry** — an incompressible blob keeps the raw, zero-copy path. A single compressed region would
   force inflating the whole pack on mount and defeat lazy resolve.

3. **`flags` carries the codec; the TOC entry grows `UncompressedSize`.** The reserved field finally
   earns its name, and the inflated length is needed at decode. Both are plain integers in the public
   header — no zstd type leaks into `Archive.h`.

4. **Hash over stored bytes.** The content hash and TOC digest cover exactly the on-disk bytes, so
   `vengc verify` stays a pure byte re-hash with no zstd dependency, and the digest still covers the
   (now compressed) sizes.

## Files

| File | Change |
|---|---|
| `CMakeLists.txt` (root) | `FetchContent` zstd at a pinned tag, as a shared dep. |
| `assetpack/include/Veng/Asset/Archive.h` | `ArchiveFormatVersion = 3`; document the codec on `flags` (a plain enum/`u32`); add `UncompressedSize` to `ArchiveTocEntry`; `ArchiveWriter::Add` takes the codec + uncompressed size; document the reader's decompress-on-resolve behavior. **No `<zstd.h>`.** |
| `assetpack/src/Archive.cpp` | `OnDiskTocEntry` gains `UncompressedSize`; the writer stores the codec/sizes; the reader inflates a compressed entry lazily into an id-keyed owned cache (stored entries keep the zero-copy span). |
| `assetpack/CMakeLists.txt` | Link zstd PUBLIC. |
| `cooker/src/Cooker.cpp` | zstd-compress each blob, pick the smaller of stored-vs-compressed, pass the codec + sizes to `Add`; hash over the stored bytes. |
| `cooker/src/Verify.cpp` | Confirm verify re-hashes the on-disk (`Size`-length, compressed) bytes — no decode added. |
| `tests/unit/…` (assetpack) | Round-trip a compressible blob (write → read → bytes equal); a stored-fallback case for an incompressible blob; assert a v2 archive still rejects. |

## Verification

- Clean build; `ctest` green. The new assetpack round-trip case passes (label `unit`, no device).
- `vengc verify` green on the re-cooked sample pack; the sample `.vengpack` is materially smaller on
  disk.
- Smoke render unaffected (decompression is lossless) — the golden does not move from this plan.
- `include_hygiene` unaffected — `Archive.h` gains only an integer field + a codec enum, no zstd in any
  public signature.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
