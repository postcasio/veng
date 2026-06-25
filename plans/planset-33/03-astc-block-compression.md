# Plan 03 — ASTC block compression (the default)

**Goal:** add the **ASTC** format family + type-mapping, wire the **astc-encoder** into the cooker, and
let a texture cook to ASTC through the **same** multi-mip block path BC7 uses — proving the engine
supports **both** codecs. **ASTC becomes the cook default** (the Metal-blessed codec on the primary
MoltenVK platform, with broad Apple-GPU support); **BC7 stays selectable** through the minimal internal
seam (the polished developer-facing codec knob is **deferred** to area 15). **Depends on Plan 02.**

## Why it is its own plan

ASTC is a **second encoder** and a **second format family** layered over the infrastructure Plan 02
built — the block-info helper, the capability enable + gate, the block-aware upload, and the loader
bridge. Isolating it keeps the `astc-encoder` dependency and the ASTC-specific block handling reviewable
apart from the BC core, and it is the plan that **verifies the "both codecs" requirement end to end** and
**sets the default**. If Plan 02's abstraction is right, this plan is a handful of rows plus an encoder
arm plus the default flip.

## What lands

- **ASTC formats, at pinned ordinals.** Append `ASTC4x4Unorm` and `ASTC4x4Srgb` to `Renderer::Format`
  (the high-quality 4×4 footprint, matching BC7's ~8:1). Plan 02 took `21`/`22`, so the new ordinals are
  **`ASTC4x4Unorm = 23`, `ASTC4x4Srgb = 24`**, with `TypeMapping::ToVk` → `eAstc4x4UnormBlock` /
  `eAstc4x4SrgbBlock`. `FormatInfo` gains their rows: 4×4 block extent, 16 bytes/block (same numbers as
  BC7, different vk format). The cooker writes `header.Format = 23/24` and `BridgeFormat` reads it, hand-
  synced to these ordinals. Wider footprints (6×6, 8×8 — more compression, lower quality) are a later
  refinement, not this plan.

- **Capability enable + gate.** `textureCompressionASTC_LDR` is enabled at `createDevice` when supported
  (the same conditional pattern Plan 02 used for BC), and `Context::IsAstcSupported()` reflects the
  enabled state. MoltenVK exposes ASTC broadly on Apple GPUs — it is the **natively-Metal-blessed**
  family — so on the primary platform ASTC has **wider** device support than BC (which is
  Apple-Silicon-only). The loader's `Unsupported` path generalizes to "the cooked codec's feature is
  absent on this device."

- **ASTC encoder in the cooker.** A pinned `FetchContent` `astc-encoder` (ARM, CMake), **cooker-only
  behind `VENG_BUILD_TOOLS`**, never linked into `libveng`. It encodes each generated RGBA8 mip to
  ASTC **4×4 LDR** at a documented quality preset, sRGB-aware (`ASTC4x4Srgb` / `ASTC4x4Unorm`).

- **ASTC becomes the default codec; BC7 selectable.** `TextureImporter`'s codec-selection seam (Plan 02)
  now defaults to **ASTC** — the right default for the Mac-primary engine — and **BC7 remains a selectable
  arm** for the anticipated Windows target. The selection is the **minimal internal seam** (a cook-side
  codec value), **not** the per-texture / per-pack authoring UX, which stays deferred. Both arms encode
  the *same* generated mip chain.

- **Loader bridges ASTC.** `TextureLoader::BridgeFormat` gains `case 23 → ASTC4x4Unorm`,
  `case 24 → ASTC4x4Srgb`; everything downstream — `MipLevels`, the block-aware multi-region upload, the
  `Unsupported` gate — is reused from Plan 02 unchanged.

## Decisions

1. **ASTC 4×4 LDR is the default footprint, sRGB-aware.** The high-quality footprint matching BC7's
   ratio. Other footprints (6×6/8×8 for more compression) and **HDR ASTC** are out of scope — HDR
   environments have their own path; this is the LDR texture codec.

2. **ASTC reuses every Plan-02 seam unchanged.** Only a format row, a `TypeMapping` row, a `FormatInfo`
   row, a capability enable + query, an encoder arm, and `BridgeFormat` cases — proof the codec
   abstraction holds. No upload or blob-layout change.

3. **ASTC default, BC7 selectable.** Both encoders are real and testable now; ASTC is the cook default
   because it is the Metal-blessed, broadly-supported codec on the primary platform, and BC7 stays
   selectable for the desktop/Windows target. The developer-facing codec/footprint authoring is the
   deferred follow-on (area 15).

4. **The capability asymmetry is acknowledged, not resolved here.** BC = Apple-Silicon-only, ASTC =
   broad on Apple GPUs; **choosing the right codec per target device** is the deferred
   developer-control / per-platform-cook plan. This plan makes both decodable and picks ASTC as the
   default.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Types.h` | Append `ASTC4x4Unorm = 23`, `ASTC4x4Srgb = 24`. |
| `engine/include/Veng/Renderer/Backend/TypeMapping.h` | `ToVk` cases for the two ASTC formats. |
| `engine/include/Veng/Renderer/FormatInfo.h` | ASTC 4×4 block rows. |
| `engine/include/Veng/Renderer/Context.h`, `engine/src/Renderer/Backend/Context.cpp` | Enable `textureCompressionASTC_LDR` at device creation when supported; `IsAstcSupported()` reflecting the enabled state. |
| `engine/src/Asset/Loaders/TextureLoader.cpp` | `BridgeFormat` `case 23/24`; generalize the `Unsupported` gate to the cooked codec's feature. |
| `cooker/CMakeLists.txt` | `FetchContent` `astc-encoder` at a pinned tag, cooker-only. |
| `cooker/src/Importers/TextureImporter.cpp` | ASTC encode arm; the codec seam **defaults to ASTC**, BC7 selectable; write `header.Format = 23/24` (hand-synced to the `Renderer::Format` ordinals). |
| `tests/…` | Cooker: an ASTC fixture cooks to `Format` `23`/`24` with the expected per-level block sizes; a round-trip test that the emitted integer bridges back to the matching `Renderer::Format`. GPU (ASTC-capable, else skip): an ASTC cooked fixture loads and samples. |

## Verification

- Clean build; `ctest` green — the ASTC GPU test **skips** without ASTC support.
- `validation_gate` green — ASTC image creation (with the feature enabled) + the multi-region block
  upload raise no validation error (including a non-multiple-of-4 mip).
- `smoke_golden` does **not** move in *this* plan — hello-triangle is still RGBA8 until Plan 04; ASTC and
  BC7 are exercised by fixtures.
- `include_hygiene` unaffected.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
