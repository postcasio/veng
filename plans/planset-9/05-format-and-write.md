# Plan 05 — Format v2 + cooker writes hashes

> **Stream C (archive content hashes), plan 1 of 2.** Independent of streams A (01–02)
> and B (03–04) — it touches only `assetformat` + the cooker; see the README's
> *Dependencies & dispatching* section.

**Goal:** add a per-blob content hash and a table-of-contents digest to the `.vengpack`
format (bumping `ArchiveFormatVersion` to 2), have `assetformat` serialize/round-trip
them **without computing them**, and have the cooker compute (xxh3-128) and write them.
The engine loader reads the wider TOC but ignores the hash — no behaviour change.

## Why this is its own plan

The format field is the format-locking change the whole concern hinges on; everything
else (`vengc verify`, dedup, incremental) builds on it. Landing the field + the cooker
write path first fixes the on-disk layout and the producer, so plan 06's `verify` has
real hashes to check.

## `assetformat` — `Archive.h` / `Archive.cpp`

A 16-byte hash value (raw bytes; `assetformat` never computes one — decision 3):

```cpp
// A 128-bit content hash, stored raw. assetformat carries these bytes; the cooker and
// the verify tool compute them (xxh3-128). Zero means "unhashed".
struct ContentHash
{
    u64 Lo = 0;
    u64 Hi = 0;
};
```

Format changes (the on-disk layout comment block updated to match):

- **Header** gains a `ContentHash ArchiveDigest` after `count` — a **table-of-contents**
  integrity value (decision 1; see "Cooker" for exactly what it covers).
- **Each TOC entry** gains a `ContentHash Hash` after `size` (the `flags` field stays
  reserved).
- `ArchiveTocEntry` (public view) and `InternalTocEntry` (reader-internal) both gain
  `ContentHash Hash`; `ArchiveReader` exposes the stored header digest
  (`[[nodiscard]] ContentHash ArchiveDigest() const`) **and the raw serialized TOC bytes**
  (`[[nodiscard]] std::span<const u8> TocBytes() const`) so `verify` can recompute the
  digest over the exact on-disk bytes — `assetformat` still computes no hash itself
  (decision 3); it only hands out the bytes.
- `inline constexpr u32 ArchiveFormatVersion = 2;` — a v1 archive now `VersionMismatch`es
  on load (existing behaviour); there is **no v1 reader** (decision 4: re-cook).
- Adding `Hash` to `ArchiveTocEntry` changes a **public** `assetformat` struct's layout — an
  ABI break for any consumer of `Entries()`. This is intentional and bounded: every pack is
  cooked at build time from the same tree, so consumers re-cook (v2) and recompile in the
  same step. (Stream A's relocatable *shipped* packs are still build outputs cooked by the
  same `vengc`; there is no externally-distributed v1 pack to strand.)

