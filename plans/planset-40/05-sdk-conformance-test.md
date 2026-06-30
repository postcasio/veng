# Plan 05 — SDK conformance test

**Goal:** prove the SDK is real, automatically — a ctest that consumes veng **both** as a staged
install and as a build-tree export, configuring + building the out-of-tree template against each
and running its launcher smoke. The build-time analogue of the relocatable-trio test. **Depends
on Plan 04.**

## Why it is its own plan

A green in-tree build proves nothing about whether a *downstream* `find_package(veng)` consumer
works — the install/export surface (00–03) is only verified by a consumer that has no engine
source tree. This plan adds that consumer as a test, so a future change that breaks the exported
surface (a missing install, an unexported dep, a source-path leak in `veng-config`) fails ctest
rather than a user's first out-of-tree build. It is a self-contained harness plan, built on the
migrated template (Plan 04).

## What lands

- **A staged-install conformance test.** A ctest (a CMake script test, mirroring
  [`SmokeGolden.cmake`](../../cmake/SmokeGolden.cmake)) that: installs veng to a throwaway prefix
  (`cmake --install … --prefix <tmp>`), then configures `examples/template` **standalone** against
  it (`-DCMAKE_PREFIX_PATH=<tmp>`), builds it, and runs `template-launcher` under the smoke
  capture, asserting exit 0 + a correct-sized PPM. Labelled `gpu` (`SKIP_RETURN_CODE 77`) so it
  skips cleanly with no Vulkan ICD, like the other launcher smokes.

- **A build-tree conformance test.** The same template configure+build+smoke, but discovering veng
  via the **build-tree export** (`-Dveng_ROOT=<engine build dir>`), with **no install step** —
  the co-development path. The two tests share a driver script parameterized on the discovery root.

- **The installed `veng-editor` is smoke-run too.** Beyond the cook (which exercises the installed
  `vengc`) and the launcher, each conformance mode invokes the installed `veng-editor` with a no-op
  flag (`--version`) and asserts exit 0, so the editor exe's own runtime resolution (`INSTALL_RPATH`
  over `libveng_editor` / `libveng_graph` / Slang) is covered — not just the cooker's.

- **The matrix is explicit.** The two template tests assert the same standalone template builds and
  runs whether veng is found in an install prefix or a build tree (the two out-of-tree families). The
  **in-tree** mode is covered separately by `hello_triangle_launcher_smoke` — hello-triangle is the
  in-tree exemplar, the template the out-of-tree one — so the three consumption modes are covered
  without the template ever being built in-tree (it no longer is, per Plan 04).

- **Bounded cost.** The conformance configure+build cooks the **minimal** template (zero-config
  default, one trivial material, a primitive) — not the maximal hello-triangle — so the per-mode
  build is small. The nested configure reuses the already-built dependency packages on
  `CMAKE_PREFIX_PATH` rather than re-fetching, the nested build is parallelism-capped per the repo
  convention, and each script test carries a `TIMEOUT` so a hung inner build fails the test rather
  than stalling the whole `ctest` run. The tests run in the `gpu` band (they need a device for the
  smoke) and skip with no ICD.

## Decisions

1. **The exported surface is tested by a source-tree-free consumer.** Only an out-of-tree
   `find_package(veng)` build catches a broken install/export; the template-as-consumer is that
   build, run in ctest.
2. **Both discovery modes are covered.** Install-prefix and build-tree each get a test, so a
   regression in either resolution path (e.g. a source-path leak that only works in build-tree
   mode) is caught.
3. **The test drives the literal `examples/template`.** The conformance harness configures the
   real template directory — not a private fixture copy — so there is no drift between "what the
   conformance test builds" and "what a new developer copies." Keeping that app minimal (zero-config
   cook) makes the per-mode configure+build fast enough for the standard band.

## Files

| File | Change |
|---|---|
| `cmake/SdkConformance.cmake` (new) | The driver: install/export → standalone configure of `examples/template` → build → `vengc` cook (incidental) → `veng-editor --version` smoke → `template-launcher` smoke; parameterized on discovery root, parallelism-capped. |
| `CMakeLists.txt` | Register the two tests (`sdk_conformance_install`, `sdk_conformance_buildtree`), labelled `gpu`, `SKIP_RETURN_CODE 77`, each with a `TIMEOUT`. |
| `cmake/SmokeGolden.cmake` (or its shared helper) | Reuse the existing PPM-size capture assertion the launcher smokes already use, rather than duplicating it. |

## Verification

- Both new tests green on the dev Mac: each installs/exports, builds the standalone template, and
  runs the launcher smoke to exit 0 + a correct-sized capture; both skip (77) with no ICD.
- The full band stays green: clean `cmake -B build` + build; `ctest --output-on-failure`
  (including the new conformance tests + the existing `*_launcher_smoke` + `smoke_golden`);
  `ctest -L validation` on a `build-debug` green; `include_hygiene` green.
- A deliberately-broken probe makes **each** test fail, not silently pass: dropping `vengc` from the
  install fails `sdk_conformance_install`, and a source-path leak / missing alias in the build-tree
  config fails `sdk_conformance_buildtree` — confirming both resolution paths are really exercised
  (the build-tree config's live-source-path resolution is the more fragile of the two, so it gets its
  own negative control).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
</content>
