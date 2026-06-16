# Plan 01 — the overlap oracle + drop the false guard

**Goal:** remove planset-12's incorrect `Create`-time `== 1` assert (it aborts at the
engine's real frames-in-flight count, and blocks any multi-frame test), then build the
standing oracle this planset turns on: a GPU test that drives **more frames than
`MaxFramesInFlight`**, reads back an earlier frame's output while a later frame is in
flight, and asserts the readback is uncorrupted — run under **synchronization
validation** — so we *measure* whether the single-copy handed-out output actually races
across frames, rather than fix on the strength of reasoning. Any single frame's pixels are
identical; `smoke_golden` is unchanged.

## Why this comes first, and on the main thread

The whole planset is "measure first, fix minimally." This plan is the measurement. Its
verdict decides plan 02's fix (a one-call barrier, or nothing but the documented contract).
Two things make it the gate: the assert must go before a multi-frame test can even
construct the renderer, and the oracle's *framing* — what "uncorrupted across overlap"
asserts and how the sync-validation hazard is caught — is the substantive judgement. The
mechanical drive loop + readback comparison is good `model: sonnet` delegation once the
assert shape is fixed.

## Drop the `== 1` guard

Delete planset-12's `Create`-time
`VE_ASSERT(context.GetMaxFramesInFlight() == 1, …)`. It is not a future tripwire — the
engine runs `MaxFramesInFlight = 2`, so the assert fires on the first `Create` and aborts
hello-triangle at launch. No replacement assert; the contract it tried to encode is stated
as a present-tense comment next to the output handoff in plan 02 (once the fix it refers to
exists). Removing it is the prerequisite that lets the renderer run — and the overlap test
construct it — at FIF=2 at all.

## The overlap oracle (`veng_gpu`)

The hazard manifests only when frame N's consumer reads the output while frame N+1 writes
it. To force that window deterministically and detect corruption:

- Construct one `SceneRenderer` (fixed extent — no `Resize`/`Configure` in the loop, so
  `GetOutput()` is a stable single-copy view across the run, the regime under test) and
  render a sequence of **K > `MaxFramesInFlight`** frames where each frame's rendered content
  is **distinguishable by frame index** — e.g. animate the camera or a clear value so frame i
  has a known signature at a sampled texel.
- Each frame: `Execute` the renderer, then **capture that frame's output** the way a consumer
  would — record a `CopyImageToBuffer` (preceded by the normal `Sample`/transfer transition)
  **into that frame's own command buffer**, targeting a **ring of K readback buffers** (one
  per driven frame, or `MaxFramesInFlight` reused after the owning frame's fence). **Do not**
  use `Image::Download`/`SubmitImmediateCommands` — they `WaitIdle`, which fully serializes
  the stream and would mask the very hazard under test. No `WaitIdle` runs between frames; the
  engine's `MaxFramesInFlight` pipelining keeps multiple frames live, and the host reads each
  readback buffer only after a final drain.
- After draining, assert **each captured frame's readback matches that frame's signature**.
  A single-copy output with an unbridged cross-graph dependency would show frame i's capture
  polluted by frame i+1's write (a torn or wrong-signature texel); a correctly serialized
  output yields each frame its own signature.

This is the functional half of the verdict. It runs **with no fix applied** — single-copy
output, no `PrepareForAccess(ColorAttachment)` before `Execute`, only the scene graph's own
derived acquire on the imported output — so its result is the honest measurement of the
current state. The per-frame readback's `Sample`/transfer transition *is* the consumer read
whose hazard against the next frame's write is under test; the test must not pre-emptively
add the producer-side barrier it exists to evaluate.

## The synchronization-validation half

Functional correctness can pass by luck (MoltenVK may serialize incidentally). The
authoritative signal is the validation layer's **synchronization validation**, which flags
hazards regardless of whether they corrupted *this* run. Because both the readback's consumer
read and the next frame's producer write are submitted to the **single graphics queue**, the
hazard (and, in plan 02, its barrier resolution) is a same-queue cross-submit case
synchronization validation tracks across submission order — not a cross-queue case it could
miss. The engine **already enables it** (`eSynchronizationValidation` in `Context.cpp`'s
validation-feature list, behind `VE_ENABLE_VALIDATION_LAYERS`), and sync hazards flow through
the debug messenger as validation errors:

- Run the overlap test under the validation gate and record whether a **`SYNC-HAZARD-*`**
  error is reported on the **output** image across frames (the measurement plan 02 acts on),
  and confirm the internal **g-buffer/depth/HDR** targets are hazard-free across frames (the
  layout-tracking argument — each lives in the renderer's own graph, which serializes its
  reuse — *verified*, not argued).
- Wire this as an extension of the existing validation gate (`cmake/ValidationGate.cmake` +
  the `validation` label) so the `gpu` band's overlap run fails on any unallowlisted sync
  hazard. Because the headless smoke reads its capture through a `WaitIdle` `Download` and has
  no cross-graph consumer, this overlap test is what first makes the sync-validation gate
  meaningful for the cross-graph output handoff under frame overlap at all.

## The verdict this plan produces

- **If the output races** (functional corruption and/or a `SYNC-HAZARD` on it): plan 02
  applies the `PrepareForAccess(output, ColorAttachment)` reuse barrier.
- **If the output is already clean** (the compiled graph serializes it via the tracked
  state `PrepareForAccess` maintains): plan 02 lands the explicit barrier as the documented
  contract anyway and keeps this test as the regression gate.

Either way the internal targets are confirmed single-copy-safe and **no ring** is built.

## Tests

- **GPU (`veng_gpu`):** the multi-frame overlap correctness test (per-frame readback matches
  per-frame signature). Labelled `gpu`, `SKIP_RETURN_CODE 77`, skips cleanly with no ICD.
- **Synchronization-validation gate:** the overlap run reports the output's cross-frame
  hazard status (the measurement) and confirms the internal targets clean; the standard
  error/warning gate stays green, allowlist empty.
- **`smoke_golden`:** unchanged — no pixel change. The renderer now *runs* at FIF=2 (the
  assert is gone) and renders identically per frame.
- **`include_hygiene`:** unaffected (no header change).

## Acceptance

Clean build; the `== 1` assert is removed and hello-triangle constructs + runs its
`SceneRenderer` at `MaxFramesInFlight = 2`; the multi-frame overlap GPU test exists, drives
> FIF frames, reads back in-flight earlier frames, and **records the output's cross-frame
hazard status** under synchronization validation (functional + sync-validation); the
internal g-buffer/depth/HDR targets are confirmed hazard-free across frames; `ctest` green;
`smoke_golden` unchanged; smoke PPM correct size + exit 0; standard validation gate green,
allowlist empty. Commit: `Plan 01: drop the false single-in-flight guard, add the
multi-frame overlap oracle under synchronization validation`.
