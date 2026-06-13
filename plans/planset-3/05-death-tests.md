# Plan 05 — Death tests (VE_ASSERT)

**Goal:** prove the engine's fatal-assert contract — the misuse cases that *must*
abort actually do. `VE_ASSERT` → `FatalAssert` → `std::abort()`, so each case runs
as its own process via the death harness from plan 01.

## Dependencies

Needs plan 01 (the `veng_death` harness + its `PASS_REGULAR_EXPRESSION`
registration; the SIGABRT-trap-to-clean-exit mechanism, not `WILL_FAIL` — see
plan 01). Independent
of the other leaves. Cases split by whether they need a GPU:
- **Pure-logic death cases** — no `Context`; run with no ICD.
- **GPU-coupled death cases** — need a headless `Context` (already available via
  the existing headless path); gated like the GPU band (skip with no driver, via
  plan 01's `RequiresVulkan()` helper). Keep these in the same harness but label
  them `gpu`+`death`.

## Cases

Add each as a named branch in `tests/death/death_main.cpp` and register with a
`PASS_REGULAR_EXPRESSION` on the assert message that pins the right failure (no
`WILL_FAIL` — see plan 01's mechanism note).

**Pure-logic (no device):**
1. `vertex_format_unknown` — build a `VertexBufferLayout` with a `Format` outside
   the vertex subset (e.g. `Format::RGBA8Unorm`) → `GetFormatSize` aborts
   ("Unknown vertex element Format").
2. `tovk_unmapped` — call a `ToVk` on a deliberately out-of-range enum value →
   "unmapped" abort. (Complements plan 03's success-path coverage.)
3. `assert_message` — a direct `VE_ASSERT(false, "...")` whose message is matched
   by regex, proving `FatalAssert` routes the formatted message to the log sink
   before aborting.

**GPU-coupled (need a headless `Context`):**
4. `index_u16_into_u32` — create a `U32` `IndexBuffer` and `Upload` a
   `std::span<const u16>` → aborts ("is U32; cannot upload u16 indices"), and the
   symmetric `index_u32_into_u16`. (From the future draft.)
5. `buffer_upload_overrun` — `Buffer::Upload` with `offset + size > buffer size`
   → aborts ("upload out of range"). Cheap, high-value bounds check.
6. `descriptor_type_mismatch` — write a resource to a `DescriptorSet` binding whose
   declared `DescriptorType` doesn't match. Confirmed present: every
   `DescriptorSet::Write` overload asserts the binding type via
   `m_Layout->GetBindingType(binding)` ([DescriptorSet.cpp:44-49, 74-79,
   109-114](../../src/Renderer/Backend/DescriptorSet.cpp)). Concrete case: declare a
   layout with a `UniformBuffer` binding, allocate the set, then
   `Write(binding, someImageView)` → hits the "not a sampled or storage image"
   assert. (Needs a `Context` to allocate the set, so this is a GPU-coupled case.)

## Acceptance

- `ctest -R '^death\.'` passes (each death case aborts, the harness converts the
  SIGABRT to a clean exit, and the pinned message matches).
- Pure-logic death cases pass with **no ICD**; GPU-coupled ones skip cleanly with
  no ICD and pass with one.
- Each registered message regex actually matches the emitted `FatalAssert` line
  (verify a deliberate wrong-regex fails, so the match isn't vacuous).

## Notes

- For an unknown case name the harness exits non-zero *without* emitting any
  assert message, so a typo in registration fails the test (the
  `PASS_REGULAR_EXPRESSION` never matches) rather than passing silently.
- Every case above asserts in code that already exists — this plan adds no engine
  asserts. If a future death case needs a new assert, that's an API-contract change
  to raise separately, not slip in here.
