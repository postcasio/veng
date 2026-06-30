# Plan 06 — Bruneton precomputed atmospheric sky (and the first real `Type3D` consumer)

**Goal:** a physically-based, **dynamic** sky — Bruneton's precomputed atmospheric scattering. The
atmosphere is precomputed once into a set of lookup tables; at runtime the sky for *any* sun direction
is a cheap LUT sample, so a day/night cycle is free. This is the heavy anchor of the planset, and the
first thing in veng to use the **`Type3D`** volume-texture capability (the 4D scattering table packs
into a 3D texture). Bounded: aerial perspective and an amortized specular sky prefilter are named
follow-ons, not built here.

## The starting point

- `ImageType::Type3D` / `ImageViewType::Type3D` and a `uvec3 Extent` exist but are **unused** — no pass
  allocates or samples a 3D texture today.
- The skybox today is tied to an `Environment` IBL cubemap (radiance/irradiance/prefilter +
  BRDF LUT); a scene without an `Environment` renders a flat fallback.
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
  fixed set of dispatches), and **irradiance** (2D). Run **once per atmosphere change**, not per frame
  — the sun direction is a runtime parameter into the eval, not a precompute input. The LUTs are
  renderer-owned resources (like the shadow atlas / bloom pyramid), regenerated on an `Atmosphere`
  change the way IBL maps regenerate on an `Environment` change.

### 3. The runtime sky pass

- A sky-render `ScenePass` samples the LUTs for per-view-direction sky radiance and the sun disk,
  rendered behind the scene geometry (depth at the far plane), into the HDR target ahead of the
  deferred lighting composite. Sun direction + `Atmosphere` ride per-frame `SceneView` / settings; the
  sky is a `SceneRendererSettings` toggle (default **off**, so the shipping path and `smoke_golden` are
  untouched until a sample opts in).
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
day/night demonstration, and the seam Plan 07's dynamic ambient hangs on. If the sample ships with the
atmosphere **on**, regenerate `smoke_golden` once; if it stays a UI-only opt-in with the cooked
environment as the default scene, the golden holds. `template` does not enable it.

## Verification

- The sky renders plausibly across sun elevations (noon blue → horizon warm → night dark) with no
  per-frame precompute (assert the LUTs regenerate only on an `Atmosphere` change, not each frame).
- **`Type3D` as a storage image and as a sampled 3D texture works on MoltenVK** — a small
  `gpu`-band test that writes and samples a 3D texture, since this is the first such use and the
  capability is otherwise unexercised.
- Validation gate clean — the 3D storage-image barriers + the multi-pass precompute are where a
  validation error would hide.
- `smoke_golden` per the opt-in decision above.

## Risks

- **MoltenVK 3D storage-image support** — verify `Type3D` UAV writes are supported on the installed
  MoltenVK before building the 4D-as-3D scattering table on it; the standalone `Type3D` test is the
  gate, and a fallback (a tiled 2D packing) is the contingency if 3D storage is absent.
- **Precompute correctness** — the multiple-scattering iteration is the subtle part; validate against
  reference imagery at known sun angles, not by eyeball alone.
- **Scope creep** — aerial perspective (scattering integrated over scene depth) and a sky-driven IBL
  specular prefilter are tempting but **out**; they are named follow-ons so this plan stays the sky +
  its LUTs, not a full atmospheric-rendering system.
