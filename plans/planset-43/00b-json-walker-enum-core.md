# Plan 00b — the JSON⇄reflection walker + enum-name core

**Goal:** the code half of the foundation the rest of the planset adopts.
`Veng/Reflection/JsonSerialize.h` lands the shared JSON⇄reflection walker (all FieldClasses,
policy hooks for the consumer-owned parts, the merge-write + tolerant-read forms the editor
needs); `EnumName.h` gains the runtime-typed enum-name functions. Enums serialize by name
from the first line of the walker. No consumer migrates yet — this plan is the library plus
its unit tests. **Depends on Plan 00a** (the public header names `json`, which requires
nlohmann PUBLIC on `veng`).

## The starting point

- `Veng/Reflection/Serialize.h` (`WriteFields`/`ReadFields`) is the shared **binary** walker;
  the JSON direction is forked five ways (PrefabImporter, LevelImporter, PrefabSerialize,
  LevelEditorPanel's config round-trip, ReflectToJson) with no shared core. `Serialize.h`'s
  binary path reads/writes `Reference`/`AssetHandle` fields as raw bytes with no hook seam —
  it runs only post-cook, where indices are already resolved — so the JSON walker's added
  hook seam is the deliberate difference, not a gap the binary walker also has.
- `Veng/Reflection/EnumName.h` has the compile-time-typed `ParseEnum<T>`/`EnumeratorName<T>`
  over the `VE_ENUM` enumerator tables (`TypeInfo::Enumerators`); the reflection-walking
  serializers need the same over a runtime `TypeInfo&` (MCP hand-rolls exactly this loop).
- nlohmann/json is PUBLIC on `veng` as of Plan 00a, so a public reflection header may name
  `json`.

## What lands

### 1. `Veng/Reflection/JsonSerialize.h`

```cpp
/// @brief Policy hooks for the consumer-owned parts of JSON field binding.
struct JsonFieldHooks
{
    /// Validates a nonzero AssetId against the caller's resolve context (cook-time pack
    /// check). Default accepts every id.
    function<VoidResult(u64 id, TypeId fieldType)> ValidateAssetId;
    /// Maps a JSON value to an Entity (prefab-local index, live entity, ...). Unset →
    /// a Reference field is an error.
    function<Result<Entity>(const json& value)> ReadReference;
    /// The write inverse of ReadReference. Unset → a Reference field is an error.
    function<json(Entity entity)> WriteReference;
};

/// @brief Binds a name-keyed JSON object into a reflected instance (an omitted field keeps
///        its default; a malformed field is an error naming the dotted field path). By
///        default an *unknown* key is also an error (the cooker's strict posture);
///        AllowUnknownFields keeps the editor panels' tolerant read (unknown keys ignored,
///        left for the merge-write to preserve).
VoidResult JsonReadFields(void* obj, const TypeInfo& type, const json& value,
                          const TypeRegistry& registry, const JsonFieldHooks& hooks = {},
                          bool allowUnknownFields = false);

/// @brief The write inverse: a reflected instance to a fresh name-keyed JSON object.
json JsonWriteFields(const void* obj, const TypeInfo& type, const TypeRegistry& registry,
                     const JsonFieldHooks& hooks = {});

/// @brief Merge-write: assigns each reflected field into an existing object in place,
///        leaving keys the reflection layer doesn't own (comments-as-keys, hand-authored
///        structure, the world id, future fields) untouched. This is the form the editor
///        writers (PrefabSerialize, LevelEditorPanel) require to keep their save a no-op
///        diff apart from the fields that changed.
void JsonWriteFields(json& into, const void* obj, const TypeInfo& type,
                     const TypeRegistry& registry, const JsonFieldHooks& hooks = {});
```

- Implementation in `engine/src/Reflection/JsonSerialize.cpp`, beside `Serialize.cpp`.
- **Coverage is total:** Scalar (bool/f32/i32/u32/u64 by leaf `TypeId`), Vector (f32 and u32
  component storage), Quaternion (`[x,y,z,w]`), Matrix (four rows of four), String, **Enum
  (names — see below)**, AssetHandle (raw u64 id at offset 0; `0`/`null` = "no asset";
  `ValidateAssetId` on nonzero), Reference (via hooks), Struct (recursive, name-keyed),
  Variant (`{ "type": <qualified name>, "value": {…} }`, matching the established authored
  form — unknown tag leaves the variant empty), Array (via the erased container shims).
- **JSON value conventions are lifted verbatim from the existing walkers** — this plan
  changes the *implementation count*, not the on-disk shapes (except enums). Where the forks
  disagree on a detail, the PrefabImporter (the most complete, most exercised fork) is the
  reference behavior.
- **Errors are dotted field paths** ("`Settings.Bloom.Kernel`: expected an enumerator
  name"), returned as `VoidResult`; the consumer prepends its located prefix. No formatting
  hook.

### 2. The runtime-typed enum functions in `EnumName.h`

```cpp
[[nodiscard]] string EnumeratorName(const TypeInfo& info, i64 value);
[[nodiscard]] optional<i64> ParseEnumValue(const TypeInfo& info, string_view name);
[[nodiscard]] i64 LoadEnumBits(const void* fieldPtr, const TypeInfo& info);
void StoreEnumBits(void* fieldPtr, const TypeInfo& info, i64 value);
```

- The templated `ParseEnum<T>`/`EnumeratorName<T>` become thin wrappers over these (one
  matching loop, not two).
- `EnumeratorName` keeps its documented decimal-string fallback for an unmatched value;
  `ParseEnumValue` matches exactly and case-sensitively, `nullopt` otherwise.
- The walker's Enum case is: write `EnumeratorName(info, LoadEnumBits(...))`; read a JSON
  **string only**, `ParseEnumValue` → `StoreEnumBits`, error on a non-string or unknown name
  (the hard cut — readers get no integer tolerance).

### 3. Unit tests

- A `tests/unit` fixture type exercising every FieldClass (nested struct, enum, variant,
  array, asset handle, reference via a stub hook) round-trips `JsonWriteFields` →
  `JsonReadFields` to a **field-wise-equal** instance (or, equivalently, write→read→write
  produces JSON-equal output) — not a raw `memcmp`, since the fixture holds padding and
  heap-owning members.
- Enum cases pinned: exact-spelling write, exact-match read, unknown-name error, integer
  rejected, out-of-range value writes the decimal fallback and fails to re-read (the
  documented asymmetry).
- Merge-write pinned: `JsonWriteFields(json& into, …)` over an object carrying an unknown
  key leaves that key untouched while updating the reflected fields; a tolerant read
  (`allowUnknownFields = true`) ignores the unknown key, and the strict default errors on it.
- Dotted-path error content pinned for a nested failure.

## Verification

- `build-debug` clean; `ctest` green including the new unit band; `include_hygiene` still
  green with `JsonSerialize.h` now naming `json` in a public header (the 00a PUBLIC link
  covers it).

## Out of scope

- Migrating any consumer or asset (plans 01–03). The five forks still compile and run
  unchanged against their old code in this plan.
