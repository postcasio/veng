# Plan 02 — Pure-logic suite (no GPU)

**Goal:** unit-test the engine's device-free logic. These cases construct no
`Context` and touch no Vulkan, so they run anywhere — they are the safety net for
the de-globalize sweep that follows this planset.

## Dependencies

Needs plan 01 (the `veng_unit` target). Independent of 03/04/05/06 — a leaf,
ideal to delegate. New file(s) only: `tests/unit/pure_logic.cpp`.

## Coverage

1. **`Result<T>` / `VoidResult`** (`Veng/Result.h`):
   - Success carries the value; `.has_value()` / `operator bool` true; `*` / `.value()` correct.
   - Error carries the string; `.error()` correct; truthiness false.
   - `VoidResult` success/error paths.
   - These are thin over `std::expected`, so this is a contract/smoke check of the
     alias and its use pattern, not a re-test of the standard library — keep it short.

2. **`VertexBufferLayout`** (`Veng/Renderer/VertexBufferLayout.h`, computed in the
   ctor — device-free):
   - Per-element `Offset` is the running sum of prior element sizes; first is 0.
   - `GetStride()` equals the total of element sizes (e.g. `RG32Sfloat` + `RGB32Sfloat`
     → 8 + 12 = 20).
   - `GetFloatCount()` equals the summed component counts (2 + 3 = 5).
   - Both the `initializer_list` and `vector` ctors yield identical results
     (guards the duplicated ctor bodies from drifting).
   - Single-element and many-element layouts.

3. **Typed-buffer size math is deliberately *not* unit-tested here.** The
   arithmetic in `TypedBuffers.h` (`vertexCount * sizeof(V)`, `sizeof(T)`,
   `elementCount * sizeof(T)`, U16/U32 stride selection) is trivial, and observing
   it device-free would mean extracting a helper out of `Create` purely to test a
   one-liner — not worth the indirection. It is covered **end-to-end on the GPU**
   by the typed-buffer round-trip in [plan 06](06-gpu-test-consolidation.md):
   upload N elements, download, compare — a wrong size fails there. Do **not**
   reshape `TypedBuffers.h` for testability.

## Acceptance

- New cases under label `unit`; `ctest -L unit` green with **no ICD present**.
- No `Context`, no `Image`/`Buffer` allocation, no Vulkan symbol referenced from
  this TU.

## Notes

- Resist re-testing `std::expected` / `std::vector` themselves — test veng's use
  of them, not the standard library.
- This plan adds **no** engine-code changes; it only adds a test file. If you find
  yourself wanting to extract something to test it, stop — the rule is to extract
  only non-trivial, silently-failing logic (that's plan 04's `DecideBarrier`), not
  trivial arithmetic. Trivial logic gets covered end-to-end instead.
