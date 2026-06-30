# Plan 05 — `Veng/Math/SphericalHarmonics.h` (the SH math, foundation-first)

**Goal:** add a pure, device-free spherical-harmonics math header beside `AABB` / `Frustum` / `BVH` /
`Ray` in `Veng/Math/` — project a radiance sample into low-order SH, convolve
radiance SH into **irradiance** SH (the Lambertian cosine lobe), and evaluate SH in a direction.
Foundation-first and fully unit-tested with **no consumer in this plan** — Plan 07 (dynamic SH
ambient) is its consumer; later GI (per-probe irradiance) is its long-term one. This mirrors how
`AABB`/`Frustum` landed before the passes that used them.

## Why land the math alone

The diffuse half of image-based lighting is irradiance, which is band-limited: Ramamoorthi &
Hanrahan's result is that **order-2 SH (9 coefficients per channel) reconstructs irradiance from any
environment with average error under ~1%**. SH is therefore the standard, compact representation of
diffuse irradiance — exact enough for diffuse, and the only practical per-probe storage for future GI
(a cubemap per probe does not scale). veng's IBL uses prefiltered *cubemaps*, so this representation is
genuinely absent. The math is pure and testable in isolation; landing it as a verified primitive keeps
Plan 07 (and any later GI) free of re-deriving the easy-to-get-wrong convolution factors.

## What lands

### 1. The types and operations

- An order-2 **`Sh9`** coefficient set — 9 coefficients per color channel (a `Sh9` of `vec3`, or three
  `array<f32, 9>`; the agent picks the layout that serializes cleanly into a constants buffer for
  Plan 07).
- **Projection** — `void ProjectSample(Sh9&, vec3 direction, vec3 radiance, f32 weight)` accumulating
  a `(direction, radiance)` sample into the basis, and a normalization helper for a set of samples
  (Monte-Carlo or analytic).
- **Cosine-lobe convolution** — `Sh9 ConvolveCosine(const Sh9&)` applying the per-band factors
  (`A_0 = π`, `A_1 = 2π/3`, `A_2 = π/4`) that turn *radiance* SH into *irradiance* SH, so a single
  evaluation gives the cosine-weighted hemisphere integral (the Lambertian response).
- **Evaluation** — `vec3 EvalIrradiance(const Sh9&, vec3 normal)` reconstructing irradiance in a
  direction.

### 2. The header home and form

- `engine/include/Veng/Math/SphericalHarmonics.h` — glm-only, header-only where the others are
  (matching `AABB`/`Frustum`), no GPU, no ImGui, inside `include_hygiene`.

## Files (sketch — the agent confirms against the tree)

- `engine/include/Veng/Math/SphericalHarmonics.h` (new) — the types + the three operations.
- `tests/unit/` — the SH test suite.

## Verification (all pure, no ICD)

- **Constant ambient** — projecting a uniform radiance over the sphere and evaluating reproduces that
  constant irradiance (the L0 term carries it; higher bands are ~0).
- **Cosine factors** — `ConvolveCosine` produces the analytic `A_l` band factors; pin them against the
  published constants.
- **Directional sample** — a single bright direction projects and reconstructs a smooth lobe peaking at
  that direction.
- **Linearity / superposition** — projecting a sum of radiances equals the sum of projections.
- **Golden constants for the GPU to match** — a fixed `Sh9` coefficient set with hand-computed
  `EvalIrradiance` values at a few normals, checked in as literals. This is the numeric target Plan 07's
  shader `EvalIrradiance` must reproduce, so the CPU↔GPU basis contract is pinned **here** rather than
  only described in prose for Plan 07 to re-derive.

## Risks

- **Basis/normalization convention** — there are several SH normalization conventions; fix one and
  document it, because Plan 07's *shader* evaluation must use the identical basis or the ambient is
  subtly wrong. This plan pins the CPU side **numerically** (the golden-constants test above), so the
  convention is enforced by checked-in values, not prose, two plans before its consumer lands; Plan 07's
  CPU↔GPU consistency check then matches the shader to those same goldens.
- **Over-scoping** — keep it order-2 (9 coefficients). Higher orders, rotation, and per-probe sets are
  later GI concerns, not this header's.
