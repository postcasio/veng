# Plan 02 — BC7 block compression (the format-and-encoder core)

**Goal:** cook textures to **BC7** — add the BC7 `Renderer::Format`s + type-mapping + a MoltenVK
capability **enable + gate**, encode each mip to BC7 in the cooker, and **upload the block-compressed
mips directly** (no GPU decode). Reuses the multi-mip blob + multi-region upload from Plan 01; the per-mip
size derivation becomes **block-aware**. Persists ~8:1 vs RGBA8 through to VRAM and sampling bandwidth.
hello-triangle is **not** migrated here — BC7 is exercised through a dedicated fixture so every
intermediate commit stays green; the migration (to ASTC) is Plan 04. **Depends on Plan 01.**

## Why it is its own plan

This is the format-and-encoder core. It introduces the **first compressed formats** into the vocabulary
enum + `TypeMapping`, the **first cooker encoder dependency**, the **device-capability enable + gate**,
and the **block-size-aware upload math** — each a discrete, reviewable seam. ASTC (Plan 03) then slots a
second encoder + format family into this same machinery and flips the default, so getting the
abstraction right here is what keeps that plan small.

## What lands

- **BC7 formats, at pinned ordinals.** Append `BC7Unorm` and `BC7Srgb` to `Renderer::Format` (the enum is
  `enum class Format : u8`, appended per the cooked-blob integer-stability rule). The current enum ends at
  `RGBA16Uint = 20`, so the new ordinals are **`BC7Unorm = 21`, `BC7Srgb = 22`**. The cooked
  `CookedTextureHeader.Format` (a `u32`) stores these integers; the cooker writes the literal and the
  loader's `BridgeFormat` reads it, so **both sides hand-sync to these exact ordinals** (the established
  cycle-avoidance pattern, the same way RGBA8 is `2`/`3` today). `TypeMapping::ToVk` →
  `eBc7UnormBlock` / `eBc7SrgbBlock`.

- **A `Format` block-info helper** (`Veng/Renderer/FormatInfo.h`): block extent (BC7 = 4×4), block byte
  size (16), and `BytesForLevel(format, w, h) = ceil(w/bw)·ceil(h/bh)·blockBytes`. An **uncompressed**
  format reports a 1×1 block, so Plan 01's `w·h·4` derivation becomes the special case of this **one**
  helper — the upload region math and the loader's per-level offset walk are then format-agnostic. The
  header still needs no offset table.

- **Capability enable + gate.** `textureCompressionBC` is a core `VkPhysicalDeviceFeatures` boolean: it
  must be **enabled at `createDevice`** (when the physical device supports it) for sampling a BC image to
  be legal — querying is necessary but not sufficient. `Context` enables it conditionally in its
  device-creation features struct (mirroring the `gpuDrivenCullingSupported` pattern), and
  `Context::IsBlockCompressionSupported()` reflects the **enabled** state. Because the cooked blob *is*
  BC7 and the runtime does **not** transcode, a device lacking BC is a **hard** condition: the texture
  loader returns a recoverable `AssetError::Unsupported` and logs once, rather than crashing or
  CPU-decoding. MoltenVK exposes `textureCompressionBC` on **Apple Silicon**; an Intel-Mac / non-BC
  device fails the gate (and the GPU BC tests skip there).

- **BC7 encoder in the cooker.** A pinned `FetchContent` encoder (`bc7enc_rdo`, a self-contained
  C++ encoder), **cooker-only behind `VENG_BUILD_TOOLS`** like stb / assimp / Slang — never linked into
  `libveng`. It encodes each generated RGBA8 mip to BC7 blocks at a documented quality preset (record the
  exact preset in the cooker, since golden stability depends on it). Partial edge blocks on
  non-multiple-of-4 mips are padded by edge replication (standard), so the full chain down to 1×1
  encodes.

- **Codec-selection seam, BC7-aware, sRGB-aware.** `TextureImporter` gains a cook-side codec value (a
  minimal internal seam, **not** the per-texture / per-pack authoring UX, which stays deferred). With BC7
  the only codec so far, the seam selects BC7, choosing **`BC7Srgb`** when the source is sRGB and
  **`BC7Unorm`** otherwise — matching the existing RGBA8Unorm/Srgb split. (Plan 03 adds ASTC to the seam
  and makes it the default.)

