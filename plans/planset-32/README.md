# planset-32 — the render allocation sizes itself

**Phase goal:** make the render-target **allocation** size *self-determining*, and stop a HiDPI
display from supersampling its backing extent. Today dynamic resolution has a fast inner loop —
`ComputeDynamicResolutionScale` eases a viewport's per-frame `RenderScale` toward a GPU-frame-time
budget, rendering into a sub-rect of high-water-mark-allocated targets ([`DynamicResolution.h`](../../engine/include/Veng/Renderer/DynamicResolution.h)) — but the **allocation** those targets
are sized at is a **hand-picked `MaxScale`** (`Viewport::AllocationScale()` returns the static
`DynamicResolutionSettings::MaxScale`). That is the knob this planset removes. And on a HiDPI display
the managed viewport sizes that allocation to the **swapchain framebuffer extent** — 2× the logical
window — so every render-graph image (a deferred g-buffer's 4 MRTs + depth + HDR + lit + TAA history
+ bloom pyramid + SSAO + SSR + hi-Z + output + gather + composite) is allocated at twice the pixels
the small window actually needs. Scaling the sub-rect down barely helps there, because the cost that
hurts on a unified-memory MoltenVK device — the allocation footprint and the full-allocation tail
(the terminal tonemap/upscale pass, the gather + swapchain composite, the TAA history-copy) — does
not scale with the sub-rect.

This planset adds the **slow outer loop**. The per-frame sub-rect stays the free, continuous knob;
the allocation becomes a **lazy, quantized, hysteretic, dwell-gated follower** of the sustained
sub-rect scale, so the allocation tracks what the scene actually sustains without the manual `MaxScale`
and **without thrashing** — the expensive `Resize` never reacts to a fast signal. Separately, the
HiDPI baseline becomes a **budget decision** (an allocation cap relative to the backing extent) rather
than the backing extent itself, so the steady state is right and the outer loop is only a safety net
for genuine sustained overload.

## The shape

```
  GPU frame time ─►[inner loop]─► RenderScale ∈ [MinScale, allocScale]   per frame · FREE (sub-rect)
                  ComputeDynamicResolutionScale         │                  (built — planset unchanged)
                                                long EMA │ (seconds)
                                                         ▼
                  [outer loop]─► allocation tier ∈ {1.0, 0.75, 0.5}       seconds · RARE (Resize)
                  StepAllocationTier  ·  quantized · hysteresis band · asymmetric dwell
                                                         │ on tier change (debounced, safe moment)
                                                         ▼
                                  SceneRenderer::Resize(round(region · cap · tier))

  HiDPI:  swapchain backing extent (≈2× points) ──cap──► allocation baseline   steady-state fix
                                            MaxAllocationScale / point-size budget
```

- **Two loops, two timescales, one physical quantity.** The inner loop (built) sets the per-frame
  sub-rect `RenderScale` from GPU frame time — continuous, free, reacts to every spike. The outer loop
  (new) sets the **allocation tier** from a multi-second EMA of that sub-rect scale — quantized, rare,
  reacts only to what the scene *sustains*. The allocation is always ≥ the sub-rect, so the sub-rect
  rides *inside* the chosen allocation and normal DRS fluctuation never pokes the ceiling.
- **The expensive knob never reacts to a fast signal.** This is the entire anti-thrash guarantee. The
  allocation moves only across **wide hysteresis gaps** (step down below one threshold, back up only
  above a higher one) after a **sustained dwell** (asymmetric: quicker to shrink under sustained load,
  slow to grow back), and only at coarse **quantized tiers** (few tiers ⇒ few possible transitions).
  A scene parked near a boundary cannot oscillate the allocation.
- **Reallocating is visually continuous (pixel count), not free (frame time).** A tier change is the
  same scale at two timescales: shrinking the allocation 1.0→0.5 turns a sub-rect riding at "0.5-of-1.0"
  into "1.0-of-0.5" — the rendered pixel count is unchanged across the `Resize`, so quality does not
  pop. The `Resize` itself still pays a real one-frame hitch (it retires every target, re-registers
  bindless, recompiles the graph); the dwell makes that rare and already-sustained. The outer loop only
  stops paying footprint/bandwidth for an allocation it was not using.
- **The HiDPI baseline is a budget, not the backing extent.** The managed viewport caps the allocation
  relative to the swapchain framebuffer extent (a `MaxAllocationScale` / logical-point budget), so a 2×
  HiDPI display is not silently supersampled. With the baseline right, the outer loop rarely fires — it
  is the safety net for sustained overload, not the steady state.

## The spine — four principles

1. **Allocation follows the sustained sub-rect, lazily.** The fast knob does all the fast work; the
   allocation is a slow, quantized follower of the inner loop's settled operating point (a long EMA of
   `RenderScale`). It never measures perf directly — it reads the inner loop's output, which already is
   the perf response.

2. **The expensive knob is thrash-proof by construction.** Quantized tiers + a hysteresis band per
   boundary + asymmetric dwell timers + reallocation only on a tier *change* mean the allocation moves
   at most once every few seconds and only after a durable regime shift. Thrashing is impossible because
   nothing fast enough to thrash is wired to it.

3. **A tier change is visually continuous.** The allocation and the sub-rect are one quantity; a `Resize`
   to match the sustained sub-rect keeps the rendered pixel count constant, so the follower never causes
   a visible step. A down-step only fires once the instantaneous sub-rect already fits the smaller tier,
   so the continuity holds exactly rather than approximately. Reallocation is pure reclamation, not a
   quality decision.

