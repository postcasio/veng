# Plan 05 — docs + roadmap

**Goal:** document the texture-compression track where it is read, capture the deferred developer-control
work on the roadmap, and run the full verification band. The **closer** for the planset. Depends on
Plans 00–04.

## What lands

- **`cooker/CLAUDE.md`.** Textures cook to **mipped ASTC by default** (BC7 selectable for the Windows
  target); the offline mip chain (sRGB-/linear-correct); the per-texture/per-pack codec + footprint
  authoring is deferred (area 15); the new cooker-only encoder deps (`bc7enc_rdo`, `astc-encoder`) stay
  cooker-only.

- **`assetpack/CLAUDE.md`.** **Format v3**, per-blob zstd, the codec field + `UncompressedSize`, hashing
  over stored bytes, the lazy-inflate cache + its main-thread-only invariant. The global format bump
  re-cooks the embedded core pack ([[project_ccache_embed_staleness]]).

- **`engine/CLAUDE.md`.** The multi-mip + block-compressed texture load path, the `FormatInfo` block
  helper (`BytesForLevel`, uncompressed = a 1×1 block), the BC/ASTC capability **enable + gate** (a
  compressed format must be enabled at `createDevice`, not merely queried), the multi-region
  `CopyBufferToImage`, and the `AssetError::Unsupported` behavior on a device lacking the cooked codec
  (the loader logs once, materials sample their fallback, the app still runs — only `smoke_golden`, which
  skips there, would diverge).

- **root `CLAUDE.md`.** **zstd added to the runtime dependency list** (the first third-party codec linked
  into `libveng`, transitively via `assetpack`); note the cooker-only encoder deps stay cooker-only.

- **`future/README.md` (area 15 — build configurations & project settings).** Confirm the deferred
  developer-control work is captured ([`future/build-configurations.md`](../future/build-configurations.md)):
  per-platform **build configurations** holding the texture **codec policy** as a role → format table;
  per-asset `*.tex.json` declaring a compression **role** (not a raw codec); the implicit/coarse cook-time
  dependency (one output pack per config); the editor's **host-capability preview gate** (build any
  config, preview only what the host GPU can sample). Append the still-open footprint items (**BC5/BC4
  channel specialization**, **wider ASTC footprints**, **HDR ASTC**, an **uncompressed fallback pack**)
  to that area's open questions.

- **`plans/README.md`.** The planset-33 summary entry (this record).

## Files

| File | Change |
|---|---|
| `cooker/CLAUDE.md` | Mipped-ASTC-default cook, the mip chain, deferred codec authoring, cooker-only encoder deps. |
| `assetpack/CLAUDE.md` | Format v3, per-blob zstd, codec field + `UncompressedSize`, lazy-inflate cache. |
| `engine/CLAUDE.md` | Multi-mip + block-compressed load path, `FormatInfo`, the enable+gate, missing-codec behavior. |
| `CLAUDE.md` | zstd in the runtime dependency list; cooker-only encoder deps stay cooker-only. |
| `plans/future/README.md` | Area 15: deferred developer-control area + appended open footprint items. |
| `plans/README.md` | The planset-33 summary entry. |
| `plans/planset-33/README.md` | Status column → `done` as plans land. |

## Verification

- Full `ctest --output-on-failure` green (incl. the Plan 00–03 cases); `smoke_golden` green on an
  **ASTC-capable** device and **skipping** on a non-ASTC device.
- `validation_gate` green under `build-debug` (`ctest -L validation`).
- `include_hygiene` green.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
