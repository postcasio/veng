# Plan 08 — remove the allocation-tier outer loop (it hitches); keep the sub-rect inner loop

**Goal:** remove planset-32's **slow outer loop** — `StepAllocationTier` and the tier-change
`SceneRenderer::Resize` it debounces, plus the max-allocation **discovery** that loop drove — because
a tier-change reallocation **hitches**. Keep the **fast inner loop**: `ComputeDynamicResolutionScale`
eases a per-frame sub-rect `RenderScale` over a **fixed** allocation, which never reallocates and never
hitches. The allocation becomes a fixed ceiling (native extent, capped by `MaxAllocationScale`). Then
update the roadmap to tell the truth: the inner loop is the dynamic-resolution mechanism; the
auto-tier outer loop was tried and removed. **Supersedes planset-32** (the outer loop) and moots
planset-34 Plan 00's "let the tier discover the operating point."

## Why remove it

planset-32 sized the allocation by folding a multi-second EMA of the sub-rect scale into a quantized
tier through hysteresis + dwell, debouncing a `Resize` on each tier change. The dwell made the hitch
*rare*, not *absent* — and a reallocation hitch in the steady state is worse than running at a fixed
allocation. The inner loop already adapts per-frame cost with **zero** reallocation by rendering into a
sub-rect; the outer loop's only job was to reclaim the *allocation footprint*, and it cannot do that
without a `Resize`, which is the hitch. So the honest trade is: keep the cost-adaptation that does not
hitch, drop the footprint-reclamation that does.

## The starting point

- `Veng/Renderer/DynamicResolution.h` holds both controllers: `ComputeDynamicResolutionScale` (inner,
  per-frame sub-rect) and `StepAllocationTier` (outer, EMA → quantized tier).
- The `Viewport` debounces a `SceneRenderer::Resize` on a tier change; `MaxAllocationScale` caps the
  allocation baseline against the swapchain framebuffer extent (the HiDPI decoupling).
- planset-34 Plan 00 lifted the sample's `MaxAllocationScale` to `1.0` specifically to let the tier
  discover the operating point.

## What lands

### 1. Delete the outer loop

- Remove `StepAllocationTier` and its EMA / hysteresis-band / dwell-timer state; remove the
  `Viewport`'s tier-change `Resize` debounce. The allocation is sized **once** to the region's native
  extent, capped by `MaxAllocationScale`, and resized only on a genuine region/window extent change —
  not on load.
- This is a **public-API removal**, not just an internal one: `ManagedViewportInfo::AllocationTier`
  (`Application.h`), `Viewport::GetAllocationTierIndex()` / `IsAllocationTierEnabled()`, and the
  `tierSettings` parameter of `SetDynamicResolution` all go. The tier debug UI in
  `engine/src/UI/DebugPanels.cpp` (which calls `GetAllocationTierIndex` / `IsAllocationTierEnabled`) and
  both examples' call sites are migrated in the same pass, or they fail to compile.
- **Keep** `ComputeDynamicResolutionScale` (the inner loop) unchanged: it eases `RenderScale` over the
  now-fixed allocation toward the GPU-frame-time budget, exactly as today, with no reallocation.
- **Keep** `MaxAllocationScale` as the **static** ceiling (the HiDPI decoupling is still wanted — a
  small window on a 2× display should not allocate at 2×). It is now a fixed cap, not a tier input.

### 2. Update the roadmap (this plan's roadmap half)

- `plans/README.md` planset-32 entry — note the outer loop was removed in planset-39 for hitching; the
  delivered-and-kept part is the inner loop + the `MaxAllocationScale` HiDPI cap.
- `plans/future/README.md` area 8 — the planset-32 "delivered" paragraph gets the same correction;
  **safe-moment reallocation** is **dropped** (there is no tier `Resize` left to defer); the
  **memory-driven initial tier** follow-on is rephrased as a **memory-driven fixed-allocation choice**
  (pick the one fixed allocation from a device memory-budget query); the **TAA history ping-pong**
  follow-on stays (it is independent of the tier).
- `plans/README.md` planset-34 entry — soften the "the tier discovers the operating point" phrasing,
  since the tier is gone.

## Files (sketch — the agent confirms against the tree)

- `engine/include/Veng/Renderer/DynamicResolution.h` + impl — delete `StepAllocationTier`,
  `AllocationTierSettings`, `AllocationTierState` and their EMA/hysteresis/dwell state; keep
  `ComputeDynamicResolutionScale`. **Rewrite the `MinScale` doc comment** — it currently reads "Sits
  strictly below the outer-loop floor tier (`AllocationTierSettings` default 0.5)…", a reference to the
  deleted concept (and a house-rule stale-narrative smell). State `MinScale` as the floor of the
  per-frame sub-rect over the fixed allocation.
- `engine/src/Renderer/Viewport.*` — remove the tier-change `Resize` debounce; size the allocation once
  from the region extent + `MaxAllocationScale`. Delete `GetAllocationTierIndex` / `IsAllocationTierEnabled`.
- `engine/include/Veng/Application.h` + `Application.cpp` — drop the `ManagedViewportInfo::AllocationTier`
  field and its plumbing; `SetDynamicResolution` loses the `tierSettings` parameter.
- `engine/src/UI/DebugPanels.cpp` — remove the allocation-tier readout and the auto/static toggle.
- Both examples — drop the tier call sites. `hello-triangle` keeps `MaxAllocationScale = 1.0f`
  (`main.cpp`): it was lifted to `1.0` for tier discovery, but full-native is the intended sample
  ceiling — it is now simply the fixed cap, no longer a tier input. Note the why in passing; no other
  content change expected.
- Docs: the renderer/dynamic-resolution comments that described the outer loop; `engine/CLAUDE.md` if it
  documents the tier.
- Roadmap: `plans/README.md`, `plans/future/README.md` per the section above.

## Verification

- Under sustained load the reallocation hitch is **gone** (no `Resize` fires from frame-time pressure);
  the inner-loop `RenderScale` still adapts and recovers.
- `smoke_golden` holds — the smoke pose renders at `RenderScale` 1.0 over a fixed native allocation,
  unchanged from before.
- Clean build, full `ctest` green; grep proves no `StepAllocationTier` and no tier-change `Resize`
  remain.

## Risks

- **A memory-starved device** loses the tier's ability to shrink footprint under pressure — now it
  stays at the fixed allocation. The rephrased **memory-driven fixed-allocation** follow-on (pick a
  smaller fixed allocation up front from the memory budget) is the named replacement; this plan does
  not build it, it removes the thrashing path that stood in for it.
- **Region/window resizes** must still reallocate (a genuine extent change is not a hitch to avoid, it
  is correct) — keep that `Resize`; only the *tier-driven* one is removed.
