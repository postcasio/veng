# Plan 06 — `CullMode` settings + debug + docs/roadmap

**Goal:** expose the GPU path through a `SceneRenderer` setting, surface the cull stats in the
example debug UI, fall back to the CPU path where `multiDrawIndirect`/`drawIndirectFirstInstance`
is absent, and re-cut the docs and roadmap so the delivered GPU-driven culling and its named
refinements stay legible. Closes the planset; depends on Plan 05.

## What lands

### The `CullMode` setting ([engine/include/Veng/Renderer/SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h) or the settings header)

`SceneRendererSettings` grows, beside the existing `FrustumCull`:

```cpp
enum class CullMode : u8
{
    CPU,   ///< BVH frustum descent on the CPU; the planset-21/23 path. Default.
    GPU,   ///< BVH frustum descent + GPU hi-Z occlusion + indirect draw (Plan 05).
};

CullMode Cull      = CullMode::CPU;   ///< Selects the cull/submit path. A recompile knob.
bool     Occlusion = true;            ///< GPU-path hi-Z occlusion on/off (frustum-only when off).
```

Both are **recompile knobs** — `CullMode::GPU` is a different pass topology (the cull compute
pass + indirect geometry pass replace the direct draw loop), so changing `Cull` runs through
`Configure` and invalidates `GetOutput()` (the deliberate recompile seam, unlike planset-21's
`FrustumCull`, which is a per-frame branch within one topology). `Occlusion` gates the
`IsOccluded` call within the GPU topology; toggling it is also a `Configure` since it changes the
compute pass's declared hi-Z read. The doc comment on each states this.

### CPU fallback where the feature is absent

`Create`/`Configure` query whether `multiDrawIndirect` **and** `drawIndirectFirstInstance` were
enabled (Plan 04's feature gate). If `Cull == GPU` is requested on a device without both, the
renderer **logs once at `WARN` and silently uses `CullMode::CPU`** — the GPU path is an optimization, not a correctness requirement,
and a one-cube scene renders identically either way. `GetActiveCullMode()` reports the path
actually in use (which may differ from the requested setting on an unsupported device), so the
debug UI and tests can assert on reality.

### Debug stats (the example, [examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp))

The example's debug window gains a cull section reading the renderer's stats:

- **gathered** — `GetLastVisibleCount()` (per-submesh candidates, Plan 01).
- **frustum-survived** — the BVH `Cull` survivor count (a new `GetFrustumSurvivedCount()`).
- **occlusion-survived / drawn** — `GetLastDrawnCount()` (per-submesh, Plan 01; the GPU
  survivor-count readback under `CullMode::GPU`, Plan 05).
- **active cull mode** — `GetActiveCullMode()` (so a fallback to CPU on an unsupported device is
  visible), plus the existing `DidBroadphaseRebuildLastFrame()` / `GetBroadphaseNodeCount()`.
- A `CullMode` selector + an `Occlusion` toggle (driving `Configure`), and the existing
  pause-spin toggle so a still scene's occlusion drop is demonstrable frame to frame.

### Docs + roadmap re-cut

- **[engine/CLAUDE.md](../../engine/CLAUDE.md)** — the `SceneRenderer` section's cull paragraph
  becomes "culls at submesh granularity through the BVH broadphase; under `CullMode::GPU`, a
  compute pass hi-Z-occludes the frustum survivors and issues them through `vkCmdDrawIndexedIndirect`",
  and the broadphase refinement list drops "GPU/occlusion culling" and "per-submesh leaves" from
  *future* to *delivered*. Present-tense facts, no plan-citation narrative.
- **[future/scene-renderer.md](../future/scene-renderer.md)** — mark **per-submesh leaves** and
  **GPU/occlusion culling** delivered (planset-25); the surviving named refinements become
  meshlet/cluster-granularity GPU culling, two-pass occlusion (the higher-quality alternative to
  temporal hi-Z), GPU compaction into a count buffer (behind a future MoltenVK `drawIndirectCount`),
  and **GPU-driven shadow-caster culling** (the highest-payoff consumer — planset-24's `N + 6N`
  shadow views). Add the verified `drawIndirectCount`/`multiDrawIndirect`/`drawIndirectFirstInstance`
  finding beside the existing multiview finding so the platform constraint stays recorded.
- **[plans/README.md](../README.md)** — the planset-25 row to delivered; the status table.

## Decisions

1. **`CullMode` is a recompile knob, semantically.** Unlike planset-21's `FrustumCull` (a runtime
   branch inside one topology), the GPU path is a different pass set, so it must run through the
   `Configure` recompile seam and accept the `GetOutput()` invalidation that implies. Stating this
   on the setting keeps a `Configure`-per-frame mistake from looking cheap.

