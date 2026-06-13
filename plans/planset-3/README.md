# planset-3 — unit testing & test infrastructure

**Phase goal:** stand up a real unit-test setup and a base suite that exercises
engine functionality, so later phases (the de-globalize sweep especially) have a
safety net. This is **area 5, first half** from [future/](../future/README.md)
("test harness + pure-logic tests"), plus consolidating the existing GPU smoke
tests under a coherent harness.

Today `tests/` is three hand-rolled CTest executables (`include_hygiene`,
`headless_smoke`, `compute_dispatch`) — no framework, no pure-logic coverage, no
death tests. This planset closes that.

## Scope decision

- **In:** a header-only, cross-platform framework, CMake/CTest wiring, a
  death-test harness, and three bands of base coverage — **pure-logic** (no GPU),
  **type-mapping round-trips**, and **GPU-backed** exercises in the existing
  one-exe-per-test style. Plus the one refactor needed to make barrier logic
  device-testable.
- **Out of scope:** CI. This is local-development tooling — the suite is something
  you run on your own machine(s) across platforms (`ctest`), not a hosted pipeline.
  GPU tests skip gracefully where no Vulkan driver is present (so the suite still
  runs on a box without one), but there is no CI job, no software-ICD provisioning,
  and no automated validation gate in this planset.
- **Deferred to a later planset (area 5b, *after* de-globalize):** the in-process
  multi-case GPU integration suite that stands a `Context` up/down per case. It
  must target the explicit-device API, so writing it now would mean rewriting it
  when `Context::Instance()` goes, and it can't get real per-test isolation while
  the context is a singleton. The existing one-exe-per-test GPU tests already get
  a fresh singleton per process, so extending *those* now is safe; a many-cases
  in-one-process GPU framework is not. See the future ordering note (5a → 3
  de-globalize → 5b).

## Framework: doctest

Single-header, the fastest to compile of the candidates, and trivial CTest
integration. The `-fno-exceptions` constraint is **not** a problem: it is PRIVATE
to the `veng` target, and test executables link `veng::veng` without inheriting
it, so a throwing framework is fine in test TUs (confirmed against
`CMakeLists.txt:176`). Catch2 is the fallback if doctest proves limiting; the
suite's assertions are written in plain enough macros to port if needed.

## Plans

| # | Plan | Status | Depends on |
|---|------|--------|-----------|
| 01 | [Framework + harness + CTest wiring](01-test-framework-harness.md) | done | — |
| 02 | [Pure-logic suite (no GPU)](02-pure-logic-suite.md) | done | 01 |
| 03 | [Type-mapping round-trips](03-type-mapping-roundtrips.md) | done | 01 |
| 04 | [Extract & test the barrier-decision rule](04-barrier-decision-extraction.md) | proposed | 01 |
| 05 | [Death tests (VE_ASSERT)](05-death-tests.md) | proposed | 01 |
| 06 | [Consolidate & extend GPU tests](06-gpu-test-consolidation.md) | proposed | 01 |

## Dependency graph (for delegation)

```
01 framework + harness  (foundation — land first, main thread)
   │
   ├─► 02 pure-logic suite ─────────┐
   ├─► 03 type-mapping round-trips ──┤  independent leaves —
   ├─► 04 barrier extraction* ───────┤  parallelize across subagents
   ├─► 05 death tests ───────────────┤
   └─► 06 gpu consolidation ─────────┘
```

**Delegation guidance.** Plan **01** is the foundation and carries the design
decisions (framework, harness mechanism, CTest conventions) — keep it on the main
thread. Once it lands, **02 / 03 / 05 / 06 are independent leaves** with no shared
files — ideal to fan out to `model: sonnet` subagents in parallel. **04 is the one
plan that edits engine code** (`Backend/Barrier.cpp`, `RenderGraph.cpp`); scope it
to a subagent but review and verify on the main thread (validation build), since a
barrier-logic regression is exactly what the smoke tests can miss (see
[CLAUDE.md](../../CLAUDE.md) — validation errors don't fail tests).

> \* 04 touches engine source; the other leaves only add files under `tests/`.

## On completion

Revisit [future/](../future/README.md) and restate what testing work actually
remains. This planset delivers area 5a; area 5b (the in-process multi-case GPU
integration suite) is written *against* the de-globalized, explicit-device API —
so its exact shape isn't fixed until the de-globalize change (future area 3)
lands. When planset-3 is done, update the area-5 note to reflect the remaining
suite in light of where de-global ends up, rather than the pre-de-global sketch
captured there now.

## Conventions

- New tests live under `tests/`; framework + helpers under `tests/support/`.
- One CMake test target per band (`veng_unit` for the framework cases, separate
  executables for the GPU one-exe tests and death harness), each registered with
  `add_test`.
- Pure-logic and type-mapping cases must run with **no Vulkan driver present**
  (so `ctest -L unit` is fast feedback on any machine). Only the GPU band needs a
  driver; it reports *skipped*, not *failed*, where none is found (plan 01's
  helper).
- Cross-platform: doctest + CTest run on macOS/Linux/Windows. Nothing here assumes
  a particular OS or a CI environment.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified. Fleshed out and ordered before
> implementation, planset-1 style.
