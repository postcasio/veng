# Plan 03 — Reflection: `TypeId` + `FieldDescriptor`/`TypeDescriptor`

**Goal:** add the **reflection layer** the editor's deferred design described,
pulled forward as the scene serializer's prerequisite. Define the open **`TypeId`**
field-type vocabulary (engine builtins pre-registered) and the closed
**`FieldClass`** meta-kind, extend `TypeDescriptor` with a `vector<FieldDescriptor>`,
write **hand-written** field descriptors for the builtin components, and prove the
layer works by a **generic in-memory serialize/deserialize round-trip** that walks
fields with no knowledge of the concrete C++ type. Pure CPU; unit-tested. **No
sample change.**

## Why this is its own plan

This is the consciously-pulled-forward piece (planset-9 deferred it; an ECS with
game-defined components and a future cooked scene needs it now). Isolating it keeps
the vocabulary decision — open `TypeId`, closed `FieldClass`, the field layout — a
single reviewable surface, and the round-trip test de-risks the deferred cooked
`.scene` asset without building any of the asset/cooker machinery.

## The field-type vocabulary — `engine/include/Veng/Reflection/TypeId.h`

```cpp
using TypeId = u32;   // open: engine builtins below, games extend it

// Closed meta-kind — generic handling branches on this, not on the open TypeId.
enum class FieldClass : u8
{
    Scalar, Vector, Quaternion, Matrix, String, AssetHandle, Struct, Enum
};
```

`TypeId` is **open**: the engine pre-registers its builtin field leaf types — `Bool`,
`F32`, `I32`, `U32`, `U64`, `Vec2`, `Vec3`, `Vec4`, `Quat`, `Mat4`, `String`, and
`AssetHandle<Texture>` / `<Mesh>` / `<Material>` — and a game can register a new one
without an engine change. `FieldClass` is **closed**: a generic walker (serializer,
later the editor inspector) switches on the small meta-kind and reads the leaf bytes
accordingly. This is the split planset-9 fixed for the editor; reusing it here means
the editor's inspectors and this serializer share one vocabulary.

Component **inheritance**, when it arrives, is **single, non-virtual, base at offset
0, walked base-first** — recorded direction only; v1 components are flat structs and
the walker has no base step yet.

## `FieldDescriptor` / `TypeDescriptor` (fields slice) — `engine/include/Veng/Reflection/`

Extend the `TypeDescriptor` from plan 01:

```cpp
struct FieldDescriptor
{
    string  Name;
    TypeId  Type;     // a registered leaf type (or a registered struct/component type)
    usize   Offset;   // offsetof(Component, member)
};

// TypeDescriptor (plan 01) gains:
//   vector<FieldDescriptor> Fields;
```

Registration grows a fields-aware overload:

```cpp
template <class T>
ComponentId Register(string name, vector<FieldDescriptor> fields);
```

Each `FieldDescriptor` is hand-written, citing `offsetof(T, member)` and the leaf
`TypeId` (looked up by the engine's pre-registered ids). The lifecycle thunks are
still synthesised from `T` exactly as in plan 01.

## Builtin descriptors

Rewrite the builtins' registration (plan 02 registered them lifecycle-only) to carry
fields:

```cpp
registry.Register<Transform>("Transform", {
    { "Position", TypeIdOf<vec3>(), offsetof(Transform, Position) },
    { "Rotation", TypeIdOf<quat>(), offsetof(Transform, Rotation) },
    { "Scale",    TypeIdOf<vec3>(), offsetof(Transform, Scale)    },
});
// Name { string }, Parent { Entity } likewise.
```

`Parent`'s `Entity` field needs a representation for serialization — an `Entity`
serializes as its `{ Index, Generation }`, remapped on load. v1 records the field as
a struct leaf; the remap policy is the cooked-scene loader's concern (deferred), so
this plan only needs the **round-trip within one `Scene`** to reproduce the value.

## The proof: a generic round-trip — `engine/src/Reflection/Serialize.*`

A pair of free functions, driver for the future cooked-scene serializer and the test:

```cpp
void WriteFields(vector<u8>& out, const void* obj, const TypeDescriptor&);
void ReadFields(span<const u8> in, void* obj, const TypeDescriptor&);
```

`WriteFields` walks `Fields`, branches on each field's `FieldClass`, and appends the
leaf bytes; `ReadFields` reverses it into a default-constructed instance. Neither
knows any concrete component type — they prove the descriptors alone are enough to
serialize a component. This is the exact seam the deferred `.scene` cooker/loader
drops onto; no on-disk format, no `assetformat` touch here.

## Tests

`veng_unit` (`-L unit`):

- **Vocabulary:** `TypeIdOf<T>()` for each builtin leaf returns a stable, distinct
  id; its `FieldClass` is correct.
- **Descriptor shape:** each builtin's `Fields` has the expected names/offsets;
  `offsetof` round-trips (write a value through the descriptor offset, read it back
  through the typed member).
- **Generic round-trip:** populate a `Transform` (and a `Name`) with known values,
  `WriteFields` then `ReadFields` into a fresh instance, assert field-equal — driven
  **only** through the descriptor, never the typed struct.
- **Open extension:** register a fake game leaf `TypeId` + a component using it and
  round-trip it, proving the vocabulary extends without an engine change.

`include_hygiene`: add `Veng/Reflection/TypeId.h`,
`Veng/Reflection/FieldDescriptor.h` (and the `TypeDescriptor` field additions are
already covered via `TypeRegistry.h`).

## Acceptance

Clean build; `ctest -L unit` green; `include_hygiene` builds. Sample unchanged.
Commit: `Plan 03: reflection layer — TypeId vocabulary, field descriptors, generic
serialize round-trip`.
