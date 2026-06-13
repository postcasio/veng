# Plan 06 — local validation-error gate

**Goal:** make Vulkan validation ERRORs *fail tests* locally. Today the debug
messenger only `Log::Error`s on validation errors and never aborts (CLAUDE.md), so
a green `ctest` under `VE_DEBUG` is **not** proof of a validation-clean run and a
validation regression can land silently. This plan adds a gate that turns a new
validation ERROR into a test failure on a developer's machine. Closes the last
loose end of future area 5 that matters without CI: "validation errors don't fail
tests" stops being true.

> **Scope note.** This plan was descoped from CI. veng has no hosted pipeline and
> none is planned — the software-ICD / GitHub Actions / badge work is dropped. The
> validation gate is kept because it is a *local* testing improvement, not CI
> infrastructure: it runs under `ctest` on the dev box.

## Why this is last

The gate needs a suite worth gating (plans 01–05) and the explicit-device API so
per-case fixtures run head-less, which is why it sits at the end. It builds
directly on plan 05's `veng_gpu` and the planset-3 `gpu`-labelled binaries.

## Design

1. **Capture validation output.** The `gpu`-labelled binaries
   (`headless_smoke`, `compute_dispatch`, `veng_gpu`) already emit
   `Vulkan validation` ERROR lines via the debug messenger when built under
   `VE_DEBUG`. The gate runs these from `build-debug/` capturing stderr and
   **fails if `Vulkan validation` ERROR lines appear** — except an allowlist for
   the documented, pinned gaps.

2. **Implement as a CTest fixture, not a CI step.** Prefer a small CTest test (or
   a CMake-registered script) so it runs with the rest of the suite locally —
   `ctest -L validation` against the `build-debug/` tree. A
   `VENG_VALIDATION_FATAL`-style build flag that promotes the messenger callback
   to a non-zero exit is an alternative; the grep gate is lower-risk (no engine
   behaviour change) and is the recommended first cut.

3. **The allowlist is the documented gaps only.**
   - The storage-image `UPDATE_AFTER_BIND` gap from CLAUDE.md (the
     `compute_dispatch` storage-image descriptor path: `DescriptorSetLayout` sets
     `UPDATE_AFTER_BIND` without `descriptorBindingStorageImageUpdateAfterBind`,
     and the descriptor pool has no `STORAGE_IMAGE` pool size).
   - The benign MoltenVK "buffer robustness" warning.

   It is **not** a place to silence new errors. When the bindless/descriptor
   rework closes the storage-image gap, remove it from the allowlist so the gate
   tightens automatically.

## Dependencies

Needs plan 05 (the full suite, incl. `veng_gpu`) so there is a suite to gate.

## Acceptance

- `ctest -L validation` (against `build-debug/`) **fails on a new
  `Vulkan validation` ERROR** and **passes on the known-and-allowlisted
  storage-image gap** — a validation regression can no longer land silently on
  the dev box.
- A deliberately-introduced validation error (temporary local test) is caught by
  the gate — verify the gate actually bites before trusting it.
- The gate runs cleanly against the default `build/` tree too (no validation
  layers → no ERROR lines → trivially green), and does not require any network or
  hosted runner.

## Notes

- Keep the gate self-contained — no extra dependencies, no ICD provisioning, no
  external services. It is a grep over output a developer already produces.
- This is the **final plan of planset-4**. On completion, update
  [future/README.md](../future/README.md): area 3 struck, area 5 done (5a =
  planset-3, 5b + validation gate = planset-4), ordering diagram re-cut to
  `2 threading → 1 asset system` (+ independent area 4), and the
  [plans/README.md](../README.md) index gets the planset-4 row.
