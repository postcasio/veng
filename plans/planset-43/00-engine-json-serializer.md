# Plan 00 — the engine JSON serializer + enum-name core

**Goal:** the foundation the rest of the planset adopts. nlohmann/json becomes a PUBLIC
dependency of `libveng`; `Veng/Reflection/JsonSerialize.h` lands the shared JSON⇄reflection
walker (all FieldClasses, policy hooks for the consumer-owned parts); `EnumName.h` gains the
runtime-typed enum-name functions. Enums serialize by name from the first line of the walker.
No consumer migrates yet — this plan is the library plus its unit tests.

## The starting point

- `Veng/Reflection/Serialize.h` (`WriteFields`/`ReadFields`) is the shared **binary** walker;
  the JSON direction is forked four ways (PrefabImporter, LevelImporter, PrefabSerialize,
  ReflectToJson) with no shared core.
- `Veng/Reflection/EnumName.h` has the compile-time-typed `ParseEnum<T>`/`EnumeratorName<T>`
  over the `VE_ENUM` enumerator tables (`TypeInfo::Enumerators`); the reflection-walking
  serializers need the same over a runtime `TypeInfo&` (MCP hand-rolls exactly this loop).
- nlohmann/json is FetchContent-pinned at the top level and linked PRIVATE by cooker
  (`veng_cook_objs` has it PUBLIC internally), editor, graph, and mcp; `libveng` has no JSON
  dependency and `include_hygiene` compiles public headers against PUBLIC deps only.

## What lands

### 1. nlohmann/json PUBLIC on `veng`

- `target_link_libraries(veng PUBLIC nlohmann_json::nlohmann_json)`; the FetchContent setup
  moves ahead of the engine target if it isn't already.
- **SDK export/install:** the installed `veng-config.cmake` gains
  `find_dependency(nlohmann_json)`, and the install prefix carries nlohmann (enable the
  FetchContent'd project's install — `JSON_Install` — so the SDK is self-contained, matching
  how the other exported PUBLIC deps resolve). The build-tree mode already has the target in
  scope; verify the exported `vengTargets` resolve it there too.
- `include_hygiene` needs no change in intent — nlohmann simply joins glm/fmt/ImGui in the
  PUBLIC link set it compiles against.
- Consumers' now-redundant PRIVATE links (editor, mcp, graph, cooker) are dropped where the
  transitive PUBLIC edge covers them; comments in those CMakeLists that assert "nlohmann
  stays PRIVATE / never reaches a public header" are updated to the new posture.
- **Acceptance:** `sdk_conformance_install` and `sdk_conformance_buildtree` green — all three
  consumption modes resolve the new dependency.

### 2. `Veng/Reflection/JsonSerialize.h`

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

/// @brief Binds a name-keyed JSON object into a reflected instance (tolerant: an omitted
///        field keeps its default; an unknown or malformed field is an error naming the
///        dotted field path).
VoidResult JsonReadFields(void* obj, const TypeInfo& type, const json& value,
                          const TypeRegistry& registry, const JsonFieldHooks& hooks = {});

/// @brief The write inverse: a reflected instance to a name-keyed JSON object.
json JsonWriteFields(const void* obj, const TypeInfo& type, const TypeRegistry& registry,
                     const JsonFieldHooks& hooks = {});
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

### 3. The runtime-typed enum functions in `EnumName.h`

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

### 4. Unit tests

- A `tests/unit` fixture type exercising every FieldClass (nested struct, enum, variant,
  array, asset handle, reference via a stub hook) round-trips `JsonWriteFields` →
  `JsonReadFields` byte-identically.
- Enum cases pinned: exact-spelling write, exact-match read, unknown-name error, integer
  rejected, out-of-range value writes the decimal fallback and fails to re-read (the
  documented asymmetry).
- Dotted-path error content pinned for a nested failure.

## Verification

- `build-debug` clean; `ctest` green including the new unit band; `include_hygiene` green
  with the new PUBLIC dep; `sdk_conformance_install` / `sdk_conformance_buildtree` green.

## Out of scope

- Migrating any consumer or asset (plans 01–03). The four forks still compile and run
  unchanged against their old code in this plan.
