# Plan 06a — the `Type3D` volume-texture foundation (capability, lifecycle, MoltenVK probe)

**Goal:** land veng's first real use of the **`Type3D`** volume-texture capability as a foundation-first
commit — before the atmosphere precompute ([Plan 06b](06b-bruneton-atmospheric-sky.md)) depends on it.
`ImageType::Type3D` / `ImageViewType::Type3D` and a `uvec3 Extent` exist in the renderer vocabulary but
are **unused**: no pass creates, writes, samples, or retires a 3D texture today. This plan exercises the
full lifecycle once, behind a standalone `gpu`-band test, and confirms the MoltenVK path up front — so
06b carries the Bruneton math with no latent platform fork inside it. This mirrors how Plan 05 lands the
SH math before its consumer.

## Why split this out

Plan 06b's LUT scheme is "the 4D scattering table packs into a 3D texture," and that 3D texture is
written as a **storage image** by the precompute and **sampled** at runtime. Two things are new and
unproven on the dev platform at once: the 3D storage-image *write* path and the 3D *sampled* path. If
either is constrained on the installed MoltenVK, the discovery belongs **before** the multiple-scattering
math is written against a specific storage layout — not mid-plan. Landing the capability and its test
first makes the 05→06a→06b chain bisectable: a volume-texture regression is isolated from a precompute
regression from a sky-pass regression.

3D storage is very likely **available** — Metal supports 3D textures and compute writes to them on both
Apple-Silicon and Mac2 GPU families, which is what MoltenVK maps `VK_IMAGE_TYPE_3D` +
`VK_IMAGE_USAGE_STORAGE_BIT` onto, and the in-tree compute bloom / hi-Z passes already prove 2D storage
images work here. So this plan expects the probe to **pass**; it is cheap insurance, not a likely fork.

## The starting point

- `ImageType::Type3D` (`Renderer/Types.h`), `ImageViewType::Type3D`, and `uvec3 Extent` exist and are
  mapped in `Backend/TypeMapping.h`, but nothing constructs a 3D `Image` / `ImageView`.
- 2D storage-image compute (the bloom pyramid, hi-Z max-Z reduction) is a proven pattern: storage views,
  per-level dispatches, explicit barriers, mid-frame retire.

## What lands

### 1. The 3D image lifecycle through `Image::Create`

- Confirm (and fix where absent) that `Image::Create` / `ImageView` creation, `BytesForLevel`/mip math,
  the barrier path, and the deferred-retire path all handle a `Type3D` image with a `uvec3 Extent`
  (the `extent.z` axis is the new variable — verify mip and byte math fold it in). No new public API
  surface beyond what `Type3D` already declares; this is making the existing enum value real.

### 2. A standalone `Type3D` gpu test

- A `gpu`-band test that drives the **full lifecycle**: create a 3D storage image, **write** it from a
  compute dispatch (`imageStore` at a 3D coordinate), barrier, **sample** it as a 3D texture in a second
  pass, read back, and **retire/destroy it mid-frame** — the retire path is where a new resource
  dimension most often leaks. It asserts the sampled values match what was written. Labelled `gpu`
  (`SKIP_RETURN_CODE 77`), it runs under the validation gate like the rest of the band.

### 3. The MoltenVK capability probe

- The test **is** the go/no-go probe for 06b: if write-only 3D storage or 3D sampling is unsupported on
  the installed MoltenVK, it fails here, before 06b's precompute is written. The design guidance it
  validates and hands to 06b: use **write-only** 3D storage with **ping-pong** source/destination
  textures (never in-place read-write 3D, the genuinely constrained case); pick an HDR storage-capable
  format (RGBA16F).

## Files (sketch — the agent confirms against the tree)

- `engine/src/Renderer/Backend/Image.cpp` / `ImageView` creation — `Type3D` create/view/mip/retire
  coverage (fixes only where the 3D axis is unhandled).
- `tests/gpu/` — the standalone `Type3D` write/sample/retire test.
- Docs: `engine/CLAUDE.md` if it documents the image-resource surface (note `Type3D` is now a real,
  tested resource dimension).

## Examples to co-migrate

None — this is engine + test only; no sample uses a 3D texture until 06b's atmosphere opt-in.

## Verification

- The `Type3D` gpu test passes on the installed MoltenVK: write → barrier → sample → readback matches,
  and mid-frame retire is clean.
- **Validation gate clean** (`build-debug -L validation`) — 3D storage-image barriers are exactly where
  a layout/hazard validation error would hide.
- `smoke_golden` holds (no render change).

## Risks

- **MoltenVK 3D storage-image support** — the named gate. Expected to pass (see *Why split this out*).
  If it somehow does not, the contingency is **layered render-to-2D-slices** of the same 3D texture (a
  render-target view per Z slice, the classic Bruneton-on-GL path) — which **keeps** the `Type3D`
  resource and sampling path. A 2D *repack* (throwing away the volume texture) is **not** the fallback;
  it would rewrite 06b's sampler addressing for a problem this probe almost certainly rules out. If the
  fallback is ever needed it is its own follow-on, not absorbed into 06b's estimate.
- **The `extent.z` axis in mip/byte math** — the most likely real bug, since every prior consumer was
  2D; the lifecycle test's readback is the guard.
