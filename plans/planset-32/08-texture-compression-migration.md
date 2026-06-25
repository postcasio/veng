# Plan 08 — texture-compression migration, golden, docs

**Goal:** migrate `examples/hello-triangle` onto the new pipeline (cooked **mips + BC7** textures over a
**zstd-compressed** archive), **regenerate the smoke golden** on a BC-capable device, and document the
texture-compression track across the `CLAUDE.md` set, this planset README, and `future/README.md`. The
**closer for Track B**. Depends on Plans 04–07.

## What lands

- **hello-triangle migrated.** Its textures cook with mips + BC7 by default into a zstd-compressed
  `.vengpack`; confirm the sample pack shrinks materially on disk and the **relocatable trio**
  (launcher + `libhello_triangle` + pack) still runs from a fresh directory and writes a correct-sized
  PPM.

- **Smoke golden regenerated.** BC7 is lossy, so the capture moves. Re-shoot via the documented path
  (`HT_SMOKE=/tmp/ht.ppm … hello_triangle-launcher`, `sips -s format png`) on a **BC-capable** device,
  and confirm `smoke_golden` passes within the existing fuzz tolerance — widening it only if BC7
  artifacts demand it, with the change documented in the test's allowlist/threshold.

- **Docs.**
  - `cooker/CLAUDE.md` — textures cook to **mipped BC7 by default**; the per-texture/per-pack codec +
    footprint authoring is deferred; ASTC is available; the new cooker-only encoder deps.
  - `assetpack/CLAUDE.md` — **format v3**, per-blob zstd, the `flags` codec + `UncompressedSize`,
    hashing over stored bytes.
  - `engine/CLAUDE.md` — the multi-mip + block-compressed texture load path, the `FormatInfo`
    block helper, the BC/ASTC capability gates and the `AssetError::Unsupported` behavior on a device
    lacking the cooked codec.
  - root `CLAUDE.md` — only if a project-wide convention shifted (e.g. zstd as a new runtime dep noted
    in the dependency list).

- **Roadmap.** Confirm the **deferred developer-control** work is captured as **future area 15 —
  build configurations & project settings** ([`future/build-configurations.md`](../future/build-configurations.md)):
  a project-settings concept owning per-platform **build configurations**, each holding the texture
  **codec policy** as a role → format table; per-asset `*.tex.json` declaring a compression **role**
  (not a raw codec); the implicit/coarse cook-time dependency (one output pack per config) and the
  editor's **host-capability preview gate** (build any config, preview only what the host GPU can
  sample). Append the still-open footprint items (**BC5/BC4 channel specialization**, **wider ASTC
  footprints**, **HDR ASTC**, an **uncompressed fallback pack**) to that area's open questions. Mark
  Plans 04–08 `done` in this planset's status column.

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/assets/…` | Texture sources / manifest cook to mipped BC7; confirm the pack builds and shrinks. |
| `tests/golden/hello_triangle_scene.png` | Regenerated capture (BC7-lossy), on a BC-capable device. |
| `cooker/CLAUDE.md`, `assetpack/CLAUDE.md`, `engine/CLAUDE.md`, `CLAUDE.md` | The doc updates above. |
| `plans/future/README.md` | The deferred developer-control area. |
| `plans/planset-32/README.md` | Status column → `done` for 04–08. |

## Verification

- Full `ctest` + `smoke_golden` + `validation_gate` green on a **BC-capable** device.
- `vengc verify` green on the re-cooked sample pack; the `.vengpack` is materially smaller than the
  pre-compression baseline.
- The relocatable trio runs from an unrelated working directory and exits 0.
- `include_hygiene` green.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