**The on-disk structs and their `static_assert`s must grow** (`Archive.cpp`, anonymous
namespace — the memcpy'd layouts): `OnDiskHeader` gains the 16-byte `ArchiveDigest` after
`Count`, so `static_assert(sizeof(OnDiskHeader) == 16)` → **`== 32`**; `OnDiskTocEntry`
gains the 16-byte `Hash` after `Size`, so `static_assert(sizeof(OnDiskTocEntry) == 32)` →
**`== 48`**. The writer/reader offset arithmetic already flows from `sizeof(OnDiskHeader)`
/ `sizeof(OnDiskTocEntry)` (the `tocBytes` / `blobRegionStart` math), so it updates
mechanically once the structs change — no offset constants are hand-coded.

The **writer takes the hashes; it does not make them**:

```cpp
// hash is the caller-computed xxh3-128 of `blob` (cooker-side). assetformat stores it.
// Defaulted so the existing non-cooker callers (the unit tests in tests/unit/
// asset_archive.cpp & asset_manager.cpp) compile unchanged — a zero hash is "unhashed",
// which the format already defines. Only the cooker passes a real hash. (verify treats a
// zero/unhashed entry as a failure precisely because every cooked archive is fully
// hashed; the defaulted overload exists only for non-cooker test fixtures, which verify
// never runs on.)
void Add(AssetId id, AssetType type, std::span<const u8> blob, ContentHash hash = {});

// The table-of-contents digest the caller computed over the serialized TOC bytes
// (decision 1; see "Cooker" below for the exact byte range).
void SetArchiveDigest(ContentHash digest);
```

`Build()`/`Write()` emit the digest into the header and each entry's `Hash` into the TOC.
`assetformat` gains **no dependency** — it serializes 16-byte fields and nothing more.

## Cooker — compute + write

- **Vendor xxHash** cooker-side, **pinned to a tag** (e.g. `v0.8.2`) via `FetchContent`
  like every other dep — no floating version. It is single-header but **not** header-only
  by default: it follows the stb pattern, so define `XXH_IMPLEMENTATION` in exactly **one**
  cooker `.cpp` before `#include "xxhash.h"` (the rest just include the header). Gated
  behind `VENG_BUILD_TOOLS` — never linked into `libveng`.
- For each cooked blob, compute **xxh3-128** over the blob bytes and pass it to
  `ArchiveWriter::Add(id, type, blob, hash)`.
- Compute the **archive digest** = xxh3-128 over the **serialized TOC byte region** — the
  contiguous run of `OnDiskTocEntry` records (each carrying `id`, `type`, `offset`, `size`,
  and the per-blob `Hash`) between the header and the blob region — then
  `SetArchiveDigest(...)`. This is a genuinely complementary check: re-hashing each blob
  already detects any *blob* corruption, while the TOC digest catches tampering the
  per-blob pass cannot — an entry reordered, an `id`/`type`/`offset`/`size` altered, or a
  per-blob `Hash` field edited to cover a swapped blob. Hashing the **serialized bytes
  in their on-disk order** also removes the ordering fragility a hash-of-hashes would have:
  `Build()` writes the entries id-sorted, and `verify` re-hashes the *same on-disk bytes*,
  so the two agree by construction with no separate sort step. (Compute it after the TOC
  bytes are laid out but before they are written, or over the just-written TOC buffer; the
  `ArchiveDigest` header field itself is **not** part of the hashed range.)
- **Re-cook** every pack to v2 (all rebuild at build time via `vengc`): the engine **core**
  pack (embedded into `libveng`), the **sample** pack, and the **`test_shaders`** pack
  (`tests/shaders/test.vengpack.json` → `test_shaders.vengpack`, loaded by `veng_gpu` /
  `veng_compute_dispatch`). The re-cook is transitive through the build graph (each pack
  `DEPENDS` on `vengc`), but name all three so none is missed.

## Engine loader — reads, ignores

The only loader-visible change is the wider TOC entry; `ArchiveReader` populates `Hash`
but the engine asset loaders (`MeshLoader`, texture/shader/material) never read it. The
runtime trusts its packs and does not hash on load (decision 2). No engine asset code
changes.

## Acceptance

- Clean build (cooker vendors xxHash); `ctest` green, including the existing cooker
  round-trip cases (they live in `veng_unit`, run under `ctest -L unit` — there is no
  separate `cooker` label); `include_hygiene` green (`Archive.h` gained only a POD field +
  accessor, no dep).
- The core, sample, and `test_shaders` packs cook to v2; `ArchiveReader::Entries()` shows a
  non-zero `Hash` per asset and a non-zero `ArchiveDigest()`.
- The headless smoke loads the v2 sample pack and renders unchanged — `HT_SMOKE` writes a
  correct-sized PPM (1280×720 RGB ≈ 2,764,816 bytes), exits 0. The engine reads v2 with no
  behaviour change.
- `assetformat` links no hash library; xxHash is present only under `VENG_BUILD_TOOLS`.
