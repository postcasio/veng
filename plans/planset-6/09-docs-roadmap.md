# Plan 09 — docs + roadmap re-cut + `Veng.h` contract

**Goal:** the roadmap-only closing plan. Revise the `Veng.h` single-threaded
contract note to match the delivered reality, update the ownership/`CLAUDE.md`
docs for the new primitive and async-default upload/load, and re-cut the future
roadmap so area 2 is taken up and area 1 is fully complete.

## Why this is its own plan

Consistent with every prior planset (planset-4/06-adjacent, planset-5/11): docs and
roadmap land as a final, code-free commit so the working planset README, the
contract notes, and the future vision are re-cut once, coherently, after the code is
proven — not drifting plan-by-plan.

## Work

1. **`Veng.h` — the single-threaded contract note.** Revise the v1 contract: work
   may now run off the main thread **through the `TaskSystem`** (decode + upload);
   `Context::BeginFrame`/`EndFrame`, draw recording, ImGui, input, and `Time` remain
   main-thread-only, and **direct concurrent veng API calls remain illegal**. State
   it as present-tense fact (CLAUDE.md comment policy — no "used to be", no plan
   citations).

2. **`docs/ownership.md`** — add `TimelineSemaphore` to the `Unique` single-owner
   list beside `Semaphore`/`Fence`. Document the **transfer-keyed retire path**
   (`RetireOnTransfer`, plan 05): worker-created upload scratch retires against the
   transfer timeline value, not the frame fence, and the retire path is now
   mutex-guarded for off-thread drops.

3. **`CLAUDE.md`** — update:
   - the single-threaded paragraph (mirror the `Veng.h` revision: off-thread work
     via the task system; concurrent API calls still illegal);
   - the upload/load surface: `Upload` / `Load` are **async by default**,
     `UploadSync` / `LoadSync` block;
   - a short `TaskSystem` line in the Application/ownership section (owned by
     `Application`, threaded explicitly, pumped once per frame).

4. **`plans/README.md`** — add the planset-6 entry (threading / task system; async
   loads; closes future area 2 and area 1's async half).

5. **`future/README.md`** — mark **area 2 taken up by planset-6** and **area 1
   complete** (async `Load` landed). Re-cut the ordering diagram: the asset+threading
   chain is done; **area 4 (events/input)** is the only remaining area, independent
   and gameplay-driven. Note **hot-reload** remains future (its re-cook half
   conflicts with offline-only cooking — needs a dev-only watcher design).

6. **`future/threading-task-system.md`** — trim to whatever enduring vision remains
   after delivery (a task *graph* as the natural follow-on; staging-buffer pooling;
   cancellation), banner the delivered parts as shipped in planset-6.

7. **`future/asset-system.md`** — mark the async half delivered; trim to the
   enduring vision (hot-reload's real design, the editor as the demanding second
   consumer, dependency-graph-driven eviction refinements).

## Dependencies

All of plans 01–08 (it documents the delivered reality).

## Acceptance

- No code change; build + `ctest` unaffected (docs-only).
- The `Veng.h` and `CLAUDE.md` contract notes match the shipped behaviour and obey
  the comment policy (present-tense fact, no plan/history citations).
- The future roadmap reflects area 2 done and area 4 as the lone remaining area.

## Notes

- Roadmap-only commit: `planset-6: <summary>` with a `Co-Authored-By` trailer.
- Watch the comment policy in `Veng.h` especially — the contract note must read as
  a present-tense fact about what veng *is*, with no "v1 used to be" narrative.
</content>
