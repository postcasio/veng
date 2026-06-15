# Plan 03 — Reflection: `TypeId` + `FieldDescriptor`/`TypeInfo` + `VE_REFLECT`

**Goal:** add the **reflection layer** the editor's deferred design described,
pulled forward as the scene serializer's prerequisite. The layer is **generic over
any type, not just components**: it reuses the single `TypeId` space plan 01 minted,
adds the closed **`FieldClass`** meta-kind, extends `TypeInfo` with `Class` + a
`vector<FieldDescriptor>` (carrying optional **editor metadata**), provides the
**`VE_REFLECT` describe-block** as the authoring surface (re-authoring the builtins
through it), and proves the layer works by a **generic in-memory serialize/
deserialize round-trip** that walks fields with no knowledge of the concrete C++
type. Pure CPU; unit-tested. **No sample change.**

## Why this is its own plan

This is the consciously-pulled-forward piece (planset-9 deferred it; an ECS with
game-defined components and a future cooked scene needs it now). Isolating it keeps
the vocabulary decision — the single open `TypeId` space, closed `FieldClass`, the
field layout, the editor-metadata slots, and the `VE_REFLECT` surface — a single
reviewable surface, and the round-trip test de-risks the deferred cooked `.scene`
asset without building any of the asset/cooker machinery.

## The field-type vocabulary — `engine/include/Veng/Reflection/TypeId.h`

`TypeId` is the **one stable id space** plan 01 defined — leaf field types, nested
structs, and components all share it, each carrying an authored `u64` id (decision 5).
This plan defines the leaf vocabulary and adds the closed meta-kind:

```cpp
// TypeId is defined in plan 01 (TypeRegistry.h): a stable u64, authored like AssetId.

// Closed meta-kind — generic handling branches on this, not on the open TypeId.
enum class FieldClass : u8
{
    Scalar, Vector, Quaternion, Matrix, String, AssetHandle, Reference, Struct, Enum
};

// Each leaf C++ type names its stable id + meta-kind through a compile-time trait.
template <class T> struct ReflectLeaf;   // engine specialises for the builtins below
```

The engine pre-specialises `ReflectLeaf<T>` for its builtin leaves — `bool`, `f32`,
`i32`, `u32`, `u64`, `vec2`, `vec3`, `vec4`, `quat`, `mat4`, `string`, and
`AssetHandle<Texture>` / `<Mesh>` / `<Material>` — each carrying a **hardcoded `TypeId`
literal** (a `0x…ULL` checked into the engine, exactly like the core pack's built-in
asset ids) and its `FieldClass`. A game adds a new leaf by specialising `ReflectLeaf`
for its own type with a `vengc generate-id` value — no engine change.

