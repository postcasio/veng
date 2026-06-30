# Plan 07 — dynamic SH ambient (the sky lights the scene's indirect term, and it moves)

**Goal:** wire Plan 05's SH math and Plan 06's atmospheric sky together: each frame, project the sky
into order-2 SH and use it as the scene's **diffuse ambient** in the deferred lighting pass. The
no-environment ambient stops being a dead-gray constant and becomes a **directional, dynamic** sky
fill that tracks the sun across a day/night cycle — for the cost of uploading 9 `vec3`s a frame.
Depends on Plan 05 (the SH math) and Plan 06 (the sky to project).

## Why SH, and what it is not

Re-prefiltering an irradiance cubemap every frame as the sun moves is the expensive operation;
projecting the sky into 9 SH coefficients is orders of magnitude cheaper and, for diffuse, essentially
lossless (the Ramamoorthi bound). This is the cheap diffuse half of a *dynamic* sky's lighting — it is
**not** bounce GI (no inter-object color bleed, no local indirect; SSAO still provides contact
occlusion) and it does **not** touch the specular path. It is "global ambient that has direction and a
day/night dial," and the foundation the same SH math serves per-probe in future GI.

## The starting point

- The deferred lighting shader's ambient branch is binary: `IblEnabled ? split-sum IBL : flat
  AmbientColor` (a hardcoded `float3(0.12,0.13,0.16)` constant), each modulated by `ambientOcclusion`
  (SSAO). `IblEnabled` is 1 whenever an `Environment` is resident.
- Plan 05 provides CPU SH projection/convolution/eval; Plan 06 provides the runtime sky to sample.
- Per-view constants ride a ring-buffered buffer the lighting pass already reads.

## What lands

### 1. Per-frame sky → SH on the CPU

- Each frame (or only when the sun/atmosphere changed), sample the Plan 06 sky in a fixed set of
  directions, project to `Sh9`, `ConvolveCosine` to irradiance SH, and upload the 9 `vec3`s into the
  lighting constants buffer (tiny — it rides the existing ring buffer, no new descriptor).

### 2. The third ambient arm

- The lighting shader's ambient block becomes three-way:
  ```
  IblEnabled    ? (diffuseIbl + specularIbl) * EnvIntensity * ao
  : SkylightOn  ? EvalIrradiance(skyCoeffs, N) * albedo * ao
  :               AmbientColor * albedo * ao   // unchanged last-resort fallback
  ```
- A `SceneRendererSettings` toggle (`Skylight`) drives the recompile; an intensity scale rides the
  per-frame push. The flat constant stays as the final fallback for a scene with neither IBL nor a
  skylight.

## Files (sketch — the agent confirms against the tree)

- `engine/include/Veng/Renderer/SceneRenderer.h` — the `Skylight` settings toggle + intensity.
- `engine/src/Renderer/SceneRenderer.cpp` — the per-frame project-and-upload; pass the SH block to the
  lighting constants.
- `engine/assets/core/shaders/deferred_lighting.frag.slang` — the SH coefficient block, the
  `EvalIrradiance` GPU function (matching Plan 05's basis exactly), the three-way ambient branch.
- `engine/assets/core/shaders/` — a shared `sh.slang` if the GPU eval is reused elsewhere.

## Examples to co-migrate

`hello-triangle`, with Plan 06's atmosphere enabled, turns **`Skylight` on** so the scene's ambient
tracks the sun — the day/night ambient demonstration. The flat-fallback and IBL paths are unchanged for
scenes that use them. `template` stays on its defaults.

## Verification

- The ambient term shifts with sun direction (blue-overhead vs warm-at-horizon) and matches a
  CPU-computed reference at a fixed sun angle.
- **CPU↔GPU SH consistency** — a test that the shader `EvalIrradiance` and Plan 05's CPU
  `EvalIrradiance` agree for the same coefficients and normals (the basis-mismatch guard).
- `smoke_golden` per Plan 06's opt-in decision: if the sample ships the atmosphere + skylight on, the
  golden is regenerated once (already covered by Plan 06's regen); otherwise it holds with the skylight
  off by default.
- Validation gate clean.

## Risks

- **Basis mismatch CPU vs GPU** — the single biggest SH footgun. The shader eval must use Plan 05's
  exact normalization/basis; the consistency test above is the gate, not eyeballing.
- **std140 layout of the SH block** — 9 `vec3`s in a uniform buffer pad to `vec4` stride unless laid
  out deliberately; mirror the C++ upload and the shader struct (the recurring material-param vec
  layout trap applies here too).
- **Per-frame cost** — sampling the sky for projection each frame is cheap but not free; gate the
  re-projection on an actual sun/atmosphere change so a static sky projects once.
