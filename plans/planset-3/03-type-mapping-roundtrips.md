# Plan 03 — Type-mapping round-trips

**Goal:** lock down the vocabulary-enum ↔ Vulkan mappings in
`Veng/Renderer/Backend/TypeMapping.h` — the exhaustive `ToVk`/`FromVk` switches
that every resource depends on. A wrong or missing mapping is silent UB at the
backend boundary; these tests make it a failing case.

## Dependencies

Needs plan 01. Independent leaf — delegate freely. New file:
`tests/unit/type_mapping.cpp`.

**Linkage decision:** unlike plan 02, this TU includes a backend header
(`TypeMapping.h` pulls in `vk::`), so it needs the Vulkan include directories.
`veng_unit` links `veng::veng` (PRIVATE Vulkan) and won't have them by default.
**Give `veng_unit` the Vulkan headers directly** — `target_link_libraries(veng_unit
PRIVATE Vulkan::Vulkan)` (headers only; no behavioural coupling) — and keep one
unit-test executable rather than splitting out a `veng_unit_backend`. This does
**not** violate include-hygiene: that guard is specifically about *public* headers
leaking backend types, and `TypeMapping.h` is a backend header that tests are
allowed to use. (If the Vulkan headers ever pull something into `veng_unit` that
makes the pure-logic cases stop building driver-free, revisit by splitting — but
headers alone don't require a driver at *run* time, only at compile time.)

## Coverage

1. **`FromVk(ToVk(x)) == x`** for every enumerator of the enums that have a
   `FromVk` inverse — `Format` and `ImageLayout`. Iterate the full enum range and
   assert the round-trip. This catches an asymmetric edit to one direction.

2. **`ToVk` total coverage** for the enums *without* an inverse (`ImageType`,
   `IndexType`, `ShaderStage`, `LoadOp`, `StoreOp`, `CompareOp`, `CullMode`,
   `PolygonMode`, `Filter`, `MipmapMode`, `AddressMode`, `BorderColor`,
   `PipelineBindPoint`, `DescriptorType`, `BlendFactor`, `BlendOp`, …): iterate
   every enumerator and assert `ToVk` does not abort and yields a distinct,
   plausible value. The point is to **fail when a new enumerator is added without
   a mapping** — turning today's runtime `VE_ASSERT(false, "unmapped …")` into a
   compile-time-adjacent test signal.
   - A mismatched enumerator aborts inside `ToVk`; that is a *death*, not a
     comparison failure, so the "unmapped enumerator aborts" check belongs in the
     death band (plan 05). Here, assert the **mapped** values are correct for a
     representative sample and that iterating the *currently defined* enumerators
     all succeed.

3. **Flag combiners** — `ToVk(ImageUsage)` / `ToVk(BufferUsage)` /
   `ToVk(ShaderStage)` are bitwise OR-combiners. Assert single flags map to the
   expected `vk::*FlagBits` and that OR-ed inputs produce OR-ed outputs.

4. **Composite mappers** — `ToVk(const BlendState&)` and `ToVk(const ClearValue&)`
   assemble structs from sub-mappings; spot-check a representative value of each.

## Keeping it maintainable

- Drive the round-trip/coverage loops from a single list of enumerators per enum
  so adding an enumerator is the only edit needed. Where the enum has no
  reflection, a hand-maintained `constexpr` array in the test is acceptable — its
  job is to *notice* drift.

## Acceptance

- Cases under label `unit`; green with **no ICD** (mapping is pure, no device).
- Adding an unmapped enumerator to `Types.h` makes a case fail (verify by a
  temporary local edit during development).
