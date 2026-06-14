# Plan 02 — `assetformat`: AssetId, the archive container, cooked-blob layouts

**Goal:** define the **one source of format truth** that the cooker writes and the
engine reads. `libveng_assetformat` owns `AssetId`, `AssetType`, the `.vengpack`
binary container (header + table-of-contents + blob region) with a pure
**reader and writer**, and the per-type cooked-blob struct layouts. It is
**Vulkan-free, importer-free, and engine-independent** — it includes no
`Renderer/` header and links no backend.

## Why this is its own plan

The format is a contract between two libraries built by two later plans (cooker,
loader). Pinning it first — with its own round-trip unit tests, before either
consumer exists — means 03 and 04 are written against a settled, tested format
rather than co-evolving it. It is also the one piece that must stay rigorously
free of engine/Vulkan coupling; isolating it makes that boundary easy to guard.

## The container format (`.vengpack`)

```
Header
  char  magic[8]   "VENGPACK"
  u32   version
  u32   count                 // number of TOC entries
TOC[count]                    // sorted by AssetId for binary-search lookup
  u64   id                    // AssetId
  u32   type                  // AssetType
  u32   flags                 // reserved (0)
  u64   offset                // from start of blob region
  u64   size
Blob region
  concatenated cooked blobs, each opaque to the container, typed by its TOC entry
```

```cpp
namespace Veng
{
    struct AssetId { u64 Value = 0; [[nodiscard]] bool IsValid() const { return Value != 0; } };
    // hashable / comparable; 0 is the reserved "invalid" id (so a pack must not use 0).

    enum class AssetType : u32 { Raw = 0, Texture, Mesh, Shader, Material };
}
```

- **`AssetId` is opaque** — no derivation rule. The cooker mints random non-zero
  `u64`s (or honours ids written in the pack); the format never interprets them.
- **Lookup** is binary search over the id-sorted TOC → a `span<const u8>` into the
  blob region (the reader keeps the whole archive in one allocation, or mmaps it).

## Cooked-blob layouts (the shared structs)

Each asset type's cooked bytes have a fixed, versioned header struct defined here
so the cooker (writer) and engine (reader) agree. v1 layouts:

```cpp
// Texture: stb-decoded, single mip v1 (mip table reserved for later)
struct CookedTextureHeader { u32 Format; u32 Width, Height; u32 MipCount; /* =1 */ };
//   followed by raw pixel bytes
// Mesh: interleaved vertices + indices in a declared layout
struct CookedMeshHeader   { u32 VertexStride, VertexCount, IndexCount, IndexType;
                            u32 SubMeshCount; /* + layout descriptor + submesh table */ };
struct CookedSubMesh      { u32 IndexOffset, IndexCount; u64 MaterialId; /* AssetId */ };
// Shader: reflected interface + SPIR-V (ShaderInterface serialization defined in plan 08)
struct CookedShaderHeader { u32 InterfaceBytes; u32 SpirvBytes; /* + interface + spirv */ };
// Material: shader ref + params + texture bindings (defined in plan 09)
struct CookedMaterialHeader { u64 ShaderId; u32 TextureBindingCount; u32 ParamBytes; /* … */ };
```

**The cycle-avoidance rule (load-bearing):** enum-typed fields (`Format`,
`IndexType`, shader stages) are stored as their **underlying integer type** here —
`assetformat` deliberately does **not** include `Renderer/Types.h`. The engine
loader casts to the `Renderer::` vocabulary enum guarded by a
`static_assert`/`VE_ASSERT` (loud one-line fix on drift, per house style). This is
what keeps `assetformat` standalone and the cooker buildable without the engine.

## Work

1. **Headers** under `assetformat/include/Veng/Asset/` — `AssetId.h`,
   `AssetType.h`, `Archive.h` (container layout + `ArchiveReader`/`ArchiveWriter`),
   `CookedBlobs.h` (the per-type header structs above). Use only `Veng.h` aliases
   (`u32`, `u64`, `span`, `vector`, `string`) — depend on the engine's `Veng.h`
   foundational header **only if** it carries no Renderer/Vulkan include (it
   doesn't); otherwise vendor the minimal aliases.
2. **`ArchiveWriter`** — `Add(AssetId, AssetType, span<const u8> blob)` then
   `Write(path)` (or to a buffer): emits header, id-sorted TOC, blob region.
3. **`ArchiveReader`** — `Open(path)`/`FromBytes(span)`; `Find(AssetId)` →
   `optional<{AssetType, span<const u8>}>`; `Entries()` for tooling/inspection.
   All `Result<>`/`optional`-based, no exceptions (`-fno-exceptions`, house rule).
4. **CMake** — `libveng_assetformat` as a small static lib; the engine links it
   **PUBLIC** (plan 04 surfaces `AssetId` in the public API); add it to the
   `include_hygiene` header sweep so the standalone/no-Vulkan boundary is guarded.
5. **Tests** — a `tests/assetformat` doctest exe (pure, no GPU): write a multi-
   entry archive, read it back, assert byte-exact blob round-trip, id lookup hits
   and misses, `version`/`magic` rejection, and TOC sort/`Find` on a large id set.

## Dependencies

Plan 01 (the `assetformat/` subdir + top-level CMake must exist). Blocks 03 and 04.

## Acceptance

- Clean build, `ctest` green incl. the new `assetformat` round-trip tests.
- `include_hygiene` compiles `assetformat`'s public headers while linking **no**
  Vulkan/backend — proving the standalone boundary.
- `grep -rn "Renderer/" assetformat/include` is empty (no engine coupling).

## Notes

- **Endianness / alignment:** v1 assumes the cook host and run host share both
  (true for the macOS dev box). Note little-endian + natural alignment as a
  documented v1 assumption; a portable-archive pass is future, not now.
- Keep the format **versioned from day one** (`version` in the header, a
  `k_FormatVersion` constant) so a `VersionMismatch` is a clean reader-side error
  (plan 04's `AssetError`), not a crash.
- The cooked-blob *headers* are defined here; their *production* (cooker) and
  *consumption* (engine loaders) land per-type in 06/07/08/09, each filling in the
  reserved fields (mips, layout descriptor, interface bytes) as that type arrives.
