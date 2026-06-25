# planset-33 — textures cook compressed and mipped

**Phase goal:** shrink packed-asset size and texture memory through three composable axes —
**zstd-compressed archive blobs** (every asset type, on-disk size), an **offline mip chain**, and
**ASTC/BC7 GPU block compression** (persisting ~8:1 through to VRAM and sampling bandwidth). The current
texture path cooks **raw, single-mip RGBA8** ([`TextureImporter.cpp`](../../cooker/src/Importers/TextureImporter.cpp)),
the `Renderer::Format` enum has **no compressed formats**, and the `.vengpack` stores blobs
**uncompressed** — so a 2048² albedo sits in the pack and in VRAM as a flat 16 MB. This planset attacks
that.

This work was originally drafted as "Track B" of planset-32 (render-allocation self-sizing); it shares
**no files** with that work and is its own coherent phase, so it lands as its own planset. The one place
the two phases meet is named below.

## The three axes

1. **On-disk size (every asset) — zstd archive blobs (Plan 00).** A container-level change in
   `assetpack` + the cooker, orthogonal to texture formats: each cooked blob is zstd-compressed (the
   cooker picks stored-vs-compressed per blob) and the reader inflates lazily on resolve. Helps meshes
   and prefabs too, but only shrinks the *download* — pixels still inflate to full RGBA8 in VRAM.

2. **A prerequisite — the offline mip chain (Plan 01).** Mips are generated at cook (sRGB-correct) and
   stored in the blob, uploaded as one copy region per level. This is required for block compression
   both in value (a single-mip compressed texture aliases) and in mechanism: a compressed image
   **cannot** be GPU-blit-mipgen'd (`vkCmdBlitImage` requires blit format features that block-compressed
   formats lack), so its mips must be precomputed. Landed first on uncompressed RGBA8 to prove the blob +
   upload in isolation.

3. **VRAM + bandwidth — ASTC/BC7 block compression (Plans 02–03).** The real win: a
   hardware-compressed format the GPU samples *directly*, ~8:1 that persists to sampling. **BC7 lands
   first** (Plan 02) as the format-and-encoder core; **ASTC** is added over the same machinery (Plan 03)
   and becomes the **default on the primary (MoltenVK) platform**, with BC7 selectable for the
   desktop/Windows target. The axes stack — an ASTC/BC7 blob still benefits from zstd on top.

## The codec default — ASTC on Mac, BC7 selectable

Under MoltenVK, `textureCompressionBC` is **Apple-Silicon only**, while `textureCompressionASTC_LDR` is
**broad on Apple GPUs** (the natively-Metal-blessed family); BC7 is the **desktop/Windows standard**. So
the engine's primary platform is best served by **ASTC as the cook default**, with **BC7 selectable**
through a minimal internal seam for the anticipated Windows port. A device lacking the cooked codec's
feature gets a logged `AssetError::Unsupported` — the runtime does **not** transcode.

**All developer control of the codec is deferred** to future **area 15 — build configurations & project
settings** ([`future/build-configurations.md`](../future/build-configurations.md)): a project owns
per-platform **build configurations** that hold the codec policy (a role → format table), a texture
declares a compression **role** rather than a raw codec, the cook-time config dependency is
implicit/coarse (one output pack per config), and the **editor gates preview to host capability** (build
any config, preview only what the host GPU can sample — so "ASTC on Windows" is structurally
impossible). Choosing the codec per device, an uncompressed fallback pack, BC5/BC4 channel
specialization, and wider ASTC footprints all live there. This planset only makes both codecs **cookable
and decodable** and hardcodes the ASTC-default / BC7-selectable seam.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Archive zstd compression | Per-blob zstd in the `.vengpack` (format **v3**: a new codec field on the TOC + a new `UncompressedSize`); the cooker compresses + picks stored-vs-zstd, `assetpack` inflates lazily on resolve. Shrinks every asset type on disk. Re-cooks the embedded core pack to v3. Independent of the rest. | done |
| 01 | Texture mip chain | Offline mip generation in the texture cooker (sRGB-/linear-correct), the multi-mip blob layout, and a multi-region upload (a new multi-region `CopyBufferToImage`) — replacing the single-mip-only restriction. Still uncompressed RGBA8; lays the infrastructure Plan 02 needs. | proposed |
| 02 | BC7 block compression | The format-and-encoder core: `BC7Unorm`/`BC7Srgb` formats + `TypeMapping` + a `FormatInfo` block helper + a `textureCompressionBC` **enable + gate**; a cooker-only BC7 encoder; block-aware upload. Exercised via a BC7 fixture (hello-triangle stays RGBA8 until Plan 04). Depends on 01. | proposed |
| 03 | ASTC block compression | `ASTC4x4` formats + a cooker-only `astc-encoder` + a `textureCompressionASTC_LDR` **enable + gate** over the same machinery; **ASTC becomes the cook default**, BC7 selectable. Proves both codecs. Depends on 02. | proposed |
| 04 | Migration + golden | Migrate hello-triangle (mipped **ASTC** over a zstd pack) and regenerate the smoke golden on an ASTC-capable device, gating it to skip on a non-ASTC device. Depends on 00–03. | proposed |
| 05 | Docs + roadmap | Document the track across the `CLAUDE.md` set + root `CLAUDE.md`, capture the deferred developer-control work as `future/README.md` area 15, and run the full verification band. The closer. Depends on 00–04. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

