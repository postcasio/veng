# Plan 01 — the HiDPI allocation cap

**Goal:** stop the managed viewport from sizing its render allocation to the full **swapchain
framebuffer extent** on a HiDPI display. Add a **`MaxAllocationScale`** budget that caps the allocation
relative to that backing extent, so a small window on a 2× display is not silently supersampled across
every render-graph image. This is the **steady-state** fix; the outer-loop controller (Plans 00/02) is
the safety net layered on top. **Independent of Plan 00.**

## Why it is its own plan

The footprint problem on a HiDPI Mac is a *baseline* mistake — the allocation is twice the pixels the
window needs before any perf signal is involved — and it is fixed by one decision (what extent to size
the allocation at), not by the control loop. Splitting it from the tier wiring keeps it reviewable on
its own (a one-time extent derivation, no controller state) and lets it land as immediate relief even
if the outer loop is still in review. With the baseline right, the outer loop in Plan 02 rarely fires,
so the two compose cleanly but are not entangled.

## Background — where the extent comes from today

The managed viewport tracks the window by following the **swapchain framebuffer extent**
(`Window::glfwGetFramebufferSize`, in backing pixels), which on a HiDPI display is the logical window
size times the OS backing scale (commonly 2×). `ManagedViewportInfo::Extent == {}` means "track the
window," and `Application` feeds that backing extent into the viewport's region, which sizes the
`SceneRenderer` allocation. So a 1280×720 logical window allocates 2560×1440 targets. `ManagedViewportInfo`
already documents this ("the swapchain framebuffer extent windowed — larger than the logical window on a
HiDPI display"); this plan acts on it.

## What lands

- **`f32 MaxAllocationScale = 1.0f` on `ViewportInfo` (threaded through `ManagedViewportInfo`).** The cap
  on the allocation extent **relative to the region extent**. `1.0` keeps today's behavior (allocate at
  the full region/backing extent); a value `< 1.0` caps it (e.g. `0.5` on a 2× display brings the
  allocation back to logical-point resolution). Documented as "render no larger than this fraction of the
  region's pixels — the HiDPI supersample budget."

- **The cap is applied in exactly one place: the viewport's allocation-extent derivation.**
  `Viewport::ExtentForScale`/`ScaledExtent` fold `MaxAllocationScale` into the extent so the allocation is
  `round(region · MaxAllocationScale · AllocationScale())`, clamped to ≥ 1. `Application` keeps feeding the
  **full** swapchain framebuffer extent as the managed viewport's region and only **threads the field
  through** — it does **not** pre-cap the region. One application point means no double-apply, the cap is
  available to a non-managed viewport (an editor panel) for free, and `GetOutput()` reflects the capped
  size.

- **The cap composes with, and bounds, the dynamic-resolution `MaxScale` and the Plan 02 tier.** With the
  allocation extent `= round(region · MaxAllocationScale · AllocationScale())`, `MaxAllocationScale` is the
  absolute ceiling and `AllocationScale()` (the `MaxScale`-bounded tier, Plan 02) only ever reduces below
  it; the inner-loop sub-rect rides inside that via `ViewRenderScale`. So the three scales multiply,
  `MaxAllocationScale` outermost. (A viewport with no dynamic resolution has `AllocationScale() == 1` and
  simply allocates at the capped baseline.)

- **Resize tracking honors the cap.** When the swapchain resizes, the managed viewport recomputes its
  capped allocation extent the same way, so the cap holds across window resize and display changes (a
  window dragged between a HiDPI and a non-HiDPI monitor recomputes the backing extent and re-applies the
  cap).

## Decisions

1. **A relative cap, not an absolute extent.** `MaxAllocationScale` is a fraction of the backing extent
   rather than a pinned `uvec2`, so it tracks window resize and is meaningful without the app knowing the
   display's backing scale up front. An app that wants a fixed resolution still uses
   `ManagedViewportInfo::Extent` (the existing pinned-extent path); the cap is for the *tracking* case.

2. **The cap is steady-state; it does not move.** Unlike the outer-loop tier, `MaxAllocationScale` is a
   static budget — it changes only when the app changes it or the backing extent changes. It answers
   "how big should the allocation be when the device is healthy," leaving "how much smaller under load"
   to the controller. Keeping them separate is what makes the HiDPI fix independent of the perf loop.

3. **Default `1.0` — no silent behavior change.** Existing apps allocate exactly as before until they
   opt into a cap; the sample sets a sensible cap as its migration. The default preserves the golden and
   every current test.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Viewport.h` | `f32 MaxAllocationScale = 1.0f` on `ViewportInfo` (full Doxygen) — the canonical home for the cap, so a non-managed viewport (editor panel) can use it too. |
| `engine/include/Veng/Application.h` | `f32 MaxAllocationScale = 1.0f` on `ManagedViewportInfo` (full Doxygen), threaded into the `ViewportInfo` the managed path builds. |
| `engine/src/Application.cpp` | Thread `MaxAllocationScale` into the managed viewport's `ViewportInfo` (and on resize tracking, keep feeding the full backing extent as the region). Does **not** apply the cap itself. |
| `engine/src/Renderer/Viewport.cpp` | Apply `MaxAllocationScale` in `ExtentForScale`/`ScaledExtent` — the single point where the allocation extent is `round(region · MaxAllocationScale · AllocationScale())`. |
| `examples/hello-triangle/main.cpp` | Set `MaxAllocationScale` on the managed viewport (a comment giving the local reason: avoid 2× HiDPI supersampling on this dev machine). |
| `tests/…` | A test asserting a viewport created over a backing extent with `MaxAllocationScale < 1` allocates the capped extent (its `SceneRenderer` output / `GetOutput()` is the capped size), and that resize tracking re-applies the cap. |

## Verification

- Clean build; `ctest` green. The new test passes.
- `smoke_golden` does **not** move — the smoke path runs headless at `HeadlessExtent` (no backing scale)
  and the default cap is `1.0`, so the cooked golden is unchanged. If the sample's windowed cap differs,
  it does not affect the headless capture.
- `include_hygiene` green — only a scalar field added to existing public structs.
- `validation_gate` green — a smaller allocation issues the same workload at a smaller extent, no new
  descriptor or barrier surface.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