4. **The HiDPI cap is steady-state; the controller is the safety net.** Baseline allocation is a budget
   decision decoupled from the backing extent. The outer loop catches genuine sustained overload from a
   sane baseline, rather than papering over a 2× supersample no one asked for.

## What already exists (built; unchanged here)

- `DynamicResolutionSettings` + `ComputeDynamicResolutionScale` — the pure inner-loop frame-time
  controller (deadband, rate limit, scale² cost model).
- `Viewport::SetDynamicResolution` / `UpdateDynamicResolution` / `m_RenderScale` /
  `AllocationScale()` / `ViewRenderScale()` — the per-frame sub-rect drive; `SceneRenderer` renders the
  `round(allocExtent · RenderScale)` sub-rect and the terminal tonemap upscales it.
- `ManagedViewportInfo::DynamicResolution` — the managed-viewport opt-in.

The static `AllocationScale() == MaxScale` is the seam this planset replaces; the inner loop is reused
verbatim as the outer loop's input signal.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Allocation-tier controller | A pure, device-free `AllocationTierSettings` + `AllocationTierState` + `StepAllocationTier` beside the inner-loop controller in `DynamicResolution.h`: a long EMA of the sub-rect scale → a quantized tier through a hysteresis band + asymmetric dwell timers, with a down-step guard that keeps a tier change visually continuous. Lowers the inner-loop `MinScale` default to `0.25`. Unit-tested for no-oscillation (noise *and* slow drift), dwell, hysteresis, and the down-step guard. Foundational, purely additive. | done |
| 01 | HiDPI allocation cap | `MaxAllocationScale` (cap on the allocation relative to the swapchain framebuffer extent) on `ViewportInfo`/`ManagedViewportInfo`, applied at one point — the viewport's allocation-extent derivation. Decouples the allocation baseline from the 2× backing extent. Independent of 00. | done |
| 02 | Viewport tier wiring | Drive the allocation tier from the inner loop: `Viewport` tracks the long EMA of `m_RenderScale`, steps the tier via `StepAllocationTier`, and on a tier change debounces a `SceneRenderer::Resize(round(region · cap · tier))` — replacing the static `AllocationScale() == MaxScale`. Migrate hello-triangle (auto allocation; expose the tier in Stats). Depends on 00 (+ 01 for the baseline). | done |
| 03 | Docs + roadmap | `engine/CLAUDE.md` (the two-loop allocation model on the `SceneRenderer`/`Viewport` sections), `future/README.md` area 8, this record. Full `ctest` + `smoke_golden` + `validation_gate` green. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

**00** (the pure controller) and **01** (the HiDPI cap) are independent and can land in parallel.
**02** depends on 00 (it calls `StepAllocationTier`) and reads better atop 01 (a sane baseline means
the tier rarely fires), but does not strictly require 01. **03** is last. Worktree-isolated parallel
dispatch should branch 02 from the 00(+01) integration commit — see [[project_megaexec_worktree_base]].

**Shared-file caveat:** 00 and 02 both touch `DynamicResolution.h` (00 adds the tier API + the `MinScale`
default; 02 only consumes the tier API) and 01 and 02 both touch `Viewport.h`/`Viewport.cpp` and
`Application.h`/`Application.cpp` (01 adds the cap to the extent derivation, 02 adds the tier state + the
debounced `Resize`). Merge in number order (01 then 02) rather than expecting a conflict-free parallel
merge on those files.

## The decisions this planset settles

- **The allocation is self-determining.** The hand-picked `MaxScale` allocation goes away; the
  allocation tracks the sustained sub-rect through a quantized, hysteretic, dwell-gated outer loop. The
  cost is a small amount of controller state on `Viewport` and a rare, debounced `Resize`.
- **Anti-thrash is structural, not tuned.** The guarantee that the allocation cannot oscillate comes
  from *what drives it* (a multi-second EMA, never instantaneous perf) plus quantization + hysteresis +
  dwell — not from carefully chosen magic numbers. Tuning the tiers/dwell changes responsiveness, never
  correctness.
- **A tier change is visually continuous by construction.** The allocation and the sub-rect are one
  quantity and a down-step only fires once the instantaneous sub-rect already fits the smaller tier, so
  the rendered pixel count is preserved across the `Resize` and the consumer's `ViewRenderScale` clamp is
  never hit. The floor tier degenerating to fixed-scale (a tier at `MinScale`) is an accepted terminal
  state, not a bug.
- **HiDPI is a budget, not a backing extent.** The allocation baseline is decoupled from the swapchain
  framebuffer extent, so the steady state on a HiDPI display is right and the controller is a safety net.
  The cap, the tier, and the sub-rect compose as three multiplicative scales
  (`round(region · MaxAllocationScale · tier) · ViewRenderScale`), the cap outermost.

## What remains (future)

**Safe-moment reallocation** — deferring the tier-change `Resize` to a scene transition / static camera
/ loading screen rather than firing it inline (the dwell already makes the inline hitch rare and
acceptable; this is a polish refinement). **Memory-driven tier capping** — choosing the *initial* tier
from a device memory query (`VkPhysicalDeviceMemoryProperties` budget) so a memory-starved device starts
low, distinct from the perf-driven outer loop. This is the seam where this planset meets **planset-33**
(texture compression): planset-33's ~8:1 block compression materially changes the texture VRAM residency
this initial-tier query would read, so the memory-budget follow-on should account for compressed textures
once both are in. **A history ping-pong** to remove the TAA history-copy from the full-allocation tail
(the one full-res cost the sub-rect cannot reduce — it scales with allocation, so a tier step *does* help
it; the ping-pong removes it entirely), the next lever if TAA is still too expensive once the allocation
is right. These stay named follow-ons behind this planset's two-loop seam.