**00** (archive zstd) is standalone and parallel to everything. **01 → 02 → 03** is a strict chain: 01
lays the multi-mip blob + multi-region upload, 02 adds the BC7 formats / encoder / capability gate /
block-aware sizing over it, 03 slots ASTC into 02's machinery and flips the default. **04** (migration +
golden) depends on 00–03, and **05** (docs + roadmap) is the closer, depending on 00–04. Worktree-isolated
dispatch branches 02 from the 01 integration commit and 03 from the 02 one — see
[[project_megaexec_worktree_base]].

**Shared-file caveat:** 01, 02, and 03 all touch `cooker/src/Importers/TextureImporter.cpp`,
`engine/src/Asset/Loaders/TextureLoader.cpp`, and (02/03) `Renderer/Types.h` + `TypeMapping.h` +
`FormatInfo.h`. Because they form a sequential chain this is merge-in-order by construction, not a
parallel-merge hazard.

## The decisions this planset settles

- **Textures cook compressed and mipped by default; the engine supports both ASTC and BC7.** Raw
  single-mip RGBA8 is gone — the cook default is mipped ASTC (the Metal-blessed codec on the primary
  platform), with BC7 selectable for the Windows target, and the archive is zstd-compressed. The runtime
  does not transcode: a device lacking the cooked codec reports `AssetError::Unsupported`.
- **The codec abstraction is one block-info helper.** Upload sizing and the loader's per-level walk go
  through `FormatInfo::BytesForLevel` for every format (uncompressed = a 1×1 block), so adding a codec
  is a format row + a `TypeMapping` row + a capability gate + an encoder arm — not a new upload path.
- **A compressed format must be enabled at device creation, not merely queried.** `textureCompressionBC`
  / `textureCompressionASTC_LDR` are core `VkPhysicalDeviceFeatures` booleans the device enables (when
  supported) at `createDevice`; the `Is…Supported()` query reflects the enabled state. Sampling a
  block-compressed image is illegal without the enable.
- **The archive format bump is global.** Format v3 affects every `.vengpack`, including the **embedded
  core pack** — the bump re-cooks it, with the ccache `#embed`-staleness footgun
  ([[project_ccache_embed_staleness]]) handled in verification.
- **Developer-facing codec control is deferred, by design.** This track hardcodes the ASTC-default /
  BC7-selectable seam; the whole authoring layer — per-platform **build configurations** holding the
  codec policy, role-based per-asset compression, the coarse cook-time config dependency, and the
  editor's host-capability preview gate — is future **area 15**
  ([`future/build-configurations.md`](../future/build-configurations.md)).

## Where this meets planset-32

planset-32 (render-allocation self-sizing) exists because the allocation footprint hurts on a
unified-memory MoltenVK device; this planset cuts texture VRAM ~8:1 on the same devices. They share no
files, but they compose at one seam: planset-32's named follow-on **memory-driven initial-tier capping**
(choosing the starting allocation tier from a device memory-budget query) reads a VRAM budget that this
planset's compression materially changes. The two are independent to *build* and *land*, but the
memory-budget follow-on should account for compressed texture residency once both are in.

## What remains (future)

The whole developer-control layer is **area 15** ([`future/build-configurations.md`](../future/build-configurations.md)):
per-platform build configurations holding the codec policy as a role → format table; per-asset
`*.tex.json` declaring a compression **role** (not a raw codec); the coarse cook-time config dependency
(one output pack per config); the editor's host-capability preview gate. Still-open footprint items —
**BC5/BC4 channel specialization** (normals/masks), **wider ASTC footprints** (6×6, 8×8), **HDR ASTC**,
and an **uncompressed fallback pack** — append to that area's open questions.