A field names its type by `TypeId`, resolved at **compile time** — `TypeIdOf<vec3>()`
reads `ReflectLeaf<vec3>::Id`. There is **no registration-ordering burden**: the id is
a constant, and the registry **auto-registers** a referenced type's `TypeInfo` from
its trait (`ReflectLeaf<T>` for leaves, `VengReflect<T>` for structs, recursively)
the first time it is needed, idempotently. `Reference` is the meta-kind for an
intra-scene `Entity` reference (e.g. `Parent`'s field); the future loader recognises
it to remap.

Component **inheritance**, when it arrives, is **single, non-virtual, base at offset
0, walked base-first** — recorded direction only; v1 types are flat structs and the
walker has no base step yet.

## `FieldDescriptor` / `TypeInfo` (fields slice) — `engine/include/Veng/Reflection/`

Extend the `TypeInfo` from plan 01 with `Class` and `Fields`, and define
`FieldDescriptor` with its serialization triple **plus optional editor metadata**:

```cpp
struct FieldDescriptor
{
    string     Name;     // serialization key — stable, code-facing; never the UI label
    TypeId     Type;     // the field's leaf / struct / component type
    FieldClass Class;    // denormalised from the field type's trait, so the walker
                         // handles a leaf field without a registry lookup
    usize      Offset;   // offsetof(Owner, member)

    // Editor metadata — optional, default-empty; the serializer ignores all of it.
    string        DisplayName;   // human label; falls back to Name when empty
    string        Tooltip;       // description
    optional<f64> Min, Max, Step;
    bool          Hidden   = false;
    bool          ReadOnly = false;
    string        Category;      // inspector grouping
};

// TypeInfo (plan 01) gains:
//   FieldClass              Class;   // the meta-kind, for leaves and structs alike
//   vector<FieldDescriptor> Fields;  // populated for Struct-class types
```

The split between `Name` (the on-disk key) and `DisplayName` (the UI label) is
deliberate: relabelling a field in the editor must never change the serialized key,
or it breaks on-disk compatibility. The serializer touches only `Name`/`Type`/
`Class`/`Offset`; everything else is editor-facing and inert until area 6 builds
inspectors. For a `Struct`-class field the walker **recurses** into the field type's
own `Fields` (looked up by `Type`), so nested structs serialize without flattening.

Registration grows two overloads (both still synthesising the lifecycle thunks from
`T` exactly as plan 01):

```cpp
// Explicit — for leaves, hand-authored types, anything a macro can't express.
template <class T>
TypeId Register(string name, FieldClass cls, vector<FieldDescriptor> fields);

// Trait-driven — reads the VE_REFLECT block written next to T (below).
template <class T>
TypeId Register();
```

## The `VE_REFLECT` authoring surface — `engine/include/Veng/Reflection/Reflect.h`

Hand-writing the `{ name, TypeId, offset, …metadata… }` list per field is verbose and
restates the field type. The **describe-block** macro is the v1 authoring surface:
the struct stays the single source of truth, and only the field *names* are restated,
once.

```cpp
struct Transform { vec3 Position{0}; quat Rotation{1,0,0,0}; vec3 Scale{1}; };

VE_REFLECT(Transform, 0x4DD9F2A1C03B5E76ULL)   // stable TypeId, minted like an AssetId
    VE_FIELD(Position, .DisplayName = "Position", .Tooltip = "Local position, parent space")
    VE_FIELD(Rotation, .DisplayName = "Rotation")
    VE_FIELD(Scale,    .DisplayName = "Scale", .Min = 0.001)
VE_REFLECT_END();
```

- `VE_REFLECT(T, <TypeId>)` carries the type's **stable id literal** (decision 5) and
  **specialises a `VengReflect<T>` trait** (`static constexpr TypeId Id`; static
  `Name()` / `Class()` = `Struct` / `Fields()`). It does **not** itself call the
  registry, so it sits at namespace scope next to the struct. The zero-arg
  `Register<T>()` reads the trait at startup.
- `VE_FIELD(member, …)` contributes a `FieldDescriptor{ .Name = #member,
  .Type = TypeIdOf<decltype(T::member)>(), .Class = /*from the field type's trait*/,
  .Offset = offsetof(T, member), …extra… }` — the field's `TypeId` and `FieldClass`
  are **compile-time constants** read off `ReflectLeaf`/`VengReflect`, no runtime
  lookup or hashing.
- The extra `…` are the optional designated-initialiser metadata fields above; omit
  them and you get a bare serialisable field.
- The raw `Register<T>(name, cls, fields)` overload **stays public** for any
  descriptor a macro can't express (and for hand-authoring).
- The only machinery is an in-house variadic `FOR_EACH`-style `__VA_OPT__` expansion
  (~20 lines of preprocessor, written once) — **no third-party dependency**. The
  `template <> struct VengReflect<T>` it emits is `inline`/header-safe for inclusion
  across TUs.

This block is the deliberate **forward-port target for C++26 annotations (P3394)**:
when AppleClang gains P2996/P3394, the names+metadata move to inline `[[=…]]` member
decorators read by static reflection, eliminating even the restated names — a
mechanical change behind this same `TypeInfo`/`FieldDescriptor` shape.

## Builtin descriptors

Re-author the builtins (plan 02 declared their ids with `VE_TYPE` and registered them
lifecycle-only) through `VE_REFLECT` — the same id, now with fields — so the engine's
own components exercise the same surface a game uses:

```cpp
VE_REFLECT(Name,   0x…ULL) VE_FIELD(Value, .DisplayName = "Name")               VE_REFLECT_END();
VE_REFLECT(Parent, 0x…ULL) VE_FIELD(Value, .DisplayName = "Parent", .ReadOnly = true) VE_REFLECT_END();
// Transform as above. Real ids are minted with `vengc generate-id` once the build is
// green (placeholder literals while implementing, per the working norms).
```

`Parent`'s `Entity` field has `FieldClass::Reference` (its leaf trait is
`ReflectLeaf<Entity>` with that class): it serializes as `{ Index, Generation }`. The
loader's remap of references into a fresh `Scene` is deferred, so this plan only needs
the **round-trip within one `Scene`** to reproduce the value — but recording it as
`Reference` (not an opaque struct) is what lets the future loader find and remap every
reference generically.

