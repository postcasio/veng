# Plan 00 — zstd-compressed archive blobs

**Goal:** compress each cooked blob in the `.vengpack` with **zstd**, and have the runtime
`ArchiveReader` transparently inflate on resolve. Shrinks the on-disk pack across **every** asset type
— textures, meshes, prefabs — with no per-asset settings and no format-specific work. The archive
container bumps to **format v3**. Independent of the texture-format track (Plans 01–03); can land in
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
  `include_hygiene` is unaffected. Pin a concrete tag (zstd `v1.5.6`) rather than a floating ref.

- **Archive format v3** (`ArchiveFormatVersion = 3`). The on-disk TOC entry's reserved **`flags`** field
  (today `u32 flags // reserved (0)` in the layout, not yet a struct member) becomes the **compression
  codec** (`0` = stored, `1` = zstd), and the entry gains a **`u64 UncompressedSize`**. These are **new
  fields added to the on-disk `OnDiskTocEntry` *and* surfaced on the public `ArchiveTocEntry`** — the
  reserved `flags` lives only in the format comment today, so this is an additive struct change, not the
  population of a pre-existing member. The blob region stores the **compressed** bytes; the entry's
  existing `Size` is the compressed length and `UncompressedSize` the inflated length. A v2 archive is
  rejected loudly by the existing version check — a stale pack must be re-cooked (see core-pack note
  below).

- **`OnDiskTocEntry` grows by 8 bytes; update its `static_assert`.** `Archive.cpp` asserts
  `sizeof(OnDiskTocEntry) == 48`; adding `u64 UncompressedSize` makes it **56** — bump the assert in the
  same change, so the on-disk layout shift stays loud.

- **Write side: one blob-emit helper in the cooker.** The cooker has two `ArchiveWriter::Add` call sites
  (the single-asset path and the manifest walk). Route **both** through one helper that zstd-compresses
  each blob, **picks the smaller** of stored-vs-compressed (an already-incompressible blob — a future
  already-block-compressed texture — stores raw), and passes the codec + both sizes to `Add`. A missed
  site would silently ship an uncompressed `flags=0` blob that still loads, so the single helper is what
  makes "every blob is considered for compression" true by construction.

- **`ArchiveWriter::Add` gains defaulted codec + uncompressed-size params.** Its `ContentHash hash = {}`
  default exists so non-cooker callers (test fixtures) compile unchanged; the new params follow the same
  rule — `Add(id, type, blob, hash = {}, codec = Stored, uncompressedSize = blob.size())` — so existing
  fixtures keep building and only the cooker passes the codec/sizes. `assetpack` stores the bytes and the
  codec and **computes nothing** on write, exactly as it does for the hash.

- **Hash over stored bytes.** The content hash and the TOC digest are taken over the **stored** (on-disk)
  bytes, so `vengc verify` re-hashes precisely what is on disk with **no** decode path.

- **Read side: `assetpack` decompresses lazily.** `ArchiveReader::Find` returns a
  `std::span<const u8>` into reader-owned storage. A **stored** entry keeps today's zero-copy span into
  `m_Storage`; a **compressed** entry is inflated **lazily on first `Find`** into a reader-owned,
  id-keyed cache and the span points into that cache. The cache uses a container with **stable value
  addresses** (e.g. `map<AssetId, vector<u8>>`) so a span stays valid across a later inflating `Find`,
  and entries are **never evicted**, so the `ArchiveEntry` lifetime contract (valid for the reader's
  lifetime) holds. The reader carries the codec + `UncompressedSize` per entry internally to drive the
  inflate. `Find` is **main-thread-only today** (every loader dereferences the span on the render thread
  during `Load()`); document that invariant as a `@warning` on the cache so a future worker-thread
  `Find` does not silently race the lazy inflate.

- **Re-cook the embedded core pack.** The v3 bump applies to **every** `.vengpack`, including the
  **embedded core pack** the engine ships via `#embed`. The build re-cooks it, but ccache does **not**
  capture an `#embed`-ed file as a dependency ([[project_ccache_embed_staleness]]), so a stale core-pack
  object can mount a v2 pack into a v3 runtime and fail every run with "asset … not found." Verification
  rebuilds the embed with the documented `CCACHE_DISABLE=1` / delete-the-embed-`.o` workaround.

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
   becomes a real struct member, and the inflated length is needed at decode. Both are plain integers in
   the public header — no zstd type leaks into `Archive.h`.

4. **Hash over stored bytes.** The content hash and TOC digest cover exactly the on-disk bytes, so
   `vengc verify` stays a pure byte re-hash with no zstd dependency, and the digest still covers the
   (now compressed) sizes.

5. **Doubled host memory is accepted.** A long-lived reader that resolves a compressed entry holds the
   compressed bytes in `m_Storage` *and* the inflated copy in the cache. The extra is only the retained
   compressed copy — the inflated blob is the same RGBA8/mesh bytes resolved before compression — so peak
   host memory rises by roughly the on-disk pack size, which is acceptable for a build artifact mounted
   once.

## Files

| File | Change |
|---|---|
| `CMakeLists.txt` (root) | `FetchContent` zstd at pinned tag `v1.5.6`, as a shared dep. |
| `assetpack/include/Veng/Asset/Archive.h` | `ArchiveFormatVersion = 3`; add the codec field (a plain enum/`u32`) and `u64 UncompressedSize` to `ArchiveTocEntry`; `ArchiveWriter::Add` takes defaulted codec + uncompressed size; document the reader's decompress-on-resolve behavior + the main-thread-only `@warning`. **No `<zstd.h>`.** |
| `assetpack/src/Archive.cpp` | `OnDiskTocEntry` gains `UncompressedSize`; bump `static_assert(sizeof(OnDiskTocEntry) == 56)`; the writer stores the codec/sizes; the reader inflates a compressed entry lazily into a stable-address id-keyed owned cache (stored entries keep the zero-copy span). |
| `assetpack/CMakeLists.txt` | Link zstd PUBLIC. |
| `cooker/src/Cooker.cpp` | One blob-emit helper through which **both** `Add` sites route: zstd-compress, pick the smaller of stored-vs-compressed, pass the codec + sizes; hash over the stored bytes. |
| `cooker/src/Verify.cpp` | Confirm verify re-hashes the on-disk (`Size`-length, compressed) bytes — no decode added. |
| `tests/unit/…` (assetpack) | Round-trip a compressible blob (write → read → bytes equal); a stored-fallback case for an incompressible blob; a span held across a second inflating `Find` stays valid; assert a v2 archive still rejects. |

## Verification

- Clean build; `ctest` green. The new assetpack round-trip case passes (label `unit`, no device).
- The embedded core pack mounts: rebuild the embed with `CCACHE_DISABLE=1` (or delete the embed `.o`)
  after the format bump, and confirm a smoke run loads core shaders (a stale v2 embed would fail to
  mount). [[project_ccache_embed_staleness]]
- `vengc verify` green on the re-cooked sample pack; the sample `.vengpack` is materially smaller on
  disk.
- Smoke render unaffected (decompression is lossless) — the golden does not move from this plan.
- `include_hygiene` unaffected — `Archive.h` gains only an integer field + a codec enum, no zstd in any
  public signature (confirm zstd as a transitive PUBLIC dep of `assetpack` pulls no `<zstd.h>` into the
  hygiene compile).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
