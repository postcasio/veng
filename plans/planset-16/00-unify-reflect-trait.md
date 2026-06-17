# Plan 00 — Unify the reflection identity trait

**Goal:** collapse `ReflectLeaf<T>` into a single `VengReflect<T>` trait that every reflected
type specialises with a uniform member set, delete the `Detail::IsReflectLeaf` SFINAE and the
`TypeRegistry::EnsureLeaf<T>()` path, and make the two compile-time dispatchers
(`TypeIdOf`/`FieldClassOf`) direct member reads. Behaviour-preserving: no `TypeId`,
`FieldClass`, or on-disk change.

## The target shape

A single trait, specialised uniformly by three authoring macros. Every specialisation provides
exactly these members:

```cpp
template <class T>
struct VengReflect
{
    static constexpr TypeId      Id    = /* authored 0x…ULL */;
    static constexpr FieldClass  Class = /* the type's meta-kind */;
    static string                Name();                         // logs/editor display
    static vector<FieldDescriptor> Fields();                     // {} for non-struct/id-only
    static void RegisterDependencies(TypeRegistry&);             // no-op for non-struct/id-only
};
```

- **Leaves** (scalars, vectors, `quat`, `mat4`, `string`, `AssetHandle<…>`, `Entity`, game
  enums) — authored via the new `VE_LEAF`; `Class` is the leaf's kind, `Fields()` = `{}`,
  `RegisterDependencies` is a no-op.
- **Id-only structs/components** — authored via `VE_TYPE`; `Class = Struct`, `Name()` =
  `#Type`, `Fields()` = `{}`, no-op `RegisterDependencies`.
- **Fielded structs** — authored via `VE_REFLECT`/`VE_FIELD`; `Class = Struct`, `Name()` =
  `#Type`, `Fields()`/`RegisterDependencies()` replay the shared `Describe` body (unchanged).

With the member set uniform, **no SFINAE is needed anywhere** to choose where to read from.

## `engine/include/Veng/Reflection/TypeId.h`

- **Add the includes** that let `VE_LEAF` live here: `#include <Veng/Reflection/ReflectionTypes.h>`
  (the extracted `TypeId`/`FieldClass` definitions) and `#include <Veng/Reflection/FieldDescriptor.h>`
  (now acyclic — see the `VE_LEAF` section). The dispatchers themselves only need the `VengReflect`
  forward declaration, but the inline leaf `Fields()` bodies need `FieldDescriptor` complete.
- **Delete** the `ReflectLeaf` forward declaration and the `Detail::IsReflectLeaf` `void_t`
  detector.
