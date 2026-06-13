# Plan 04 — Extract & test the barrier-decision rule

**Goal:** make the render graph's barrier logic device-testable, then test it.
This is the one plan in the planset that **edits engine code** — a focused refactor
that pulls the pure decision out of the device-coupled path so it can be exercised
without a GPU.

## Why this needs a refactor

The barrier decision is currently split and entangled with live Vulkan state. The
real rule, read off [`Barrier.cpp`](../../src/Renderer/Backend/Barrier.cpp):

- Each image subresource carries tracked state `(Layout, Stage, Access)`, read via
  `native.At(layer, mip)`.
- `hazard = (src.Layout != newLayout) || IsWriteAccess(src.Access) ||
  IsWriteAccess(dstAccess)`, where `IsWriteAccess` masks the `*Write` access bits.
- **No hazard** (read-after-read, same layout): emit *no* barrier; widen the
  tracked read scope — `Stage |= dstStage; Access |= dstAccess` — keeping `Layout`.
  This makes a later write wait on every prior read.
- **Hazard**: emit an `ImageMemoryBarrier` `(src.Layout, src.Stage, src.Access) →
  (newLayout, dstStage, dstAccess)`, then overwrite tracked state to
  `(newLayout, dstStage, dstAccess)`.

The decision is genuinely pure; the only device-bound parts are reading/writing
`native.At(...)` (the tracked-state store), `Utils::GetAspectFlags`, and the actual
`pipelineBarrier` call. The `ScopeFor(AccessKind)` table in `RenderGraph.cpp`
(anonymous namespace) supplies the desired `(layout, stage, access)` and is already
pure.

## Work

1. **Extract the decision onto plain data.** Introduce a pure function (no
   `Image`, no `cmd`), e.g. in `Backend/BarrierDecision.{h,cpp}`:

   ```
   struct SubresourceState { vk::ImageLayout Layout; vk::PipelineStageFlags Stage; vk::AccessFlags Access; };
   struct BarrierDecision  { bool NeedsBarrier; SubresourceState NewState;
                             /* when NeedsBarrier: */ SubresourceState Src; SubresourceState Dst; };
   BarrierDecision DecideBarrier(const SubresourceState& current,
                                 vk::ImageLayout newLayout,
                                 vk::PipelineStageFlags dstStage, vk::AccessFlags dstAccess);
   ```

   Implements exactly the hazard rule above: no-hazard → `{NeedsBarrier=false,
   NewState=(current.Layout, current.Stage|dstStage, current.Access|dstAccess)}`;
   hazard → `{NeedsBarrier=true, Src=current, Dst=(newLayout,dstStage,dstAccess),
   NewState=(newLayout,dstStage,dstAccess)}`. `TransitionImage` becomes "read
   tracked state → `DecideBarrier` → emit barrier if `NeedsBarrier` → write
   `NewState` across the subresource range". Move `IsWriteAccess` alongside.
   **Behaviour-preserving** — byte-for-byte the same barriers as today.

2. **Re-verify the refactor on GPU.** Because validation errors don't fail tests
   (see [CLAUDE.md](../../CLAUDE.md)), confirm no regression by running
   `compute_dispatch` and `headless_smoke` under the `VE_DEBUG` validation build
   and grepping stderr for validation errors — the compute test is the one that
   exercises the cross-stage barrier chain. This step is why 04 stays
   review-on-main-thread even if the edit is delegated.

3. **Unit-test `DecideBarrier`** in `tests/unit/barrier_decision.cpp` (part of
   `veng_unit`, backend-header linkage as in plan 03):
   - First use from `Undefined` (layout change) → `NeedsBarrier`, `Dst` = desired,
     `NewState` = desired.
   - Same layout, read → read (e.g. `ShaderReadOnlyOptimal`, `eShaderRead` both
     sides) → **no barrier**; `NewState.Layout` unchanged, `Stage`/`Access` are the
     OR of current and desired (the scope-widen).
   - Same layout but `dstAccess` is a write (read → write) → `NeedsBarrier` (write
     hazard despite no layout change).
   - Current access is a write, next is a read, same layout (write → read) →
     `NeedsBarrier` (source write hazard).
   - `IsWriteAccess` classifies each `*Write` bit as a write and reads as not.
   - `ScopeFor` returns the documented `(layout, stage, access)` for each
     `AccessKind` (the table in `RenderGraph.cpp`).

## Dependencies

Needs plan 01. Independent of 02/03/05/06 in terms of *test* files, but it is the
only plan modifying `src/`, so sequence its merge to avoid colliding with any
concurrent engine edits. Delegatable to a subagent, **review + validation-build
verify on the main thread.**

## Acceptance

- `compute_dispatch` + `headless_smoke` still pass, and are **validation-clean**
  under `VE_DEBUG` to the same degree as before the refactor (no new validation
  errors; the known storage-image gap from CLAUDE.md is unchanged, not widened).
- New `barrier_decision` cases green under label `unit` with **no ICD** (the
  decision function is pure data → data).

## Notes

- Keep the public API untouched — this is internal-only; `RenderGraph.h` /
  `Barrier.h` signatures need not change (only their implementations move).
- The rule is fully known (see above) — this is a mechanical extract-and-cover, not
  a redesign. Preserve behaviour exactly; if you spot a *latent* barrier bug while
  extracting, note it separately rather than fixing it under this plan.
- The layout-only `TransitionImage` overload (the out-of-graph path that derives
  stage/access from the layout via `Utils::Get*Mask`) routes through the same
  explicit overload, so it inherits `DecideBarrier` for free — no second path.
