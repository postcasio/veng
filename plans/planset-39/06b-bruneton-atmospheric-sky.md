# Plan 06b — Bruneton precomputed atmospheric sky

**Goal:** a physically-based, **dynamic** sky — Bruneton's precomputed atmospheric scattering. The
atmosphere is precomputed once into a set of lookup tables; at runtime the sky for *any* sun direction
is a cheap LUT sample, so a day/night cycle is free. This is the heavy anchor of the planset. It is the
first real consumer of the **`Type3D`** volume-texture capability — the 4D scattering table packs into a
3D texture — which [Plan 06a](06a-type3d-foundation.md) lands and proves first, so this plan builds on a
known-good volume-texture path rather than discovering it mid-stream. Bounded: aerial perspective and an
amortized specular sky prefilter are named follow-ons, not built here. Depends on **06a**.

## The starting point

- [Plan 06a](06a-type3d-foundation.md) made `Type3D` a real, tested resource dimension — create, compute
  storage-write, sample, and mid-frame retire all pass on the installed MoltenVK, with the design
  guidance: **write-only** 3D storage, **ping-pong** source/destination (no in-place read-write), HDR
  storage format (RGBA16F).
- The skybox today is tied to an `Environment` IBL cubemap (radiance/irradiance/prefilter +
  BRDF LUT); a scene without an `Environment` renders a flat fallback. The existing `SkyboxScenePass`
  composites the radiance cube **after** deferred lighting (`LoadOp::Load` into the lit scene-color
  target), before the bloom/SSR/tonemap tail.
- Compute-driven mip/LUT generation is a proven pattern (hi-Z max-Z reduction, the bloom pyramid):
  per-level dispatches, storage views, explicit barriers.

## What lands

### 1. The atmosphere model

- An **`Atmosphere`** parameter struct — Rayleigh / Mie / ozone coefficients, planet + atmosphere
  radii, sun angular radius and irradiance — a reflected settings struct so the editor inspects it for
  free.

### 2. The precompute

- A compute precompute producing the LUTs: **transmittance** (2D), **single + multiple scattering**
  (the 4D table packed as a **`Type3D`** texture, the multiple-scattering iteration run as a small
  fixed set of dispatches), and **irradiance** (2D). The multiple-scattering iteration reads order *n*
  and writes order *n+1* via **ping-pong** write-only 3D storage (per 06a's design — never in-place
  read-write). Run **once per atmosphere change**, not per frame — the sun direction is a runtime
  parameter into the eval, not a precompute input. The LUTs are renderer-owned resources (like the
  shadow atlas / bloom pyramid), regenerated on an `Atmosphere` change the way IBL maps regenerate on an
  `Environment` change.

### 3. The runtime sky pass

- A sky-render `ScenePass` samples the LUTs for per-view-direction sky radiance and the sun disk,
  rendered behind the scene geometry (cleared-depth background). It occupies the **same slot as the
  existing `SkyboxScenePass`** — composited into the lit scene-color target *after* deferred lighting,
  before the bloom/SSR/tonemap tail — so the atmosphere sky and the cubemap skybox are interchangeable
  sky sources in one place, not two insertion points. Sun direction + `Atmosphere` ride per-frame
  `SceneView` / settings; the sky is a `SceneRendererSettings` toggle (default **off**, so the shipping
  path and `smoke_golden` are untouched until a sample opts in).
- Integration with the existing skybox/`Environment` path: the atmosphere is an alternative sky source;
  when enabled it supplies the sky, and Plan 07 consumes it for the ambient term. The `Environment`
  IBL path is left intact (an atmosphere-driven IBL prefilter is a named follow-on).

## Files (sketch — the agent confirms against the tree)

- `engine/include/Veng/Renderer/Atmosphere.h` (new) — the `Atmosphere` parameter struct + the
  device-free math/parameterization that is testable in isolation.
- `engine/src/Renderer/Passes/AtmospherePrecompute.*`, `SkyScenePass.*` (new) — the precompute
  dispatches and the runtime sky pass.
- `engine/assets/core/shaders/atmosphere/` — the transmittance / scattering / irradiance precompute
  shaders and the sky-eval shader.
- `engine/src/Renderer/SceneRenderer.cpp` — own the LUTs, regenerate on `Atmosphere` change, wire the
  sky pass + the toggle/recompile.

## Examples to co-migrate

`hello-triangle` gains an **atmosphere toggle** in its render-settings UI and can switch its sky from
the cooked environment to the procedural atmosphere with a sun-direction control — the visible
day/night demonstration, and the seam Plan 07's dynamic ambient hangs on. The atmosphere stays a
**UI-only opt-in** with the cooked environment as the default smoke-pose scene, so this plan does not
move `smoke_golden` by itself. (Plan 04 already moves the golden; the regeneration happens once against
the integrated tree — see the planset README's golden note — not per-plan.) `template` does not enable
it.

## Verification

- The sky renders plausibly across sun elevations (noon blue → horizon warm → night dark) with no
  per-frame precompute (assert the LUTs regenerate only on an `Atmosphere` change, not each frame).
- The `Type3D` storage-write + sampled-read path is already proven by Plan 06a's standalone test; this
  plan does not re-prove the capability, it consumes it.
- Validation gate clean — the 3D storage-image barriers + the multi-pass precompute are where a
  validation error would hide.
- `smoke_golden` holds (atmosphere is UI-only opt-in; the integrated-tree regen is owned elsewhere).

## Risks

- **MoltenVK 3D storage-image support** — resolved **before** this plan by Plan 06a's probe, so the
  scattering table is built on a known-good path. (If 06a's probe had failed, the contingency is layered
  render-to-2D-slices of the same 3D texture, keeping `Type3D` — not a 2D repack; 06a owns that call.)
- **Precompute correctness** — the multiple-scattering iteration is the subtle part; validate against
  reference imagery at known sun angles, not by eyeball alone.
- **Scope creep** — aerial perspective (scattering integrated over scene depth) and a sky-driven IBL
  specular prefilter are tempting but **out**; they are named follow-ons so this plan stays the sky +
  its LUTs, not a full atmospheric-rendering system.
