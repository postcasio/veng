# Plan 04 — splitscreen, as a tested capability

**Goal:** prove the inversion's headline payoff — **N `Presented` viewports assembled into
sub-rectangles of one window** — as a gpu test, with no sample feature. Register multiple
`Presented` viewports with quadrant regions, drive them through the gather + composite tail into an
offscreen target, and assert both halves: that the gather pass receives the viewports with the right
regions, **and** that the assembled/composited image actually carries each viewport's content in its
quadrant.

**Depends on Plan 02 (the drive-list + managed gather/composite driven from the `Presented` set).**

## Why it is its own plan

Splitscreen is the capability the placement inversion was made for, and it is exactly the kind of
thing that can pass a build while silently placing every quadrant at the origin. A dedicated test
that composites real viewports into quadrants and reads back the pixels is the guard that the
region-driven placement is correct — and it documents the "register N `Presented` viewports" pattern
without committing a sample asset. Keeping it test-only matches the decision to demonstrate the
capability now and defer a playable splitscreen sample.

## What lands

- **A headless splitscreen gpu test.** It:
  1. builds a scene and **N viewports** (e.g. two `Presented` viewports, left/right halves; or four
     quadrants), each with a distinguishing camera/clear so its content is identifiable, registered
     in a known order;
  2. drives one engine frame's render + gather + composite **into an offscreen target** (headless has
     no swapchain, so the composite writes a renderer-owned image, not a presented surface);
  3. asserts **both**: the gather pass's received placements match each viewport's `GetRegion()`
     (placement-level — via a `GetPlacements()`-style observation accessor on the gather pass, added
     in Plan 01 since the pass otherwise exposes no view of what it received), **and** the assembled
     offscreen image carries each viewport's distinguishing content within its quadrant and the clear
     elsewhere (pixel-level).

- **The offscreen gather+composite path for headless.** Headless has no swapchain, so the test
  passes an offscreen image view where the composite expects its per-frame swapchain view (the
  composite's target is already a parameter — the graph imports whatever view it is handed). The
  gather assembles into its owned target and the composite writes the offscreen image, read back
  without a window — a device-level test the gather + composite tail lacks today.

- **The "register N `Presented` viewports" pattern, exercised.** The test is the worked example of
  the splitscreen shape — multiple viewports, quadrant regions, registration order — that a future
  playable sample or a game would follow.

## Decisions

1. **Both assertions.** Placement-level (regions reached the gather pass) catches a wiring break;
   pixel-level (quadrants carry the right content) catches a scissor/placement-math break. They fail
   for different reasons, so the test checks both.

2. **Offscreen target, not a swapchain.** Splitscreen placement is a property of the gather pass's
   region math, not of presentation; routing the tail into a read-back offscreen target tests that
   math headlessly and keeps the test in the `gpu` band with no display.

3. **Tests only, no sample asset.** The capability is proven and the pattern documented without a
   `.vmat`/`AssetId` or a playable mode — a playable splitscreen sample is the named follow-on, not
   this planset's scope.

## Files

| File | Change |
|---|---|
| `tests/gpu/…` *(new)* | The splitscreen test: N `Presented` viewports → quadrant regions → offscreen gather + composite → placement + pixel assertions. |
| `tests/CMakeLists.txt` | Register the test (label `gpu`, `SKIP_RETURN_CODE 77`, under the validation band). |

## Verification

- Clean build; `ctest` green. The new gpu test passes and skips cleanly with no ICD.
- `validation_gate` green under `build-debug` running the test — the N-placement gather + composite raises no
  barrier/layout/binding validation error.
- `smoke_golden` / `hello_triangle_launcher_smoke` unaffected — no runtime sample path changes.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
