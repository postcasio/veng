# Plan 03 — BC5/BC4 channel specialization (normals and masks stop paying for full-channel codecs)

**Goal:** extend planset-35's **role → format** table so a `Normal` role resolves to a **two-channel**
codec (`BC5`) and a `Mask` role to a **single-channel** codec (`BC4`) on the block-compression
targets, instead of every role mapping to a full-channel BC7/ASTC. Add the ASTC normal-packing
convention ASTC needs (it has no two-channel mode). Self-contained: it rides the role→format table
(planset-35) and the codec plumbing (planset-33, `FormatInfo::BytesForLevel`, device-capability
gates); it adds new formats, not a new tier of developer control.

## The starting point

- A texture declares a compression **role** (Color / Normal / Mask / HDR / UI); a `BuildConfiguration`
  resolves role → concrete `CompressionFormat` per platform (planset-35). Under the two current codecs
  **every role maps full-channel** — `Color → sRGB`, the rest → unorm full BC7/ASTC.
- `CompressionFormat`, `Renderer::Format`, and the backend `TypeMapping` exhaustive switches define the
  codec set; `FormatInfo::BytesForLevel` gives block math; device creation gates the codecs
  (`IsBlockCompressionSupported` / `IsAstcSupported`).
- The cooker's BC/ASTC encoders (`bc7enc_rdo`, `astc-encoder`) run the block encode; the runtime never
  transcodes — a device lacking the cooked codec reports `AssetError::Unsupported`.

## What lands

### 1. The codecs

- `CompressionFormat` gains `BC5` (two-channel, RG) and `BC4` (single-channel, R); `Renderer::Format`
  gains the matching **`BC5Unorm`** (unsigned, `*2-1` unpack — committed here, not optional) and
  **`BC4Unorm`**; the `TypeMapping` switches map them to the Vulkan formats. `FormatInfo::GetFormatBlockInfo`
  currently folds `BC7Unorm/BC7Srgb/ASTC4x4Unorm/ASTC4x4Srgb` into **one** `.Bytes = 16` case — that
  case must be **split**: `BC5Unorm` joins the 16-byte arm, but `BC4Unorm` is **8 bytes** and needs its
  own arm.
- The cooker encodes a `Normal`-role texture as **BC5** (RG, the X/Y of the tangent-space normal) and
  a `Mask`-role texture as **BC4** (R), via the existing BC encoder. The `RoleToFormat` defaults for
  the block-compression configuration map `Normal → BC5`, `Mask → BC4`.

### 2. The ASTC normal convention

ASTC has no two-channel mode, so the ASTC configuration keeps `Normal → ASTC` but with a **packing
convention**: store X/Y (drop Z), reconstruct Z in the shader. The cooker writes the convention into
the cooked texture as a **channel-layout flag** — a new field in the cooked-texture header
(`assetpack` `CookedBlobs.h`), so this **bumps the cooked-texture format version**; the loader's
version gate reads the flag and every pack re-cooks. The material's normal-sampling helper reconstructs
`z = sqrt(1 - x² - y²)` for both BC5 and the ASTC-XY convention (unsigned `*2-1` unpack matching the
encode), behind one shared shader function so call sites are codec-agnostic.

### 3. Device gating

BC5/BC4 are core wherever BC is (the existing `IsBlockCompressionSupported` covers them); no new
capability query. The `Unsupported` fallback path is unchanged — a device without BC still reports it
per texture.

## Files (sketch — the agent confirms against the tree)

- `engine/include/Veng/Project/CompressionRole.h` / the `CompressionFormat` enum — add `BC5`/`BC4`.
- `engine/include/Veng/Renderer/Types.h`, `engine/src/Renderer/Backend/TypeMapping.h` — the new
  `Renderer::Format`s and their Vulkan mappings.
- `engine/.../FormatInfo.h` — split the combined `GetFormatBlockInfo` block-format case (BC4 = 8-byte
  arm, BC5 joins the 16-byte arm).
- `assetpack/.../CookedBlobs.h` + the texture loader — the channel-layout flag field and the
  cooked-texture format-version bump that adding it implies (loader reads the flag).
- `cooker/src/Importers/TextureImporter.*` — encode Normal→BC5, Mask→BC4; write the channel-layout
  flag for the ASTC-XY convention.
- The build-config `RoleToFormat` defaults (the shipped `*.buildcfg` for the BC target).
- The core material shader header — the shared normal-reconstruction helper (BC5 / ASTC-XY).

## Examples to co-migrate

`hello-triangle`'s brick normal map declares the `Normal` role (it likely already does post-planset-35);
its mask/ORM-source textures declare `Mask` where single-channel. On the **BC** configuration they now
cook BC5/BC4; on the host **ASTC** configuration (the Mac smoke path) they cook ASTC with the XY
convention. `template` carries no normal/mask textures, so it is unaffected beyond the shared helper.

## Verification

- Cook the brick normal as BC5 (BC config) and as ASTC-XY (host config); sample both and confirm the
  reconstructed normal matches the full-channel result within tolerance.
- **BC5 has no golden coverage on the dev platform** — `smoke_golden` is host-ASTC only, so the BC5
  branch the shipped Windows/Linux configs use never renders on macOS CI. Add a non-golden gpu test
  that decodes a known normal through the BC5 path explicitly, independent of the ASTC smoke.
- `smoke_golden` is on the **host ASTC** path — the change there is the XY convention + Z
  reconstruction, which is lossy at the codec level; regenerate the golden **once** if the normal
  reconstruction moves the lit result beyond the fuzzy threshold, otherwise it holds.
- A `FormatInfo::BytesForLevel` unit test over BC5/BC4 block counts (pure, no ICD) — covering the split
  `GetFormatBlockInfo` arms (BC4 = 8 bytes, BC5 = 16 bytes).
- The `Unsupported` path still triggers on a device without BC (cooker `cooker` suite).

## Risks

- **Signed vs unsigned BC5 for normals** — settled as `BC5Unorm` with `*2-1` unpack (§1); the shader
  unpack must match the encode exactly, or the normal flips/halves.
- **The ASTC-XY convention is lossy differently than BC5** — the two codecs reconstruct Z from
  different stored precision, so the host golden and a BC-target render are not byte-identical to each
  other (expected; each is validated against its own reference, not against the other).
- **Channel-layout flag plumbing** — the runtime sampler must read the convention from the cooked
  texture, not assume it; a missed flag samples a normal as raw RGB.