- **Loader bridges + uploads BC.** `TextureLoader::BridgeFormat` gains `case 21 → BC7Unorm`,
  `case 22 → BC7Srgb`; the loader sets `ImageInfo.MipLevels` and uploads the block bytes through Plan 01's
  multi-region upload, whose region sizes now flow through `BytesForLevel`. No `GenerateMipmaps` (a
  compressed image cannot blit).

## Decisions

1. **BC7, both sRGB and unorm.** The cook picks the encoding by the texture's sRGB flag, mirroring the
   existing format pair. BC5 (normals) / BC4 (single-channel masks) and any per-texture codec choice are
   **deferred** to the developer-control plan (area 15).

2. **One block-info helper serves every format.** `BytesForLevel` + block extent unify uncompressed and
   block sizes, so upload-region math and the loader's level walk are format-agnostic and Plan 01's
   derivation is the 1×1-block case. Adding ASTC is a *row* in this helper, nothing more.

3. **A compressed format is enabled, not just queried.** `textureCompressionBC` is added to the enabled
   device features when supported; `IsBlockCompressionSupported()` reflects the enabled state. Sampling a
   BC image without the enable is a validation error / UB.

4. **BC support is a runtime requirement, not a transcode.** A BC-cooked pack assumes
   `textureCompressionBC`; an unsupported device gets a logged `AssetError::Unsupported`, never a crash
   and never a CPU decode. Choosing the codec per target device, and an uncompressed fallback pack, are
   the **deferred** developer-control / per-platform-cook work.

5. **Compressed mips are cooked, never GPU-generated.** Reinforces Plan 01's seam — the
   `GenerateMipmaps` blit path is unreachable for BC and stays scoped to runtime uncompressed textures.

6. **Exercise via fixture, don't migrate the sample.** hello-triangle stays RGBA8 here; a dedicated BC7
   fixture texture drives the cook + load + sample test. Migrating the sample (to ASTC) and regenerating
   the lossy golden is Plan 04, so the smoke golden does not move in this plan and every commit is green.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Types.h` | Append `BC7Unorm = 21`, `BC7Srgb = 22` (documented as appended for cooked-blob integer stability). |
| `engine/include/Veng/Renderer/Backend/TypeMapping.h` | `ToVk` cases for the two BC7 formats. |
| `engine/include/Veng/Renderer/FormatInfo.h` (new) | Block extent + `BytesForLevel`; uncompressed = 1×1 block. Refactor Plan 01's size math onto it. |
| `engine/include/Veng/Renderer/Context.h`, `engine/src/Renderer/Backend/Context.cpp` | Enable `textureCompressionBC` at device creation when supported; `IsBlockCompressionSupported()` reflecting the enabled state. |
| `engine/src/Asset/Loaders/TextureLoader.cpp` | `BridgeFormat` `case 21/22`; block-aware level offsets; `AssetError::Unsupported` when the device lacks BC. |
| `engine/src/Asset/Texture.cpp`, `engine/src/Renderer/Backend/Image.cpp` | Region sizing through `BytesForLevel`; no blit for compressed. |
| `cooker/CMakeLists.txt` | `FetchContent` `bc7enc_rdo` at a pinned tag, cooker-only. |
| `cooker/src/Importers/TextureImporter.cpp` | BC7-encode each mip; sRGB-aware format selection; the codec-selection seam; write `header.Format = 21/22` (hand-synced to the `Renderer::Format` ordinals). |
| `tests/…` | Cooker: a BC7 fixture cooks to `Format` `21`/`22` with the expected per-level block sizes; a **round-trip** test that the cooker's emitted integer bridges back through `BridgeFormat` to the matching `Renderer::Format` (guards an ordinal transposition). GPU (BC-capable, else skip): a BC7 cooked fixture loads and samples. |

## Verification

- Clean build; `ctest` green — the GPU BC tests **skip** on a non-BC device (`SKIP_RETURN_CODE 77`).
- `validation_gate` green on a BC-capable device — BC image creation (with the feature enabled) + the
  multi-region block upload raise no validation error; the test exercises a **non-multiple-of-4 mip** so
  the partial-edge-block `bufferRowLength`/`imageExtent` path is validated (the classic block-copy trap).
- `smoke_golden` does **not** move — hello-triangle stays RGBA8; BC7 is exercised by the fixture only.
- `include_hygiene` unaffected — `FormatInfo.h` is header-only over `Veng.h`/`Types.h`, no backend
  include.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
