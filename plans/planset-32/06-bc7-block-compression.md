# Plan 06 — BC7 block compression (the default)

**Goal:** cook textures to **BC7** by default — add the BC7 `Renderer::Format`s + type-mapping + a
MoltenVK capability gate, encode each mip to BC7 in the cooker, and **upload the block-compressed mips
directly** (no GPU decode). Reuses the multi-mip blob + multi-region upload from Plan 05; the per-mip
size derivation becomes **block-aware**. Persists ~8:1 vs RGBA8 through to VRAM and sampling bandwidth.
**Depends on Plan 05.**

## Why it is its own plan

This is the format-and-encoder core. It introduces the **first compressed formats** into the vocabulary
enum + `TypeMapping`, the **first cooker encoder dependency**, the **device-capability gate** (BC
support is Apple-Silicon-only under MoltenVK), and the **block-size-aware upload math** — each a
discrete, reviewable seam. ASTC (Plan 07) then slots a second encoder + format family into this same
machinery, so getting the abstraction right here is what keeps that plan small.

## What lands

- **BC7 formats.** Append `BC7Unorm` and `BC7Srgb` to `Renderer::Format` (appended, per the enum's
  cooked-blob integer-stability rule), with `TypeMapping::ToVk` → `eBc7UnormBlock` / `eBc7SrgbBlock`.

- **A `Format` block-info helper** (`Veng/Renderer/FormatInfo.h`): block extent (BC7 = 4×4), block byte
  size (16), and `BytesForLevel(format, w, h) = ceil(w/bw)·ceil(h/bh)·blockBytes`. An **uncompressed**
  format reports a 1×1 block, so Plan 05's `w·h·4` derivation becomes the special case of this **one**
  helper — the upload region math and the loader's per-level offset walk are then format-agnostic. The
  header still needs no offset table.

- **Capability gate.** `Context::IsBlockCompressionSupported()` queries the `textureCompressionBC`
  device feature. Because the cooked blob *is* BC7 and the runtime does **not** transcode, a device
  lacking BC is a **hard** condition: the texture loader returns a recoverable
  `AssetError::Unsupported` and logs once, rather than crashing or CPU-decoding. The cook target (BC vs
  ASTC vs uncompressed) is a **pack/platform decision**; a BC-cooked pack assumes a BC-capable runtime.
  MoltenVK exposes `textureCompressionBC` on **Apple Silicon**; an Intel-Mac / non-BC device fails the
  gate (and the GPU BC tests skip there).

- **BC7 encoder in the cooker.** A pinned `FetchContent` encoder (`bc7enc_rdo`, a self-contained
  C++ encoder), **cooker-only behind `VENG_BUILD_TOOLS`** like stb / assimp / Slang — never linked into
  `libveng`. It encodes each generated RGBA8 mip to BC7 blocks at a sane quality default. Partial edge
  blocks on non-multiple-of-4 mips are padded by edge replication (standard), so the full chain down to
  1×1 encodes.

- **Cook default: BC7, sRGB-aware.** Every texture cooks to BC7, choosing **`BC7Srgb`** when the source
  is sRGB and **`BC7Unorm`** otherwise — matching the existing RGBA8Unorm/Srgb split. The codec is a
  cook-side **default constant**; the per-texture / per-pack authoring knob is **deferred** ("developer
  control later"). The chosen `Format` integer is stored in the header exactly as today (the BC7
  integers hand-synced to the `Renderer::Format` ordinals, the established cycle-avoidance pattern).

- **Loader bridges + uploads BC.** `TextureLoader::BridgeFormat` gains the BC7 cases; the loader sets
  `ImageInfo.MipLevels` and uploads the block bytes through Plan 05's multi-region upload, whose region
  sizes now flow through `BytesForLevel`. No `GenerateMipmaps` (a compressed image cannot blit).

## Decisions

1. **Default BC7, both sRGB and unorm.** The cook picks the encoding by the texture's sRGB flag,
   mirroring the existing format pair. BC5 (normals) / BC4 (single-channel masks) and any per-texture
   codec choice are **deferred** to the developer-control plan.

2. **One block-info helper serves every format.** `BytesForLevel` + block extent unify uncompressed and
   block sizes, so upload-region math and the loader's level walk are format-agnostic and Plan 05's
   derivation is the 1×1-block case. Adding ASTC is a *row* in this helper, nothing more.

3. **BC support is a runtime requirement, not a transcode.** A BC-cooked pack assumes
   `textureCompressionBC`; an unsupported device gets a logged `AssetError::Unsupported`, never a crash
   and never a CPU decode. Choosing the codec per target device, and an uncompressed fallback pack, are
   the **deferred** developer-control / per-platform-cook work.

4. **Compressed mips are cooked, never GPU-generated.** Reinforces Plan 05's seam — the
   `GenerateMipmaps` blit path is unreachable for BC and stays scoped to runtime uncompressed textures.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Types.h` | Append `BC7Unorm`, `BC7Srgb`. |
| `engine/include/Veng/Renderer/Backend/TypeMapping.h` | `ToVk` cases for the two BC7 formats. |
| `engine/include/Veng/Renderer/FormatInfo.h` (new) | Block extent + `BytesForLevel`; uncompressed = 1×1 block. Refactor Plan 05's size math onto it. |
| `engine/include/Veng/Renderer/Context.h`, `engine/src/Renderer/Backend/Context.cpp` | `IsBlockCompressionSupported()` (`textureCompressionBC` query). |
| `engine/src/Asset/Loaders/TextureLoader.cpp` | `BridgeFormat` BC7 cases; block-aware level offsets; `AssetError::Unsupported` when the device lacks BC. |
| `engine/src/Asset/Texture.cpp`, `engine/src/Renderer/Backend/Image.cpp` | Region sizing through `BytesForLevel`; no blit for compressed. |
| `cooker/CMakeLists.txt` | `FetchContent` the BC7 encoder, cooker-only. |
| `cooker/src/Importers/TextureImporter.cpp` | BC7-encode each mip; sRGB-aware format selection; the default-codec constant; the BC7 format integers hand-synced to `Renderer::Format`. |
| `tests/…` | Cooker: a texture cooks to a BC7 format with the expected per-level block sizes. GPU (BC-capable, else skip): a BC7 cooked texture loads and samples. |

## Verification

- Clean build; `ctest` green — the GPU BC tests **skip** on a non-BC device (`SKIP_RETURN_CODE 77`).
- `validation_gate` green on a BC-capable device — BC image creation + the multi-region block upload
  raise no validation error.
- `smoke_golden`: BC7 is **lossy**, so the capture moves — regenerate the golden on a **BC-capable**
  device via the documented `HT_SMOKE` path, and confirm it passes within the existing fuzz tolerance
  (widen only if BC7 artifacts demand it, and document the change). This regen is the responsibility of
  the migration closer (Plan 08).
- `include_hygiene` unaffected — `FormatInfo.h` is header-only over `Veng.h`/`Types.h`, no backend
  include.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
