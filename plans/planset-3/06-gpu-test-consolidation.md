# Plan 06 — Consolidate & extend GPU tests

**Goal:** bring the existing GPU tests under a coherent harness, add a few focused
GPU exercises for resource paths the smoke tests don't cover, and make the whole
GPU band skip gracefully where no Vulkan ICD exists — all in the **one-exe-per-test**
style that already gives each test a fresh `Context` singleton per process.

## Scope boundary (important)

This is **not** the in-process multi-case GPU integration suite — that is area 5b,
deferred until after de-globalize (see the planset README). Everything here stays
one executable per test so per-process isolation holds while `Context` is still a
singleton. Resist the urge to fold these into `veng_unit`.

## Dependencies

Needs plan 01 (the `RequiresVulkan()` skip helper + `gpu` label). Independent of
02/03/04/05 — a leaf. Touches `tests/` and `CMakeLists.txt` test section only.

## Work

1. **Adopt the skip helper.** `headless_smoke` and `compute_dispatch` currently
   abort/fail if no ICD is present (noted in their headers). Route them through
   plan 01's `RequiresVulkan()` so they report *skipped* on a machine with no
   Vulkan implementation, instead of a hard failure. Label both `gpu`.

2. **Shared GPU test support.** Factor the duplicated bring-up/teardown
   (`Context context; context.Initialize({...}, nullptr); … WaitIdle();
   DisposeResources(); Dispose();`) and the pixel-compare loop (identical in both
   existing tests) into `tests/support/`. Keep it thin — a fixture-ish helper, not
   a framework.

3. **New focused GPU exercises** (each its own executable + `add_test`, label
   `gpu`), covering resource paths the two smoke tests don't:
   - **Buffer round-trip** — `Buffer::Create` → `Upload(bytes)` → `Download()`
     returns the same bytes; offset upload lands at the right place.
   - **Typed-buffer round-trip** — `VertexBuffer<V>` / `StorageBuffer<T>` upload →
     download. This is the **primary coverage of the typed-buffer size
     arithmetic** (plan 02 deliberately leaves that math untested device-free
     rather than extracting a helper): a wrong `count * sizeof(T)` produces a
     wrong-sized allocation or a download mismatch and fails here.
   - **Image clear + download** at a non-trivial extent / a second format
     (broadens `headless_smoke`'s single 4×4 RGBA8 case).
   - **Descriptor write paths** — exercise the `DescriptorSet::Write` overloads
     (sampled image+sampler, storage image, uniform/storage buffer) without a
     full render, asserting they don't trip validation under `VE_DEBUG`. (This is
     where the known storage-image descriptor gap from CLAUDE.md will show — note
     it / xfail it rather than papering over.)

## Acceptance

- `ctest -L gpu` passes where an ICD exists; reports skipped (not failed) where
  none does.
- New tests verified under the `VE_DEBUG` validation build by running the binaries
  directly and grepping stderr for validation errors (per CLAUDE.md — green ctest
  is not proof of validation-clean).
- Existing `headless_smoke` / `compute_dispatch` behaviour unchanged besides the
  skip-instead-of-fail path.

## Notes

- Keep the bytes/pixels deterministic (no wall-clock dependence — unlike the
  hello-triangle smoke PPM) so these *can* be value-asserted.
- Any new descriptor validation gap discovered here feeds the bindless/descriptor
  rework in `plans/future/` — record it, don't fix it here.
