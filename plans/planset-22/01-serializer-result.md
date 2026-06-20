# Plan 01 — ReadFields returns a Result

**Goal:** turn the reflection serializer's read side from fatal-on-bad-bytes into recoverable.
`ReadFields` returns a `VoidResult`; the byte-level truncation guards in
[Serialize.cpp](../../engine/src/Reflection/Serialize.cpp) return `std::unexpected` instead of
`VE_ASSERT`; the schema/registry lookups stay fatal; `WriteFields` is untouched. This is the
pure, device-free core — its value lands in in-process unit tests over malformed input that the
abort previously made impossible to write.

## What lands

### `ReadFields` signature ([engine/include/Veng/Reflection/Serialize.h](../../engine/include/Veng/Reflection/Serialize.h))

```cpp
// Reads fields from in into a (default-constructed) obj per the descriptors,
// tolerating drift in either direction. A truncated record — a length prefix
// or value that runs past the end of `in` — is a recoverable error, not an
// abort: the byte stream may come from a corrupt cooked blob or a mid-edit
// source. A descriptor that names a type the registry does not hold is a
// schema/registration fault and stays a fatal assert.
VE_API VoidResult ReadFields(std::span<const u8> in, void* obj, const TypeInfo& type,
                             const TypeRegistry& registry);
```

`WriteFields` keeps its `void` return — it appends a registered in-memory value's bytes and
cannot meet a truncated stream.

### The reader bodies ([engine/src/Reflection/Serialize.cpp](../../engine/src/Reflection/Serialize.cpp))

Each truncation `VE_ASSERT` becomes a recoverable error. The internal helpers thread the result:

- **`ReadU32`** returns `Result<u32>` (or takes a span and returns `std::unexpected` on
  `cursor + sizeof(u32) > in.size()`).
- **`ReadValue`** returns `VoidResult`; its `Scalar/Vector/Quaternion/Matrix/Enum` leaf guard,
  `String` length+payload guards, and `AssetHandle` id guard each `return std::unexpected(...)`
  on overrun. The `registry.Info(field.Type)` lookups for a leaf size and for a nested struct
  type **stay** `VE_ASSERT` — an unregistered type is a registration fault.
- **`ReadFieldsInner`** returns `Result<usize>` (the consumed-byte count, so a nested struct can
  still advance its parent on success) or `std::unexpected` on a truncated record-count, name
  length, name bytes, or value length. The nested-`Struct` recursion propagates a child failure
  up.
- **`ReadFields`** calls `ReadFieldsInner` and maps its `Result<usize>` to `VoidResult`
  (discarding the count).

Error messages stay **located and factual**, matching the existing wording style — e.g.
`"ReadFields: truncated string"`, `"ReadFields: field value runs past end of record"`. They name
*what* overran, not a plan or a fix.

### Forced caller migration

The signature change has a precise, bounded blast radius. `VoidResult` is
`std::expected<void, std::string>` with **no `[[nodiscard]]`**, so a caller that ignores the
return still **compiles** — the existing happy-path read sites (13 in
[tests/unit/reflection.cpp](../../tests/unit/reflection.cpp), 6 in
[tests/cooker/prefab_cook.cpp](../../tests/cooker/prefab_cook.cpp), and the two engine sites)
need no compile-driven change. Two sites are **not** optional, though:

- **The three serializer death cases are deleted.** `serialize_truncated_header`,
  `serialize_truncated_value`, and `serialize_truncated_string`
  ([tests/death/death_main.cpp:226–279](../../tests/death/death_main.cpp), registered at
  [CMakeLists.txt:358–367](../../CMakeLists.txt)) assert that malformed bytes **abort** — the
  exact contract this plan reverses. Once `ReadFields` returns instead of aborting, each body
  returns cleanly, the death harness never sees its expected `PASS_REGULAR_EXPRESSION` abort
  message, and the three tests fail. So this plan **removes** the three `Run*` functions, their
  dispatch arms ([death_main.cpp:392–394](../../tests/death/death_main.cpp)), the three `add_test`
  blocks, and the now-false "the serializer's contract on malformed bytes is a loud fatal assert"
  comment. All three are truncation cases — precisely the newly-recoverable path — so none
  convert; their coverage **moves** to the in-process `.error()` assertions added to
  `reflection.cpp` below.