- **`TypeIdOf<T>()`** → `return VengReflect<T>::Id;` (drop the `if constexpr`).
- **`FieldClassOf<T>()`** → `return VengReflect<T>::Class;` (drop the `if constexpr`; a struct's
  `Class` is `FieldClass::Struct`, authored by the macro, so the old "structs are always
  Struct" branch is now just the member value).
- **Reauthor the builtin leaf vocabulary** from `template <> struct ReflectLeaf<X> { Id; Class;
  }` to `VE_LEAF(X, 0x…ULL, FieldClass::Kind)`, **carrying every id verbatim**:

  | Type | Id (unchanged) | Class |
  |---|---|---|
  | `bool` | `0x283EDB5B266A27ED` | `Scalar` |
  | `f32` | `0x4AF0D89664A476FB` | `Scalar` |
  | `i32` | `0xE4A543818EB46182` | `Scalar` |
  | `u32` | `0x6AD25BC2BE1A5D65` | `Scalar` |
  | `u64` | `0x94AB42FEF4E32D87` | `Scalar` |
  | `vec2` | `0xB9A6A5F871901160` | `Vector` |
  | `vec3` | `0xA9A78263CAA293E7` | `Vector` |
  | `vec4` | `0xA936BFC80085F684` | `Vector` |
  | `quat` | `0xFD92495C91720213` | `Quaternion` |
  | `mat4` | `0x8ABB4818B9CC633E` | `Matrix` |
  | `string` | `0x2E46B7DE1FFC7DFC` | `String` |
  | `AssetHandle<Texture>` | `0x612EE7E69BE7B848` | `AssetHandle` |
  | `AssetHandle<Mesh>` | `0x1CD2C85C50AFC9E0` | `AssetHandle` |
  | `AssetHandle<Material>` | `0x3992D11EB4362B4C` | `AssetHandle` |
  | `Entity` | `0x448BDF481E27075E` | `Reference` |

  (`AssetHandle<Texture>` has no top-level comma, so it is a single macro argument; the
  `FieldClass::Kind` argument likewise carries none.)
- **Rewrite the trait doc comment** to describe the single `VengReflect<T>` and the three
  authoring macros — present tense, no "was two traits" narrative.

## The `VE_LEAF` macro (`TypeId.h`, beside the builtin leaves)

```cpp
#define VE_LEAF(Type, TypeIdLiteral, FieldClassValue)                          \
    template <>                                                                \
    struct ::Veng::VengReflect<Type>                                           \
    {                                                                          \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                  \
        static constexpr ::Veng::FieldClass Class = (FieldClassValue);         \
        static ::Veng::string Name() { return #Type; }                         \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; } \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}             \
    }
```

`VE_LEAF` and the builtin leaf specialisations stay in `TypeId.h` — the foundational reflection
header every leaf-using TU already includes (`TypeRegistry.h`, `PrefabImporter.cpp`, the editor
node-graph tests). Keeping them there avoids scattering new includes across those consumers.

**Resolve the include cycle first.** `VE_LEAF`'s `Fields()` returns `vector<FieldDescriptor>`,
which odr-uses (so requires the **complete**) `FieldDescriptor` — a forward declaration will not
do, because the uniform `Register<T>()` calls `Fields()` for every leaf. But `TypeId.h` cannot
`#include <Veng/Reflection/FieldDescriptor.h>` today: `FieldDescriptor.h` already includes
`TypeId.h` (for the `TypeId` type and `FieldClass` enum), so the include would be circular and
leave `FieldDescriptor` incomplete in whichever header is processed first.

Break the cycle by extracting the two primitive definitions `TypeId` and `FieldClass` into a new
leaf header `engine/include/Veng/Reflection/ReflectionTypes.h`:

- `ReflectionTypes.h` — defines `TypeId` and the `FieldClass` enum. Includes only `Veng.h`. Has
  no dependency on `FieldDescriptor.h` or the rest of the reflection layer.
- `FieldDescriptor.h` — includes `ReflectionTypes.h` instead of `TypeId.h` (it only needs
  `TypeId` + `FieldClass` for its struct members).
- `TypeId.h` — includes `ReflectionTypes.h` **and** `FieldDescriptor.h` (now acyclic), then
  defines `VengReflect`, the `TypeIdOf`/`FieldClassOf` dispatchers, `VE_LEAF`, and the builtin
  leaf vocabulary as before. It re-exposes `TypeId`/`FieldClass` transitively, so **every
  existing `#include <Veng/Reflection/TypeId.h>` consumer is unchanged** — no scattered include
  additions, no public-surface move.

`VE_TYPE` (in `TypeRegistry.h`) and `VE_REFLECT` (in `Reflect.h`) stay where they are; both
those headers already pull in `FieldDescriptor.h`, so their macros' `Fields()` bodies compile
unchanged.

## `engine/include/Veng/Reflection/TypeRegistry.h`

- **Delete `EnsureLeaf<T>()`.** Its job — record a leaf's lifecycle/size/class under its id with
  an empty field list — is now done by the uniform `Register<T>()` (a leaf's `Fields()` is
  `{}`, so it records nothing extra and recurses into nothing).
- **`Register<T>()` (zero-arg)** stays structurally the same but is now uniform across leaf and
  struct: read `VengReflect<T>::Id`, `contains()`-guard, `RegisterImpl<T>(id,
  VengReflect<T>::Name(), VengReflect<T>::Class, VengReflect<T>::Fields())`, then
  `VengReflect<T>::RegisterDependencies(*this)`. For a leaf, `Name()` is the type name (no
  longer empty), `Fields()` is `{}`, and `RegisterDependencies` is a no-op — same recorded
  `TypeInfo` as the old `EnsureLeaf` except the name is populated.
  - **Concrete edit:** the existing call at `TypeRegistry.h:80` spells the class as a function —
    `VengReflect<T>::Class()` — because `Class` was a function on structs. With `Class` now a
    `constexpr` member everywhere, drop the parens: `VengReflect<T>::Class`. (Currently this line
    only ever ran for the struct path; it is now the single path for leaf + struct.) It also now
    reads `VengReflect<T>::Name()`/`Fields()` for leaves, which the old struct-only body did
    not — those were `EnsureLeaf`'s job.
- **`Register<T>(name)` and `Register<T>(name, cls, fields)`** overloads are **kept** (the
  explicit escape hatches the ECS tests use). `Register<T>(name)` reads `VengReflect<T>::Id`
  and forces `FieldClass::Struct` + empty fields exactly as today.
- Update the `EnsureLeaf` doc comment removal and the `Register<T>()` comment to the single-path
  story.

## `engine/include/Veng/Reflection/Reflect.h`

- **`VE_REFLECT`**: change `static constexpr ::Veng::FieldClass Class() { return …Struct; }` to
  `static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Struct;` (member, not
  function). `Name()`/`Fields()`/`RegisterDependencies()` are unchanged.
- **`Detail::DependencyRegistrar::Field_`**: collapse the `if constexpr (FieldClassOf<Field>()
  == Struct) Register<Field>() else EnsureLeaf<Field>()` to a single
  `Registry.Register<Field>();` for **every** field. A leaf field's `Register<Field>()` records
  it (empty fields, no recursion); a struct field's recurses. The `FieldCollector` sink is
  unchanged.
- Update the `DependencyRegistrar` doc comment to the single-call story.

## `engine/include/Veng/Reflection/TypeRegistry.h` — `VE_TYPE`

Change `VE_TYPE` to emit the full uniform member set:

```cpp
#define VE_TYPE(Type, TypeIdLiteral)                                           \
    template <>                                                                \
    struct ::Veng::VengReflect<Type>                                           \
    {                                                                          \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                  \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Struct;\
        static ::Veng::string Name() { return #Type; }                         \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; } \
        static void RegisterDependencies(::Veng::TypeRegistry&) {}             \
    }
```

This keeps `VE_TYPE`'s one-line authoring surface while making an id-only type flow through the
same uniform `Register<T>()`. Drop the "use it for a game leaf" phrasing in its comment — a game
leaf/enum now uses `VE_LEAF`; `VE_TYPE` is for a **fieldless struct/component**.

## `engine/src/Reflection/Serialize.cpp`

**No logic change.** The serializer reads each leaf's `Size` off `registry.Info(field.Type).Size`,
which the uniform `Register<T>()` records identically. Touch only the "Writes a leaf/struct field's
value bytes" comment if it references the old two-trait split (it does not appear to). Verify the
prefab/material round-trip is byte-identical (the `cooker` suite + `reflection.cpp` cover this).

## `cooker/src/Importers/PrefabImporter.cpp`

One comment edit: `// TypeId (the stable ids on ReflectLeaf<AssetHandle<T>>)` →
`// TypeId (the stable ids on VengReflect<AssetHandle<T>>)`. No code change — the
`TypeIdOf<AssetHandle<…>>()` / `TypeIdOf<f32>()` calls resolve to the same values through the
unified trait.

## Tests

- **`tests/unit/reflection.cpp`** — three concrete edits:
  - **Line 102** — the one game-side leaf spec migrates:
    ```cpp
    template <> struct Veng::ReflectLeaf<Team>
    { static constexpr TypeId Id = 0x11AA22BB33CC44DDULL; static constexpr FieldClass Class = FieldClass::Enum; };
    ```
    becomes
    ```cpp
    VE_LEAF(Team, 0x11AA22BB33CC44DDULL, Veng::FieldClass::Enum);
    ```
    The asserted id/class are identical.
  - **Line 151** — `CHECK(VengReflect<Transform>::Class() == FieldClass::Struct)` drops the
    parens: `VengReflect<Transform>::Class` (now a member). This is the only `::Class()` call
    site in the tests.
  - **Line 387** — the comment "its id and class come entirely from the game's
    `ReflectLeaf<Team>`" updates to name the `VE_LEAF`-authored `VengReflect<Team>` (provenance
    rename, no code change).
  - **Pin the name improvement (non-optional).** Add one assert next to the existing leaf
    id/class checks that a builtin leaf now carries a real name —
    `CHECK(registry.Info(TypeIdOf<f32>()).Name == "f32");` — so the "leaves gain a real
    `TypeInfo.Name`" behaviour (decision 2) is covered rather than latent. (It is the single
    intentional observable change; everything else is byte-identical, so it earns a test.)
