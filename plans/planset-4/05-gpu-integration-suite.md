# Plan 05 — In-process multi-case GPU integration suite (area 5b)

**Goal:** the testing work planset-3 deferred — a single `veng_gpu` executable
running **many** GPU cases in one process, each standing a `Context` **up and down
per case** for real isolation. This is **future area 5b**, and it is written here,
*after* de-global, because per-case device lifecycle is impossible against a
singleton and the suite targets the explicit-device API (plans 02–04) **once**
rather than being rewritten when the global goes.

## Why this couldn't exist before planset-4

planset-3 plan 06 deliberately kept the GPU band **one executable per test**: each
test process got a fresh `Context` singleton, which was the only isolation a global
allowed. A many-cases-in-one-process framework needs each case to construct and
destroy its own `Context` — which only works now that `Context::Create`-time and
destruction are device-explicit. The one-exe band (`headless_smoke`,
`compute_dispatch`) **stays** as-is for the cases that genuinely want process
isolation (e.g. death-adjacent or full-app smoke); 5b is the *additional*
in-process band for fine-grained resource cases.

## Design: the per-case fixture

1. **A doctest fixture that owns a `Context` per case.** Build on planset-3 plan
   06's `tests/support/` GPU bring-up helper, but invert it: instead of one shared
   `Context context; Initialize(...); … Dispose();` per executable, a fixture
   constructs a headless `Context` in setup and tears it down in teardown, so each
   `TEST_CASE` (or `SUBCASE`) gets a clean device:

   ```cpp
   struct GpuFixture {
       Renderer::Context context;
       GpuFixture()  { context.Initialize({ .ApplicationName = "veng_gpu",
                                            .InternalRenderExtent = {64, 64} }, nullptr); }
       ~GpuFixture() { context.WaitIdle(); context.DisposeResources(); context.Dispose(); }
   };
   ```

   Every case now passes `context` into the explicit `Create` factories (plans
   02/03) — the suite *is* a consumer of the de-globalized API, which is the point:
   it exercises the new surface end to end.

2. **Skip, don't fail, with no ICD.** Reuse plan 01's `RequiresVulkan()` from
   planset-3 — the whole `veng_gpu` band reports *skipped* on a box with no Vulkan
   implementation. Same contract as the one-exe band: `ctest -L gpu` is safe on any
   machine.

3. **One executable, label `gpu`.** Register `veng_gpu` with `add_test` and label
   it `gpu` (alongside the one-exe tests). Backend-header linkage as in the
   planset-3 type-mapping/barrier cases (it touches `Backend/` for some asserts).

## Cases (migrate + extend)

Port the deterministic resource cases from planset-3 plan 06's one-exe exercises
into in-process fixture cases (where per-process isolation wasn't the point), and
add the ones the singleton blocked:

- **Buffer round-trip** — `Create(context, …)` → `Upload(bytes)` → `Download()`
  byte-identical; offset upload lands correctly.
- **Typed-buffer round-trip** — `VertexBuffer<V>` / `StorageBuffer<T>` upload →
  download. Still the **primary coverage of the typed-buffer size arithmetic**
  (planset-3 left it device-tested, not extracted).
- **Image clear + download** at a non-trivial extent and a second format.
- **Descriptor write paths** — the `DescriptorSet::Write` overloads
  (sampled image+sampler, storage image, uniform/storage buffer) without a full
  render. The known storage-image `UPDATE_AFTER_BIND` gap (CLAUDE.md) surfaces
  here — **note/xfail it, do not fix it** (it's
  [bindless](../future/bindless-descriptors.md) work, out of scope per the README).
- **Per-case isolation proof** — two cases each create/destroy resources on their
  *own* `Context`; the second case starting clean (no leaked handles, no carried
  state) is the thing 5b adds over the one-exe band. A second `Context` in one
  process running cleanly is also the first real exercise of de-global's
  multi-context capability (without introducing concurrency — still single
  thread).

## Dependencies

Needs plan 04 (the explicit-device API + deleted singleton — a per-case `Context`
is only isolated once the global is gone). Builds on planset-3 plan 06's
`tests/support/` helpers and plan 01's `RequiresVulkan()` skip + `gpu` label.

## Acceptance

- `ctest -L gpu` passes where an ICD exists; reports **skipped** (not failed) where
  none does; the in-process `veng_gpu` cases are isolated (a later case is
  unaffected by an earlier one's resources).
- Verified under the `VE_DEBUG` validation build by running `build-debug/veng_gpu`
  directly and grepping stderr for validation ERRORs (green `ctest` is not proof —
  CLAUDE.md). No new gaps beyond the documented storage-image one.
- The existing one-exe GPU band (`headless_smoke`, `compute_dispatch`) is unchanged
  — 5b is additive, not a replacement.

## Notes

- Keep every case **deterministic** (fixed bytes/pixels, no wall-clock dependence —
  unlike the hello-triangle smoke PPM) so values can be asserted.
- Per-case `Context` up/down is *slower* than sharing one — that's the deliberate
  trade for isolation. If bring-up cost ever bites, a `SUBCASE`-per-resource within
  one fixture `Context` is the middle ground; default to per-case isolation and
  only relax if measured.
- This closes area 5b. Anything it surfaces about descriptor management feeds the
  bindless/descriptor rework in `future/`, recorded not fixed.
