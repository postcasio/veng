# Plan 04 — make `examples/template` a standalone `find_package(veng)` consumer

**Goal:** make `examples/template` the **out-of-tree exemplar** — a standalone CMake project that
consumes veng via `find_package` (build-tree or install) and exercises `veng_add_project` /
`veng_add_game` / `veng_add_editor` — and **remove it from the engine tree's build** so the engine
no longer configures it. `examples/hello-triangle` stays the **in-tree** exemplar. The two are the
dual-mode conformance matrix the request asked for. **Depends on Plan 03.**

## Why it is its own plan

The whole planset exists so a game can live outside the engine tree; the template is the proof it
works for the *minimal* app — the smallest correct thing a new developer copies. Standing it up as a
real out-of-tree consumer is the first end-to-end exercise of the entire SDK surface (00–03) and is
the fixture the conformance test (Plan 05) drives. It is a build-system migration of one example,
cleanly separable from the test harness around it.

## The misconception this corrects

Treating the template as *both* a standalone `find_package` consumer *and* an `add_subdirectory` of
the engine is incoherent: a nested `project()` + `find_package(veng REQUIRED)` runs while
`add_subdirectory(examples)` is being configured (root `CMakeLists.txt` line ~892), **before** the
engine's `install`/`export(EXPORT)` block (line ~1035) has produced any `veng-config.cmake` to find.
There is no point in the engine's own configure at which the template could `find_package` the engine
that is mid-configure. So the template stops being built by the engine tree entirely:

- **`examples/CMakeLists.txt` drops `add_subdirectory(template)`** — only `hello-triangle` stays
  in-tree. The template is no longer configured or built by a plain `cmake --build build`.
- **The template's only builder is the conformance test (Plan 05)**, which configures it standalone
  against an install prefix or the build-tree export. That is exactly how a real new developer
  consumes it — zero in-tree special-casing, which is the entire point of the planset.

## What lands

- **`examples/template` becomes a standalone project.** Its
  [`CMakeLists.txt`](../../examples/template/CMakeLists.txt) gains its own `cmake_minimum_required`
  + `project(template …)` and a leading `find_package(veng REQUIRED)`, consuming veng as a package.
  The `veng_add_project` / `veng_add_game` / `veng_add_editor` calls are **otherwise unchanged** —
  that they survive verbatim is the demonstration that the SDK surface matches the in-tree one. The
  core-pack `REFERENCE` reads `${veng_CORE_PACK_JSON}` (the mode-resolved consumer-facing variable)
  instead of the hardcoded `${CMAKE_SOURCE_DIR}/engine/assets/core/...`.

- **The engine tree stops building the template.**
  [`examples/CMakeLists.txt`](../../examples/CMakeLists.txt) becomes just
  `add_subdirectory(hello-triangle)`. The template directory remains physically under `examples/` as
  the copyable starter source; it is simply not part of the engine build graph anymore.

- **The coverage moves to the conformance test.** Because the template is no longer compiled by the
  default build, a breaking engine change that misses it is caught by the **SDK conformance test**
  (Plan 05, `gpu` band), not by a plain `cmake --build build`. hello-triangle remains the in-tree
  sample covered by the default build + `smoke_golden` + `hello_triangle_launcher_smoke`. The
  co-migration norm in the root `CLAUDE.md` is updated (Plan 06) to state this split — the template's
  standing check is the conformance test.

- **`hello-triangle` stays in-tree, unchanged.** It remains the maximal sample built as part of the
  engine tree (`add_subdirectory(hello-triangle)`), the in-tree half of the matrix. No migration —
  its job is to prove the in-tree path still works.

- **A template `README` notes the consumption model.** The minimal starter documents that it is
  copied *out of the engine tree* and consumes veng via `find_package`, pointing at the co-dev
  override (Plan 03) for developing against a local engine checkout.

## Decisions

1. **The template is the out-of-tree exemplar; hello-triangle is the in-tree one.** The two
   co-migrated examples now also exercise the **two consumption families** — the standing check that
   both paths keep working on every breaking change.
2. **The authoring calls survive the migration verbatim.** The template's
   `veng_add_project` / `veng_add_game` / `veng_add_editor` bodies are identical to the in-tree
   spelling; only the project preamble (`cmake_minimum_required` + `project` + `find_package`)
   differs. This is the visible proof the SDK surface is real.
3. **The template leaves the engine build graph.** It is no longer `add_subdirectory`'d; its build
   and verification live entirely in the conformance test, which removes the configure-order
   circularity and makes the template a genuine out-of-tree consumer rather than a dual-mode hybrid.

## Files

| File | Change |
|---|---|
| `examples/template/CMakeLists.txt` | `cmake_minimum_required` + `project()` + `find_package(veng REQUIRED)`; `REFERENCE ${veng_CORE_PACK_JSON}`; authoring calls unchanged. |
| `examples/CMakeLists.txt` | Remove `add_subdirectory(template)` — only `hello-triangle` stays in-tree. |
| `examples/template/README.md` (new or updated) | The out-of-tree / `find_package` consumption model + the co-dev override pointer. |

## Verification

- Clean in-tree build + `ctest` green — `hello-triangle` builds and its smokes pass
  (`smoke_golden`, `hello_triangle_launcher_smoke`); the engine build **no longer configures the
  template**, so a default build is unaffected by it.
- A **standalone** out-of-tree configure of `examples/template` against the build-tree export
  (`-Dveng_ROOT=$PWD/build`) configures, builds, cooks, and produces a runnable `template-launcher`
  with a correct-sized capture (the automated harness is Plan 05).
- `include_hygiene` / `validation_gate` unaffected.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
