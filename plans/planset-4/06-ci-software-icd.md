# Plan 06 — CI with a software Vulkan ICD + validation gate

**Goal:** stand up the hosted pipeline planset-3 deliberately omitted — run the
full `ctest` suite on every push against a **software Vulkan ICD** (lavapipe), with
a **validation-error gate** that promotes Vulkan validation ERRORs to test
failures. Closes the last item in future area 5: the GPU/headless suite stops being
local-dev-only, and "validation errors don't fail tests" stops being true in CI.

## Why this is last

planset-3 was explicitly local-dev-only (no CI, no ICD provisioning, no validation
gate). The GPU band only ever ran on a developer's machine, and a validation
regression could land silently because the debug messenger only logs (CLAUDE.md).
This plan needs a *suite worth running* (plans 01–05) and the explicit-device API
(so per-case fixtures run head-less in CI), which is why it sits at the end.

## Design

1. **Software ICD: lavapipe (Mesa `llvmpipe`).** A CPU Vulkan implementation, the
   standard choice for headless CI. SwiftShader is the fallback if lavapipe's
   feature/extension coverage proves short for veng's needs (e.g. a format or a
   descriptor-indexing feature the suite touches). The suite already **skips**
   gracefully where no ICD is present (plans 01/05), so the only CI-specific work
   is *installing* an ICD and pointing `VK_ICD_FILENAMES` at it.

2. **Pipeline (GitHub Actions, `.github/workflows/ci.yml`).** Linux runner
   (`ubuntu-latest`):
   - Install build deps (Vulkan SDK/loader, GLFW, glm, zlib, `glslc`) and Mesa
     lavapipe (`mesa-vulkan-drivers`), export `VK_ICD_FILENAMES` /
     `VK_DRIVER_FILES` to the lavapipe manifest.
   - Configure + build **both** the default (`build/`, validation OFF) and the
     `VE_DEBUG` (`build-debug/`, validation ON) trees — the validation gate needs
     the layers enabled.
   - `ctest --output-on-failure` on the default build (correctness), then the
     validation job below on the debug build.
   - macOS/MoltenVK CI is **out of scope** here (hosted macOS + MoltenVK headless
     is flaky and costly); document it as a possible later runner. veng's primary
     dev platform stays local macOS — CI is the portability/regression net.

3. **The validation gate.** Because the debug messenger only `Log::Error`s on
   validation errors and never aborts (CLAUDE.md), green `ctest` under `VE_DEBUG`
   is *not* a clean run. Add a gate that fails CI when validation ERRORs appear:
   - Run the `gpu`-labelled binaries (`headless_smoke`, `compute_dispatch`,
     `veng_gpu`) from `build-debug/` capturing stderr, and **fail if
     `Vulkan validation` ERROR lines appear** — except an allowlist for the
     documented, pinned gaps (the storage-image `UPDATE_AFTER_BIND` gap from
     CLAUDE.md, and the benign MoltenVK "buffer robustness" warning — the latter
     won't appear under lavapipe but keep the allowlist explicit).
   - Implement the gate as a small CTest test or a CI step that greps captured
     output — prefer a CTest fixture so it runs locally too (`ctest -L validation`),
     not just in CI. A `VENG_VALIDATION_FATAL`-style build flag that promotes the
     messenger callback to a non-zero exit is an alternative; the grep gate is
     lower-risk (no engine behaviour change) and is the recommended first cut.

4. **Status / badge.** Add a CI badge to the top-level README once green.

## Dependencies

Needs plan 05 (the full suite, incl. `veng_gpu`, to run under the ICD). The ICD +
validation gate are what make the planset-3/05 GPU bands meaningful in CI.

## Acceptance

- CI runs on push/PR: configures, builds (default + `VE_DEBUG`), runs `ctest`
  against lavapipe — pure-logic/round-trip/death bands always run; the `gpu` band
  runs (not skipped) because the ICD is present.
- The **validation gate fails the build** on a *new* `Vulkan validation` ERROR, and
  passes on the known-and-allowlisted storage-image gap — i.e. a validation
  regression can no longer land silently.
- A deliberately-introduced validation error (temporary local test) is caught by
  the gate — verify the gate actually bites before trusting it.
- The suite still skips (not fails) the `gpu` band on a no-ICD machine locally, so
  the CI gate doesn't break local dev on a box without lavapipe.

## Notes

- The allowlist is the **documented** gaps only — it is not a place to silence new
  errors. When the bindless/descriptor rework closes the storage-image gap, remove
  it from the allowlist so the gate tightens automatically.
- Keep the workflow minimal and pinned (action versions, Mesa package) — this is
  infrastructure, and a flaky/auto-updating CI is worse than none.
- This is the **final plan of planset-4**. On completion, update
  [future/README.md](../future/README.md): area 3 struck, area 5 fully done (5a =
  planset-3, 5b + CI = planset-4), ordering diagram re-cut to `2 threading →
  1 asset system` (+ independent area 4), and the [plans/README.md](../README.md)
  index gets the planset-4 row.
