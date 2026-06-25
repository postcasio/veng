# Plan 01 — offline texture mip chain

**Goal:** generate a full **mip chain offline** in the texture cooker, store every level in the cooked
blob, and **upload all levels** at load — replacing today's single-mip-only restriction
([`TextureImporter.cpp`](../../cooker/src/Importers/TextureImporter.cpp) rejects `generate_mips`, and
[`TextureLoader.cpp`](../../engine/src/Asset/Loaders/TextureLoader.cpp) rejects `MipCount != 1`). Still
emits **uncompressed RGBA8**; it lays the multi-mip blob layout + multi-region upload the block
compression in Plan 02 needs. Depends on nothing in this track; **Plan 02 depends on it.**

## Why it is its own plan

Mips are a prerequisite for block compression both in value (a single-mip BC texture aliases badly) and
in mechanism: a **compressed image cannot be GPU-blit-mipgen'd** — `vkCmdBlitImage` requires blit format
features that block-compressed formats lack — so a compressed texture's mips **must** be precomputed at
cook time. Landing the offline chain on the existing uncompressed format **first** proves the blob layout
and the multi-region upload in isolation, before the encoder enters the picture. The runtime already
carries `Image::MipLevels` + `GenerateMipmaps` (a GPU blit) for *runtime-built* textures; this plan adds
the **cooked, precomputed** mip path beside it.

## What lands

- **Multi-mip blob layout.** `CookedTextureHeader` already carries `MipCount`; the blob becomes the
  header followed by the mip levels **tightly packed, largest first**. For an uncompressed format each
  level's byte size derives from its dimensions (`max(1, W>>i) · max(1, H>>i) · 4`), so **no offset
  table** is needed — the loader walks the levels arithmetically. (Plan 02 generalizes the size
  derivation to block formats through one helper, so the header never needs an offset array.)

- **Cooker generates the chain.** `TextureImporter` drops the `generate_mips`-is-unsupported rejection
  and instead generates the chain **by default**, halving with `stbir_resize_uint8_srgb` /
  `_linear` (sRGB-correct for sRGB sources, linear otherwise) down to 1×1, setting
  `MipCount = floor(log2(max(W, H))) + 1` and appending every level. `generate_mips: false` opts back
  out to a single mip.

- **Loader accepts a chain.** `TextureLoader` accepts `MipCount >= 1`, computes each level's offset/size,
  sets `TextureData.MipLevels` / `ImageInfo.MipLevels = MipCount`, and hands all levels to a new
  multi-mip upload.

- **A multi-region `CopyBufferToImage`.** Today `CommandBuffer::CopyBufferToImage(buffer, image)` records
  a **single** region into mip 0; `Image::UploadSync(span)` calls it then **GPU-generates** the rest when
  `MipLevels > 1`. This plan adds a **multi-region** copy entry point — a `CopyBufferToImage` overload (or
  sibling) taking explicit per-level regions (buffer offset, mip level, extent) — and an `UploadSync`
  overload that records **one `VkBufferImageCopy` region per level** from a single staging buffer and
  performs **no** `GenerateMipmaps`. The async `Upload` sibling gets the same multi-region treatment so
  streamed textures carry their cooked mips. The existing single-region copy + GPU-mipgen path stays for
  runtime-built (uncompressed) textures — primitives, adopted resources — where there is no cook step.

## Decisions

1. **Cooked mips, not GPU-generated, for cooked textures.** Offline mipgen is sRGB-correct,
   deterministic (golden-stable), and the **only** option once the format is block-compressed. The GPU
   `GenerateMipmaps` blit path stays scoped to runtime-built uncompressed textures.

2. **No per-mip offset table for uncompressed — sizes derive from dimensions.** Keeps the blob compact
   and the loader a simple arithmetic walk. Plan 02 swaps the per-level size derivation for a
   **block-aware** one (still a pure function of dimensions + format, no table), so the header layout is
   unchanged when compression arrives.

3. **One staging buffer, N copy regions.** All levels upload in a single `UploadSync`/`Upload`,
   mirroring how one mip uploads today — no GPU round-trip, no per-level submit.

4. **Default mips on; `generate_mips: false` opts out.** Reverses today's hard rejection; mips are the
   normal case and the opt-out is explicit.

## Files

| File | Change |
|---|---|
| `cooker/src/Importers/TextureImporter.cpp` | Remove the `generate_mips` rejection; generate + append the sRGB-/linear-correct chain; set `MipCount`. |
| `assetpack/include/Veng/Asset/CookedBlobs.h` | Document the multi-mip blob layout on `CookedTextureHeader` (level packing, largest-first). |
| `engine/src/Asset/Loaders/TextureLoader.cpp` | Accept `MipCount > 1`; drop the single-mip guard; build per-level spans. |
| `engine/include/Veng/Asset/Texture.h`, `engine/src/Asset/Texture.cpp` | `TextureData.MipLevels`; `Prepare` builds the per-level copy regions and passes `MipLevels` to `ImageInfo`. |
| `engine/include/Veng/Renderer/CommandBuffer.h`, `engine/src/Renderer/Backend/CommandBuffer.cpp` | A multi-region `CopyBufferToImage` taking explicit per-level regions (the existing single-region overload stays). |
| `engine/include/Veng/Renderer/Image.h`, `engine/src/Renderer/Backend/Image.cpp` | Multi-mip `UploadSync`/`Upload` recording one region per level via the new copy entry point (no `GenerateMipmaps` when mips are supplied). |
| `tests/…` | Cooker: a cooked texture carries the expected `MipCount`. GPU/loader: a multi-mip cooked texture loads with the right `MipLevels`. |

## Verification

- Clean build; `ctest` green; the smoke binary writes a correct-sized PPM.
- `smoke_golden`: if hello-triangle's textures gain mips the capture *may* shift (mip 0 sampling at the
  fixed smoke pose is usually unchanged); regenerate per the documented `HT_SMOKE` path only if the
  fuzzy compare trips.
- `include_hygiene` unaffected.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
