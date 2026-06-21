# Plan 02 — selectable Dual Kawase kernel

**Goal:** make the pyramid's downsample/upsample filter a **selectable kernel** —
`SceneRendererSettings::BloomKernel { Cod, Kawase }`, default `Cod`. The `Cod` path
(Plan 01's 13-tap-down / 3×3-tent-up dual filter) is the documented reference and the
kernel the golden is blessed against; `Kawase` is the **Dual Kawase** filter (Bjørge,
"Bandwidth-Efficient Rendering") — a cheaper 5-tap-down / 8-tap-up bilinear pattern
designed for the tile-based, bandwidth-bound GPUs veng primarily targets. Depends on
Plan 01 (the pyramid, the compute pipelines, the pass wiring).

## What lands

### The kernel setting

```cpp
enum class BloomKernel { Cod, Kawase };
BloomKernel Kernel = BloomKernel::Cod;   // topology: selects the per-level filter
```

The field is named `Kernel` (not `Bloom_Kernel` — house style forbids the embedded
underscore, and `Settings.Kernel` is unambiguous, exactly as `Settings.Mode` already names
the `DebugView`). The *type* keeps the descriptive name `BloomKernel`.

The kernel choice changes the **per-level compute shader**, so it is a topology knob — a
`Configure` recompile, like the `Bloom` toggle — not a per-frame push. (`Threshold` /
`Intensity` / `Radius` stay per-frame.)

### The Kawase tap variant in the down/up shaders

`bloom_down.comp.slang` and `bloom_up.comp.slang` gain the Kawase tap pattern alongside the
COD one, selected at pipeline build:

- **Selection mechanism — a Slang specialization constant** (`[SpecializationConstant]` /
  `constexpr` branch) baked at `ComputePipeline::Create`, so each kernel compiles to a
  **branch-free** inner loop (no per-invocation uniform branch in the hot path). The renderer
  builds the down/up pipelines for the active `Kernel` in `Create`/`Configure` and rebuilds
  them when the setting changes — the same recompile seam `Configure` already drives.
  **Pre-check before implementing:** confirm `ComputePipeline::Create` plumbs
  `VkSpecializationInfo` and the cooker's Slang reflection round-trips a spec constant into the
  `ShaderInterface` (a short spike — veng cooks SPIR-V offline and the engine loads it, so
  spec-constant support is not a given). **Trip-wire to the fallback:** if Slang reflection
  drops the spec constant from the `ShaderInterface`, the cooker rejects it, or
  `ComputePipeline::Create` has no `VkSpecializationInfo` path, use the fallback — two `.slang`
  files per sweep (four total), the setting picking the pipeline. The spec-constant
  single-source form is preferred to keep the tap math in one place; the fallback only
  **adds** the Kawase path and must not alter the `Cod` SPIR-V, so the `Cod` golden cannot
  move under it.
- **Down (Kawase):** the 5-tap bilinear downsample (center + four diagonal half-texel taps)
  in place of the 13-tap; the level-0 bright-pass + Karis weighting is **unchanged** (it
  wraps whichever downsample kernel runs).
- **Up (Kawase):** the 8-tap bilinear upsample in place of the 3×3 tent; the in-place
  accumulate + `Radius` scale is unchanged.

The pyramid resource, the per-level dispatch loop, the composite, and the barriers are all
**identical** across kernels — only the tap pattern inside the down/up dispatch differs, so
this is a contained shader + pipeline-selection change, no plumbing churn.

## Decisions

1. **Kernel is a `Configure` topology knob, not a per-frame push.** Switching kernels
   swaps the compiled pipeline; it is a rare authoring/quality choice, so a recompile is
   acceptable and keeps both inner loops branch-free. This matches how `Bloom` on/off and
   the `DebugView` mode already drive recompiles.

2. **`Cod` stays the default and the golden's kernel.** The reference 13-tap/tent kernel is
   the documented, widely-validated baseline; the `smoke_golden` is blessed against it in
   Plan 01 and **does not move** here. Kawase is opt-in.

3. **Karis weighting wraps the kernel, not the reverse.** Firefly suppression is a property
   of the HDR → mip 0 step regardless of tap pattern, so it sits outside the kernel branch —
   both kernels get a stable mip 0.

4. **Spec-constant single source preferred over duplicated shaders.** One `bloom_down` /
   one `bloom_up` source with a kernel spec constant keeps the two tap patterns side by side
   and avoids a four-file fan-out; the two-file form is the documented fallback only if the
   cooker's Slang reflection path does not handle the spec constant cleanly.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `BloomKernel` enum + the `Kernel` setting on `SceneRendererSettings`. |
| `engine/assets/core/shaders/bloom_down.comp.slang` | Add the Kawase 5-tap downsample behind a kernel spec constant. |
| `engine/assets/core/shaders/bloom_up.comp.slang` | Add the Kawase 8-tap upsample behind a kernel spec constant. |
| `engine/assets/core/core.vengpack.json` | (Only if the fallback two-file form is used — the extra Kawase shader entries + minted ids.) |
| `engine/src/Renderer/SceneRenderer.cpp` | Build the down/up pipelines for the active `BloomKernel`; rebuild on the setting change through the `Configure` recompile. |
| `tests/gpu/scene_renderer.cpp` | A `gpu` assertion that `BloomKernel::Kawase` also blooms a bright region (the property, not a second golden). |

## Verification

- Clean build; both kernels compile; the default `Cod` path's SPIR-V is unchanged from
  Plan 01, so the `smoke_golden` does **not** move. (Whether the `Cod` source gains a spec
  constant or stays a separate file, the `Cod` binary — and thus the golden — must not change;
  that is the property to hold, not byte-identity of the source.)
- `gpu` band: with `Kernel = Kawase`, the bright-region property assertion passes;
  switching the kernel triggers a `Configure` recompile and produces a valid (wider/softer
  or comparable) bloom.
- `ctest -L validation` clean — the dispatch/barrier structure is identical across kernels,
  so no new validation surface beyond Plan 01's.
