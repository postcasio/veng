# Plan 07 — ASTC block compression

**Goal:** add the **ASTC** format family + type-mapping, wire the **astc-encoder** into the cooker, and
let a texture cook to ASTC through the **same** multi-mip block path BC7 uses — proving the engine
supports **both** codecs. BC7 stays the default; ASTC is selectable through a minimal internal seam (the
polished developer-facing codec knob is **deferred**). **Depends on Plan 06.**

## Why it is its own plan

ASTC is a **second encoder** and a **second format family** layered over the infrastructure Plan 06
built — the block-info helper, the capability gate, the block-aware upload, and the loader bridge.
Isolating it keeps the `astc-encoder` dependency and the ASTC-specific block handling reviewable apart
from the BC core, and it is the plan that **verifies the "both codecs" requirement end to end**. If
Plan 06's abstraction is right, this plan is a handful of rows plus an encoder arm.

## What lands

- **ASTC formats.** Append `ASTC4x4Unorm` and `ASTC4x4Srgb` to `Renderer::Format` (the high-quality
  4×4 footprint, matching BC7's ~8:1), with `TypeMapping::ToVk` → `eAstc4x4UnormBlock` /
  `eAstc4x4SrgbBlock`. `FormatInfo` gains their rows: 4×4 block extent, 16 bytes/block (same numbers as
  BC7, different vk format). Wider footprints (6×6, 8×8 — more compression, lower quality) are a later
  refinement, not this plan.

- **Capability gate.** `Context::IsAstcSupported()` queries `textureCompressionASTC_LDR`. MoltenVK
  exposes ASTC broadly on Apple GPUs — it is the **natively-Metal-blessed** family — so on the primary
  platform ASTC has **wider** device support than BC (which is Apple-Silicon-only). The loader's
  `Unsupported` path generalizes to "the cooked codec's feature is absent on this device."

- **ASTC encoder in the cooker.** A pinned `FetchContent` `astc-encoder` (ARM, CMake), **cooker-only
  behind `VENG_BUILD_TOOLS`**, never linked into `libveng`. It encodes each generated RGBA8 mip to
  ASTC **4×4 LDR** at a quality preset, sRGB-aware (`ASTC4x4Srgb` / `ASTC4x4Unorm`).

- **Codec arm in the importer.** `TextureImporter` gains a codec switch — **BC7 remains the default**
  from Plan 06, ASTC the second arm — encoding the *same* mip chain. The selection is a **minimal
  internal seam** (a cook-side codec value), **not** the per-texture / per-pack authoring UX, which
  stays deferred.

- **Loader bridges ASTC.** `TextureLoader::BridgeFormat` gains the ASTC cases; everything downstream —
  `MipLevels`, the block-aware multi-region upload, the `Unsupported` gate — is reused from Plan 06
  unchanged.

## Decisions

1. **ASTC 4×4 LDR, sRGB-aware.** The high-quality footprint matching BC7's ratio. Other footprints
   (6×6/8×8 for more compression) and **HDR ASTC** are out of scope — HDR environments have their own
   path; this is the LDR texture codec.

2. **ASTC reuses every Plan-06 seam unchanged.** Only a format row, a `TypeMapping` row, a
   `FormatInfo` row, a capability query, an encoder arm, and `BridgeFormat` cases — proof the codec
   abstraction holds. No upload or blob-layout change.

3. **Codec is internally selectable, BC7 default.** Both encoders are real and testable now; the
   developer-facing codec/footprint authoring is the deferred follow-on.

4. **The capability asymmetry is acknowledged, not resolved here.** BC = Apple-Silicon-only, ASTC =
   broad on Apple GPUs; **choosing the right codec per target device** is the deferred
   developer-control / per-platform-cook plan. This plan only makes both decodable.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Types.h` | Append `ASTC4x4Unorm`, `ASTC4x4Srgb`. |
| `engine/include/Veng/Renderer/Backend/TypeMapping.h` | `ToVk` cases for the two ASTC formats. |
| `engine/include/Veng/Renderer/FormatInfo.h` | ASTC 4×4 block rows. |
| `engine/include/Veng/Renderer/Context.h`, `engine/src/Renderer/Backend/Context.cpp` | `IsAstcSupported()` (`textureCompressionASTC_LDR` query). |
| `engine/src/Asset/Loaders/TextureLoader.cpp` | `BridgeFormat` ASTC cases; generalize the `Unsupported` gate to the cooked codec's feature. |
| `cooker/CMakeLists.txt` | `FetchContent` `astc-encoder`, cooker-only. |
| `cooker/src/Importers/TextureImporter.cpp` | ASTC encode arm; the codec switch (default BC7); ASTC format integers hand-synced to `Renderer::Format`. |
| `tests/…` | Cooker: a texture cooks to an ASTC format with the expected per-level block sizes. GPU (ASTC-capable, else skip): an ASTC cooked fixture loads and samples. |

## Verification

- Clean build; `ctest` green — the ASTC GPU test **skips** without ASTC support.
- `validation_gate` green — ASTC image creation + upload raise no validation error.
- The default cook stays **BC7**, so `smoke_golden` is **unchanged from Plan 06** (no re-regenerate); a
  dedicated ASTC fixture texture exercises the path instead of the smoke scene.
- `include_hygiene` unaffected.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
