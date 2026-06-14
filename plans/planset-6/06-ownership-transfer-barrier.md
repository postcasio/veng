# Plan 06 — cross-queue ownership transfer (the barrier rule)

**Goal:** make the barrier decision **queue-family-aware**: a resource uploaded on
the transfer queue and first used on the graphics queue needs a **queue-family
ownership transfer** — a release barrier on the transfer side (recorded by the
upload, plan 07) and an **acquire** barrier on the graphics side (emitted on first
graphics use). When the families match (MoltenVK), both halves are a no-op. This
plan lands the **pure decision rule + its unit tests** (the part testable without a
device); the *emission site* that reads a producing-family marker is wired in
plan 07, where a producer first exists to exercise it.

## Why this is its own plan

Queue-family ownership transfer is "the piece most easily gotten wrong" (the
threading doc) and the smoke render cannot catch a missing or mismatched half — a
GPU race shows up, if at all, as a flicker on hardware that isn't the dev box.
planset-3 already extracted `DecideBarrier`/`ScopeFor` as a **pure data→data
function with unit tests** precisely so this kind of rule can be reasoned about and
covered without a GPU. Landing the decision logic here, alone, with exhaustive unit
tests over the family-match / family-differ cases, means plan 07 only has to
*record* the barriers the rule decides — and the dual-queue path (which no MoltenVK
dev box executes) is verified by the test, not left to chance.

## Background

The render graph **derives barriers from declared use** — a pass declares the views
it writes (`.Color`) and reads (`.Sample`), and the graph emits the layout
transitions. planset-3 pulled the core decision into `DecideBarrier` / `ScopeFor`
(`engine/include/Veng/Renderer/Backend/BarrierDecision.h` + `tests/unit`). It is a
**pure data→data function in `Veng::Renderer::Backend`** — it already traffics in
`vk::ImageLayout` / `vk::PipelineStageFlags` / `vk::AccessFlags` (it is a *backend*
header, never in a public header, so this is fine and does not affect
`include_hygiene`). "Pure" here means *no device, no handles, deterministic
data→data* — **not** "no `vk::` types." This plan adds a **queue dimension**: the
rule already knows the last and next use; it now also takes the **source** and
**destination queue family indices** (plain `u32`).

## Work (this plan: the rule + tests only)

1. **Extend the pure rule.** Give `DecideBarrier` the source/destination queue
   family indices. It returns, in addition to the existing src/dst
   stage+access+layout:
   - `srcQueueFamilyIndex` / `dstQueueFamilyIndex` — set to the transfer and
     graphics families for the **acquire** half on first graphics use of a
     transfer-produced resource;
   - **both `VK_QUEUE_FAMILY_IGNORED`** when the families match — the transfer is a
     no-op and the barrier degenerates to the ordinary same-queue layout transition.
   Keep the two halves' indices **paired** (the release half plan 07 records uses
   the same src/dst) and signal "skip both" when families are equal.

2. **Define the producing-family marker's *shape*** (a small field the rule keys
   off — "last produced on family X"), but **do not wire its emission**: the marker
   is *set* by plan 07's upload and *read* at the graph's emission site, which
   plan 07 adds. With no producer yet, every resource reads "graphics," so the new
   acquire branch is inert this plan — which is why the emission site lands in 07,
   not here (it cannot be exercised until a transfer producer exists).

## Tests (`tests/unit`, no GPU)

Extend the existing `DecideBarrier` cases:

- **Families differ** (discrete transfer queue): first graphics use of a
  transfer-produced image yields an acquire barrier with `src=transfer`,
  `dst=graphics`, correct layout/stage/access. This is the path **no MoltenVK dev
  box runs** — the unit test is its only coverage on the dev platform, which is the
  whole point of keeping the rule a device-free data→data function.
- **Families match** (MoltenVK): the same scenario yields `IGNORED`/`IGNORED` and
  degenerates to the plain same-queue transition — no ownership transfer.
- A graphics-produced, graphics-consumed resource is unchanged by the new
  dimension (regression guard).

## Dependencies

Independent in code (it extends the pure rule). Conceptually paired with plan 07,
which sets the producing-family marker and adds the emission + release halves. Land
06 first so 07 records against a tested decision.

## Acceptance

- Clean build, `ctest` green with the new `DecideBarrier` queue-family cases.
- The rule stays a **pure data→data function** (no device, no handles) in the
  backend namespace, same as planset-3 — only plan 07's *emission* site issues the
  actual barrier.
- No behaviour change on the dev box (families match → no-op); the differ-case is
  proven by the unit test and exercised end-to-end in plan 07.

## Notes

- This is the planset's analogue of planset-4/01: isolate the one load-bearing,
  hard-to-observe correctness change, land it alone, and cover it with a test the
  dev hardware can't substitute for.
- The release half and the emission site are **not** here — both are in plan 07
  (`cmd.ReleaseToQueue(graphicsFamily)` on the upload, the acquire emission on first
  graphics use), using the paired indices this rule decides.
</content>