- **The prefab loader's load-time read** is the other forced site — Plan 02 — because ignoring the
  result there would silently leave a half-populated instance (worse than today's abort), so it
  must propagate. The spawn-time read stays an assert (the record validated at load).

The happy-path test/cooker calls **may** optionally be wrapped (`CHECK(ReadFields(...))` /
`.value()`) to assert success rather than ignore it — a small polish, not forced, and not part of
this plan's required diff.

## Decisions

1. **Only the read side gains a `Result`.** `WriteFields` serializes a live, registered object;
   it has no untrusted input. Its sole failure mode — a descriptor naming an unregistered nested
   type — is the schema assert, which is fatal in both directions. Keeping the write side `void`
   means no cooker churn (the `PrefabImporter` uses only `WriteFields`) and a smaller surface.

2. **Truncation is recoverable; a broken schema is fatal — the one line drawn.** The
   `cursor + n <= in.size()` checks guard against *bad bytes* (a corrupt or mid-edit stream) and
   become `Result` errors. The `registry.Info(...)` lookups guard against *a descriptor for a
   type the registry was never given* — registration misuse the host must fix — and stay
   `VE_ASSERT`. This is the [CLAUDE.md](../../CLAUDE.md) "API misuse → assert, recoverable →
   `Result`" split applied literally, and it keeps the diff confined to the truncation arithmetic.

3. **`ReadFieldsInner` keeps returning the consumed count, now inside a `Result`.** A nested
   struct still advances its parent by the child's consumed bytes — the count is wrapped in
   `Result<usize>`, not dropped. The public `ReadFields` discards the count and returns
   `VoidResult`.

4. **Drift tolerance is unchanged.** An unknown record (no matching descriptor) is still skipped
   by its length prefix, and a descriptor absent from the data still keeps its default — those
   paths never asserted and don't change. Only the *truncation* guards move from abort to error.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Reflection/Serialize.h` | `ReadFields` → `VoidResult`; doc comment states the truncation-recoverable / schema-fatal split. `WriteFields` unchanged. |
| `engine/src/Reflection/Serialize.cpp` | `ReadU32`/`ReadValue`/`ReadFieldsInner` return `Result`; truncation `VE_ASSERT`s → `std::unexpected`; `registry.Info` asserts kept; propagate child failures through the `Struct` recursion. |
| `tests/unit/reflection.cpp` | Add the malformed-input cases (below). The existing drift-skip work in the working tree stays; the new cases assert on `ReadFields(...).error()`. |
| `tests/death/death_main.cpp` | Delete the three `RunSerializeTruncated*` functions, their dispatch arms, and the "loud fatal assert" comment — their truncation contract is reversed here. |
| `CMakeLists.txt` | Delete the three `death.serialize_truncated_*` `add_test` blocks (≈ lines 358–367). |

## Verification

- Clean build; `include_hygiene` compiles `Veng/Reflection/Serialize.h` (already public, glm/std
  only — `VoidResult` is `Veng::Result<void>`, in `Veng.h`).
- **`tests/unit/reflection.cpp`** (device-free, no ICD), each asserting `ReadFields` **returns an
  error rather than aborting**:
  - **Truncated record count** — a buffer holding fewer than 4 bytes → error.
  - **Truncated leaf** — a record claiming a `f32`/`vec3` field whose value bytes run past the
    end → error, no read past the span.
  - **Truncated string** — a `String` field whose length prefix exceeds the remaining bytes →
    error.
  - **Truncated asset id** — an `AssetHandle` field with fewer than 8 value bytes → error.
  - **Truncated name** — a record whose name length exceeds the buffer → error.
  - **A valid round-trip still succeeds** — `WriteFields` then `ReadFields` returns a value and
    the fields compare equal (the `PlainData`/`Labeled` fixtures already present), proving the
    happy path is unchanged.
  - **Drift recovery returns success** — an unknown trailing record is skipped and a
    descriptor-only field keeps its default, both with `ReadFields` returning a *value* (the
    recovery the death harness could not assert).
- `smoke_golden` is **byte-identical** — no encoding change.
- **Death band**: the three `death.serialize_truncated_*` cases are **gone** (deleted, not
  failing) — their truncation-aborts contract is reversed, and `ctest -L death` lists three fewer
  cases. The remaining death cases (stale `Entity`, GPU misuse, etc.) are unaffected.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