2. **CPU is the default and the silent fallback.** The primary platform's MoltenVK lacks
   `drawIndirectCount`; even with the `multiDrawIndirect` shape, the GPU path is the more
   complex, less-portable arm. Defaulting to CPU and falling back to it on an unsupported device
   (logging once) means the smoke/golden path and any device without the feature keep working with
   no per-call branching, and `GetActiveCullMode()` makes the fallback observable rather than
   silent-and-confusing.

3. **The stats name the whole funnel.** gathered → frustum-survived → occlusion-survived → drawn
   makes each stage's contribution visible, so a regression (the cull dropping too much, the
   occlusion dropping nothing) is legible in the debug UI, not just in a test. This is the
   debug-surface analogue of planset-23's rebuilt/node-count stats.

4. **The roadmap names shadow culling as the next payoff explicitly.** GPU-driven culling's
   biggest win is the many-view shadow workload (decision 9 of the planset), which this planset
   deliberately defers. Recording it as *the* highest-payoff follow-on — not a parenthetical —
   keeps the direction honest about where the GPU path goes next.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` (or settings header) | `CullMode` enum; `Settings::Cull`/`Occlusion`; `GetActiveCullMode()`. |
| `engine/src/Renderer/SceneRenderer.cpp` | Wire `Cull`/`Occlusion` through `Configure`; the feature-gated CPU fallback + the `WARN`-once log. |
| `examples/hello-triangle/main.cpp` | The cull-stats debug section + the `CullMode`/`Occlusion` controls. |
| `engine/CLAUDE.md` | The `SceneRenderer` cull paragraph + the delivered/future refinement split. |
| `plans/future/scene-renderer.md` | Per-submesh leaves + GPU/occlusion culling delivered; the verified `drawIndirectCount` finding; the re-cut refinement list. |
| `plans/README.md` | planset-25 row → delivered. |

## Verification

- Clean build; `include_hygiene` still compiles `Veng/Renderer/SceneRenderer.h` (the `CullMode`
  enum + getters are vk-free vocabulary types).
- **Settings behavior** (`gpu` band where the feature is present): setting `Cull = GPU` then
  `Configure` switches the active path (`GetActiveCullMode() == GPU`) and produces a byte-identical
  golden under frustum-only; `Cull = GPU` on a device without `multiDrawIndirect` reports
  `GetActiveCullMode() == CPU` and logs once — the fallback is observable and correct.
- **`Occlusion` toggle:** on the provably-occluded fixture, `Occlusion = true` draws strictly
  fewer submeshes than `Occlusion = false`, golden unmoved (Plan 05's guard, surfaced through the
  setting).
- **The example debug UI** shows the gathered/frustum/occlusion/drawn funnel and the active cull
  mode; the smoke path (default `CullMode::CPU`) writes a correct-sized PPM and exits 0.
- **`smoke_golden` byte-identical**; docs build (Doxygen target where installed) is clean.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present;
  validation gate clean under `VE_DEBUG` in both `CullMode`s. Update this planset's status table
  and `plans/README.md`.
</content>
