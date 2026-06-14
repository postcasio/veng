# Plan 01 — `Upload` → `UploadSync` rename

**Goal:** rename the blocking `Buffer::Upload` / `Image::Upload` to `UploadSync`
and migrate every caller in the same pass, **freeing the unmarked `Upload` name**
for the async default that plan 07 adds. Behaviour-preserving: byte-for-byte the
same Vulkan calls, only the spelling changes. After this plan the default name is
unclaimed and the blocking path wears the verbose name it will keep forever.

## Why this is its own plan

Doing the rename *first*, alone, separates a trivially-verifiable mechanical sweep
from the genuinely risky async work (plan 07). Every current upload caller keeps
working unchanged under the new name, the build stays green, and plan 07 starts on
a clean slate where `Upload` means "the non-stalling one" by construction — the
obvious method is the right one. The same staged-naming discipline planset-5 used
for `LoadSync` (ship the blocking name first so the default never has to be renamed
out from under callers).

## What changes

- **`engine/include/Veng/Renderer/Buffer.h`** — `Upload(span, offset)` →
  `UploadSync(span, offset)`; impl in `engine/src/Renderer/Backend/Buffer.cpp`.
- **`engine/include/Veng/Renderer/Image.h`** — `Upload(span)` → `UploadSync(span)`;
  impl in `engine/src/Renderer/Backend/Image.cpp`. Note `Image::UploadSync`'s own
  body fills a staging `Buffer` — that internal call becomes `staging->UploadSync`.
- **Callers (grep `\.Upload(` / `->Upload(` across the tree):**
  - The `AssetManager` loaders (`engine/src/Asset/…` — texture/mesh upload paths)
    that call `Image::Upload` / `Buffer::Upload`.
  - `examples/hello-triangle` (any direct upload).
  - `tests/` — the `gpu`/`unit` buffer/image roundtrip cases.

## Work

1. Rename the two member functions and their definitions to `UploadSync`.
2. Sweep every caller to the new name. This is a pure find-and-replace of the
   call sites — no signature change beyond the name, no behaviour change.
3. Leave `SubmitImmediateCommands` / `WaitIdle` exactly as they are — `UploadSync`
   still blocks; that is its job. Plan 07 adds the non-blocking sibling.

## Dependencies

None — foundation of the threading arc; must land before plan 07 (which defines
`Upload` as async and would otherwise collide with the existing name).

## Acceptance

- Clean build, `ctest` green, smoke binary writes a correct-sized PPM
  (1280×720 RGB ≈ 2,764,816 bytes).
- `grep -rn "\.Upload\b\|->Upload\b" engine examples tests` finds **no** call to a
  bare `Upload` — every upload is now `UploadSync` (the async `Upload` does not
  exist yet).
- No diff in rendered behaviour: same uploads, same queue, same `WaitIdle`.

## Notes

- **Behaviour-preserving**, like planset-4's de-global sweep: any latent bug spotted
  while renaming is noted separately, not fixed under this plan.
- Good `model: sonnet` delegation — it is mechanical — but it edits the sample and
  tests, so land it before anything depends on the freed name.
</content>
