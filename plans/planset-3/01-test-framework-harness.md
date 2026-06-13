# Plan 01 — Framework + harness + CTest wiring

**Goal:** integrate doctest and stand up the scaffolding every other plan in this
planset builds on: a unit-test executable, a death-test harness, shared test
support, and CTest registration. No engine code changes.

## Why first / dependencies

Foundation. 02–06 all add cases against the harness this plan defines. It carries
the cross-cutting decisions (framework, how death tests run, naming, the ICD-skip
helper), so it lands first and on the main thread; the rest fan out from it.

## Work

1. **Fetch doctest** via `FetchContent` in the top-level `CMakeLists.txt`, pinned
   to a release tag, `EXCLUDE_FROM_ALL`, only under `VENG_BUILD_TESTS`. Header-only
   `INTERFACE` target `doctest::doctest`.

2. **`veng_unit` target** — the single executable that hosts the framework-driven
   cases (plans 02, 03, 04). One TU provides `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`
   (`tests/unit/main.cpp`); each band adds its own `*.cpp`. Links
   `veng::veng` + `doctest::doctest`. Registered via doctest's CTest discovery
   (`doctest_discover_tests`) or a single `add_test`, so cases show individually
   in `ctest`.
   - Confirm the TU does **not** inherit `-fno-exceptions` (it links veng but is a
     separate target — verify the compile line). doctest needs exceptions.

3. **Death-test harness** — `VE_ASSERT` calls `std::abort()`, which doctest cannot
   trap in-process, so death cases run as **separate processes**:
   - One small executable `tests/death/death_main.cpp` that switches on `argv[1]`
     (the case name), runs exactly that offending operation, and is *expected to
     abort*. Unknown/no case name → exit non-zero with no assert message (so a
     misregistered case fails loudly rather than passing).
   - **Mechanism (corrected during implementation):** the original plan assumed
     `WILL_FAIL TRUE` would invert the SIGABRT into a pass. It does **not** —
     CTest reports a signal death as "Subprocess aborted", which neither
     `WILL_FAIL` nor `PASS_REGULAR_EXPRESSION` overrides (verified against this
     CMake). So the harness installs a `SIGABRT` handler that, *after*
     `FatalAssert` has already logged + flushed its message to stderr, converts
     the abort into a clean exit via `std::_Exit`. Each death case is then an
     `add_test(NAME death.<case> COMMAND veng_death <case>)` registered with
     **only** `PASS_REGULAR_EXPRESSION` pinned to that case's assert message —
     the message match is what decides pass/fail. Use one explicit `add_test`
     per case (not auto-discovery) — death cases are few and reading them one per
     line is clearer.
   - This pins the assert message against the `FatalAssert` log line (routed to
     unbuffered stderr by the harness so it survives `abort()`), so a case that
     aborts for the *wrong* reason — or fails to abort at all (clean exit, or an
     unknown/missing case name) — never matches and fails. (The framework-driven
     unit cases in `veng_unit`, by contrast, use doctest's
     `doctest_discover_tests` so each shows individually in `ctest`.)
   - The harness here is just the dispatch + registration *mechanism*; the actual
     cases are plan 05. Land this plan with one trivial sentinel death case (e.g.
     `VE_ASSERT(false, ...)` under name `sentinel`) to prove the wiring.

4. **Shared support** under `tests/support/`:
   - A `RequiresVulkan()` / skip helper for the GPU band: probe for a Vulkan
     driver (attempt minimal instance creation) and let a GPU test bail out as
     *skipped*, not *failed*, where none is present — so the suite still runs on a
     machine without a working driver. Pure and type-mapping bands never call it.
     (Consumed by plan 06; defined here so it has one home.) Keep it minimal — this
     is robustness across your own machines, not CI provisioning.
   - Small assertion/format helpers as needed (e.g. pixel-compare for the GPU
     band) — keep minimal; grow in the plans that use them.

5. **CTest labels** — label tests `unit`, `gpu`, `death` so you can select bands
   locally: `ctest -L unit` is the fast, driver-free feedback loop; `ctest`
   (no filter) runs everything.

## Acceptance

- `cmake -B build -S . && cmake --build build` builds `veng_unit` and `veng_death`.
- `ctest --test-dir build -L unit` passes (just the sentinel/empty suite so far).
- `ctest --test-dir build -R death.sentinel` passes (proves WILL_FAIL wiring).
- `veng_unit` runs to completion with **no Vulkan ICD** on `PATH`/loader.
- `include_hygiene` still green — no public header pulled into the framework setup.

## Notes

- Keep doctest out of the installed/exported surface — it is test-only, never a
  dependency of `veng` itself.
- Do not register the GPU one-exe tests here; plan 06 owns their consolidation.