- **`tests/unit/scene_ecs.cpp`** — `VengReflect<Position>::Id` read is unchanged (`Position` is
  `VE_TYPE`-authored); these tests deliberately keep using the `Register<T>("name")` string
  overload (they exercise ECS semantics, not the trait-read path), which is retained. No edit
  beyond confirming they compile.
- **`tests/unit/scene_queries.cpp`, `tests/death/death_main.cpp`** — `VE_TYPE` users; no edit
  beyond confirming they still compile/run (the collision-assert death test still fires on the
  duplicate id).
- Run the **full** suite: `veng_unit`, `veng_death`, `veng_cooker`, and the `gpu` band where a
  device is present. A reflection-encoding change would surface in `veng_cooker` (prefab cook
  round-trip) and `reflection.cpp`.

## `include_hygiene`

The only new include edges are within the reflection layer: `TypeId.h` now includes the new
`ReflectionTypes.h` + `FieldDescriptor.h`, and `FieldDescriptor.h` includes `ReflectionTypes.h`
instead of `TypeId.h`. All three are pure-data, backend-free public headers (no `vk::`/VMA/GLFW),
so the merge pulls in nothing backend-side. `ReflectionTypes.h` must be added to the public-header
set `tests/include_hygiene.cpp` compiles (it is a new public header). Confirm `veng_include_hygiene`
still builds.

## Acceptance

`ReflectLeaf<T>`, `Detail::IsReflectLeaf`, and `TypeRegistry::EnsureLeaf<T>()` no longer exist;
`VengReflect<T>` is the sole identity trait, authored by `VE_LEAF`/`VE_TYPE`/`VE_REFLECT` with a
uniform member set; `TypeIdOf`/`FieldClassOf` are direct reads with no `if constexpr`; the
builtin leaf ids are carried over verbatim; `Class` is a `constexpr` member in every macro. Clean
build (default + `build-debug`), full `ctest` green (`veng_unit`/`veng_death`/`veng_cooker`, and
the `gpu`/`validation` bands where a device is present), `include_hygiene` green, smoke PPM
correct size + exit 0, and the prefab/material on-disk encoding byte-identical. Commit:
`Plan 00: unify the reflection identity trait — single VengReflect<T>, VE_LEAF, drop EnsureLeaf`.