## The proof: a generic round-trip — `engine/src/Reflection/Serialize.*`

A pair of free functions, driver for the future cooked-scene serializer and the test:

```cpp
void WriteFields(vector<u8>& out, const void* obj, const TypeInfo&, const TypeRegistry&);
void ReadFields(span<const u8> in, void* obj, const TypeInfo&, const TypeRegistry&);
```

`WriteFields` walks `Fields` and, per field, writes a **`{ field-name, value }`**
record, branching on `FieldClass`:

- **leaf** (`Scalar`/`Vector`/`Quaternion`/`Matrix`/`String`) — append the leaf bytes;
- **`AssetHandle`** — append the underlying `u64 AssetId` (rehydration to a resident
  handle is the deferred loader's job);
- **`Enum`** — append the underlying integer;
- **`Reference`** — append `{ Index, Generation }` for the loader to remap;
- **`Struct`** — **recurse** into the field type's `Fields` (looked up by `Type` in
  the registry).

`ReadFields` reverses it: it reads records by name and writes each into the matching
field of a default-constructed instance, **tolerating schema drift** — a field present
in the data but absent from the descriptor is skipped, and a field in the descriptor
but absent from the data keeps its default value. Neither function knows any concrete
type — they prove the descriptors alone are enough to serialize a value, **whether a
component or any other reflected struct**. This is the exact seam the deferred
`.scene` cooker/loader drops onto; no on-disk format, no `assetformat` touch here.

## Tests

`veng_unit` (`-L unit`):

- **Vocabulary:** `TypeIdOf<T>()` for each builtin leaf is a stable, distinct
  compile-time constant (assert in a `static_assert`/`constexpr` context); its
  `FieldClass` is correct.
- **`VE_REFLECT` shape:** a struct authored through the describe-block produces
  `Fields` with the expected names/offsets and the metadata it was given
  (`DisplayName`, `ReadOnly`, range), and a `TypeId` equal to the authored literal;
  `offsetof` round-trips (write a value through the descriptor offset, read it back
  through the typed member). `DisplayName` defaults to `Name` when omitted.
- **Generic round-trip:** populate a `Transform` (and a `Name`) with known values,
  `WriteFields` then `ReadFields` into a fresh instance, assert field-equal — driven
  **only** through the descriptor, never the typed struct. The editor-metadata fields
  do **not** affect the serialized bytes.
- **Nested struct recursion:** a struct with a `Struct`-class field round-trips,
  proving the walker recurses (auto-registering the nested type's schema on
  reference).
- **Schema tolerance:** serialize, then `ReadFields` into a type whose descriptor has
  a field **removed** (the extra record is skipped) and one with a field **added**
  (the missing field keeps its default) — neither corrupts the rest.
- **`AssetHandle` field:** a field of `AssetHandle<Mesh>` writes/reads its underlying
  `AssetId` (value survives; residency is out of scope — no `AssetManager` here).
- **`Enum` field:** an enum field round-trips as its underlying integer.
- **`Reference` field:** an `Entity` field round-trips its `{ Index, Generation }`
  within one `Scene`.
- **Generic over non-components:** reflect a plain struct that is *never* added to a
  `Scene` and round-trip it, proving the layer is not component-bound.
- **Open extension:** specialise `ReflectLeaf` for a fake game leaf `TypeId` + a
  struct using it and round-trip it, proving the vocabulary extends without an engine
  change.

`include_hygiene`: add `Veng/Reflection/TypeId.h`,
`Veng/Reflection/FieldDescriptor.h`, `Veng/Reflection/Reflect.h` (the `TypeInfo`
field/`Class` additions are already covered via `TypeRegistry.h`).

## Acceptance

Clean build; `ctest -L unit` green; `include_hygiene` builds. Sample unchanged.
Commit: `Plan 03: reflection layer — TypeId vocabulary, FieldDescriptor + editor
metadata, VE_REFLECT, generic serialize round-trip`.
