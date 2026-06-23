# Plan 05 — render-to-texture into a material, and the render-order contract

**Goal:** lock and prove RTT into another scene's material — a monitor, a mirror, a portal. An
`Offscreen` viewport's output is sampled inside a second viewport's scene through
`Material::SetTextureHandle(name, producer.GetOutputHandle())` (the API is keyed by the material's
texture-field **name**, a `std::string_view`, not a numeric slot). The machinery exists
(`GetOutputHandle` from Plan 00, `Material::SetTextureHandle` already shipped, the drive-list's
registration-order render from Plan 02); this plan establishes the **ordering and synchronization
contract** that makes it correct and covers it with a viewport-feeds-viewport gpu test.

**Depends on Plan 02 (the drive-list + registration-order render).**

## Why it is its own plan

RTT-into-ImGui is delivered by Plans 00–03 (the editor). RTT-into-a-material is a distinct
guarantee: a *producer* viewport must render before a *consumer* viewport that samples it, **within
the same frame**, and the single-copy output contract must hold across that handoff with no ring or
semaphore. That guarantee is cheap to state and easy to break silently, so it earns a test that
would fail if the order or the barrier regressed — rather than being an undocumented side effect.

## What lands

- **The render-order contract, documented.** A consumer viewport sampling a producer viewport's
  `GetOutputHandle()` must be **registered after** the producer (registration order is render order,
  Plan 02). Within a frame the producer's `Render` ends with its output in `Sample` layout (the
  `PrepareForAccess(Sample)` the `Viewport` issues), the consumer samples that bindless handle, and
  the producer's *next-frame* `Execute` transitions the output back to `ColorAttachment` — exactly
  the frames-in-flight handoff `SceneRenderer` already documents, with the consumer as one more
  same-frame reader.

- **No ring, no semaphore — and the reason, stated.** Both halves record on the single graphics
  queue in submission order, so the producer's `Sample` transition reaches the consumer's read
  without a cross-queue semaphore and the output stays single-copy. Ringing a viewport output (or a
  semaphore) is reserved for an **async or off-queue** consumer — the boundary
  [area 8](../future/README.md#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries)
  draws for TAA history and cross-queue handoffs. A same-frame material sample is neither.

- **The gpu test: viewport-feeds-viewport.** Build two scenes and two viewports — a producer
  (`Offscreen`) rendering scene A, and a consumer rendering scene B whose material binds the
  producer's `GetOutputHandle()`; register the producer first, drive one engine frame, and assert
  the consumer's output reflects the producer's content (a distinguishing color/region) — proving
  the producer rendered first and its output was sampleable when the consumer ran. Labelled `gpu`,
  run under the validation gate so a missing/wrong barrier across the handoff surfaces there.

## Decisions

1. **Registration order is the contract, not a hidden detail.** The user-facing rule is one line —
   "register producers before consumers" — backed by the test. A declared-dependency topo-sort stays
   a future refinement; until an RTT graph is deep enough to make manual ordering awkward,
   registration order is sufficient and obvious. The one sharp edge is **dynamic** registration: an
   editor that closes and reopens a producer document re-registers it *after* its consumer, silently
   breaking the handoff (the consumer samples a stale or black texture, no validation error). For
   app-driven/static registration the rule is safe; dynamic re-registration is exactly the case the
   declared-dependency future work resolves — noted so a future editor RTT panel does not trip on it.

2. **Prove it with a test, not a sample asset.** A gpu test keeps the planset asset-free (no
   `.vmat`, no minted `AssetId`); a runtime monitor/mirror **sample feature** is the natural demo on
   top, named as a follow-on rather than built here.

3. **The single-copy contract is reused, not extended.** The handoff fits the existing
   `SceneRenderer` output contract exactly; this plan adds documentation and a guard, not a new
   synchronization mechanism.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/Viewport.h` | Document the render-order + single-copy contract on `GetOutputHandle` (`@warning` + `@see` the frames-in-flight note). |
| `tests/gpu/…` *(new)* | The viewport-feeds-viewport test: producer→consumer registration, one frame, content assertion. |
| `tests/CMakeLists.txt` | Register the new gpu test (label `gpu`, `SKIP_RETURN_CODE 77`, validation band). |

## Verification

- Clean build; `ctest` green. The new gpu test passes and skips cleanly with no ICD.
- `validation_gate` green under `build-debug` running the test — the cross-viewport handoff raises no
  barrier/layout validation error, confirming the single-copy contract holds same-frame without a
  ring.
- `smoke_golden` / `hello_triangle_launcher_smoke` unaffected.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
