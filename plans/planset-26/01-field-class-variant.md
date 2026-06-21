# Plan 01 — Reflection variants: `FieldClass::Variant`

**Goal:** teach the reflection layer a tagged-union meta-kind. Add
`FieldClass::Variant`, a `Variant<Ts...>` value type, the `VE_VARIANT` authoring
macro, and the `TypeInfo` ops a generic walker needs to read/activate a variant's
member without naming its C++ type. No serializer, cooker, or editor work yet — this
plan is the type, the macro, and the registry contract those three (plans 02–04) build
on. Backed by a throwaway test variant and a unit test of the ops.

## Why this is its own plan

Every downstream consumer (serializer, cooker, inspector) switches on `FieldClass` and
reaches the active member through `TypeInfo`. Getting the variant's memory model, the
thunk signatures, and the macro right here means each consumer is a single new `case`.

## The `Variant<Ts...>` type — `engine/include/Veng/Reflection/Variant.h` (new)

A thin wrapper over `std::variant<std::monostate, Ts...>`. `monostate` is the empty
state (a default-constructed `Variant` holds no alternative), so a freshly-added
component or an omitted prefab field is "no shape", not "shape 0".

```cpp
/// @brief A reflected tagged union: holds at most one of the alternative types Ts.
///
/// Default-constructs empty (no active alternative). Reflection reaches the active
/// member through the type-erased ops VE_VARIANT records on this type's TypeInfo;
/// it never indexes the storage by offset. Each Ts must be a registered Struct-class
/// reflected type. std::variant supplies move/destruct, so a Variant member is a
/// normal poolable value.
template <class... Ts>
class Variant
{
public:
    /// @brief Constructs an empty variant (no active alternative).
    Variant() = default;

    /// @brief Returns the active alternative's TypeId, or InvalidTypeId when empty.
    [[nodiscard]] TypeId ActiveType() const;

    /// @brief Returns true when an alternative is active.
    [[nodiscard]] bool HasValue() const { return m_Storage.index() != 0; }

    /// @brief Activates the alternative whose TypeId is `id`, default-constructed.
    /// @return Pointer to the activated member's storage, or nullptr if `id` is
    ///         none of Ts (the caller treats that as a skip / located error).
    void* SetActive(TypeId id);

    /// @brief Resets to the empty (monostate) state, destructing any active alternative.
    void Clear() { m_Storage.template emplace<std::monostate>(); }

    /// @brief Returns the active member's storage, or nullptr when empty.
    [[nodiscard]] void* ActivePtr();
    [[nodiscard]] const void* ActivePtr() const;

    /// @brief The TypeIds of the alternatives, in declaration order.
    [[nodiscard]] static std::span<const TypeId> Alternatives();

    /// @brief Registers every alternative type into `registry` (the dependency recursion).
    static void RegisterAlternatives(TypeRegistry& registry) { (registry.template Register<Ts>(), ...); }

private:
    std::variant<std::monostate, Ts...> m_Storage;
};
```

`ActiveType`/`SetActive`/`ActivePtr` are implemented with a compile-time fold over
`Ts...` (`TypeIdOf<Ts>()` per alternative): `SetActive` matches `id` against each
`TypeIdOf<Ts>()` and `m_Storage.emplace<Ts>()` on a hit; `ActiveType`/`ActivePtr`
`std::visit` the storage (monostate → `InvalidTypeId`/`nullptr`). `Clear` re-emplaces
`monostate` (destructing the active alternative). `Alternatives()` returns a
`static constexpr std::array<TypeId, sizeof...(Ts)>{ TypeIdOf<Ts>()... }`, and
`RegisterAlternatives` is the pack-fold the `VE_VARIANT` macro forwards `RegisterDependencies`
to — the macro never needs to see `Ts...` itself, since the type owns its pack.

These public member functions are the implementation detail behind the **type-erased
thunks** the registry actually calls — kept as members so they are unit-testable
directly and so the thunks are one-liners.

## `TypeInfo` variant ops — `TypeRegistry.h`

`TypeInfo` carries lifecycle thunks and `Fields`. Add an optional, variant-only op set
(all `nullptr`/empty for non-variant types):

```cpp
/// @brief Variant-only: the alternative TypeIds, in declaration order. Empty for non-variants.
vector<TypeId> VariantAlternatives;
/// @brief Variant-only: returns the active alternative's TypeId (InvalidTypeId = empty).
TypeId (*VariantActiveType)(const void*) = nullptr;
/// @brief Variant-only: returns the active member's storage, or nullptr when empty.
void* (*VariantActivePtr)(void*) = nullptr;
const void* (*VariantActivePtrConst)(const void*) = nullptr;
/// @brief Variant-only: activates `id`'s alternative (default-constructed); nullptr if not an alternative.
void* (*VariantSetActive)(void*, TypeId) = nullptr;
/// @brief Variant-only: resets the variant to empty, destructing any active alternative.
void (*VariantClear)(void*) = nullptr;
```

A walker that has a `FieldClass::Variant` field reads these off
`registry.Info(field.Type)` — symmetric to how a `Struct` field reads `Fields` off
`registry.Info(field.Type)`.

## The `VE_VARIANT` macro — `Reflect.h`

Specialises `VengReflect<Type>` (where `Type` is a `Variant<Ts...>` or an alias of one)
the way `VE_TYPE`/`VE_LEAF` specialise leaves. The macro stays **two-arg** —
`VE_VARIANT(Type, TypeIdLiteral)` — because `Type` already *is* `Variant<Ts...>`, so the
pack is reachable through the type's own static members (`Type::Alternatives()`,
`Type::RegisterAlternatives`); the macro never needs the alternatives spelled out a
second time. Each generated thunk is a one-line cast-and-forward to a `Variant` member,
so the macro can emit them knowing only `Type`:

```cpp
#define VE_VARIANT(Type, TypeIdLiteral)                                                       \
    template <> struct ::Veng::VengReflect<Type> {                                            \
        static constexpr ::Veng::TypeId Id = (TypeIdLiteral);                                 \
        static constexpr ::Veng::FieldClass Class = ::Veng::FieldClass::Variant;              \
        static ::Veng::string Name() { return #Type; }                                        \
        static ::Veng::vector<::Veng::FieldDescriptor> Fields() { return {}; }                \
        static ::Veng::TypeId ActiveType(const void* p)                                       \
            { return static_cast<const Type*>(p)->ActiveType(); }                             \
        static void* ActivePtr(void* p) { return static_cast<Type*>(p)->ActivePtr(); }        \
        static const void* ActivePtrConst(const void* p)                                      \
            { return static_cast<const Type*>(p)->ActivePtr(); }                              \
        static void* SetActive(void* p, ::Veng::TypeId id)                                    \
            { return static_cast<Type*>(p)->SetActive(id); }                                  \
        static void Clear(void* p) { static_cast<Type*>(p)->Clear(); }                        \
        static ::Veng::vector<::Veng::TypeId> Alternatives()                                  \
            { const auto s = Type::Alternatives(); return {s.begin(), s.end()}; }             \
        static void RegisterDependencies(::Veng::TypeRegistry& r) { Type::RegisterAlternatives(r); } \
    }
```

The thunks and the alternatives list reach `TypeInfo` through `RegisterImpl<T>`, which is
already a `template <class T>` that builds the `TypeInfo` locally (synthesising the
lifecycle thunks from `T`) before `m_Types.emplace`. Its signature — `(id, name, cls,
fields)` — carries no variant slot, so add an `if constexpr` block that fills the variant
fields off `VengReflect<T>` before the emplace, alongside where it sets
`MoveConstruct`/`Destruct`:

```cpp
// inside RegisterImpl<T>, after info.Fields = …, before m_Types.emplace:
if constexpr (VengReflect<T>::Class == FieldClass::Variant)
{
    info.VariantActiveType     = &VengReflect<T>::ActiveType;
    info.VariantActivePtr      = &VengReflect<T>::ActivePtr;
    info.VariantActivePtrConst = &VengReflect<T>::ActivePtrConst;
    info.VariantSetActive      = &VengReflect<T>::SetActive;
    info.VariantClear          = &VengReflect<T>::Clear;
    info.VariantAlternatives   = VengReflect<T>::Alternatives();
}
```

`Register<T>()` already calls `RegisterDependencies` after `RegisterImpl`, so registering a
`PrimitiveComponent` auto-registers its `Variant`, and the variant's
`RegisterDependencies` → `Variant<Ts...>::RegisterAlternatives` auto-registers each
alternative — same no-ordering guarantee a struct field gets.

`FieldClassOf<Variant<Ts...>>()` must yield `FieldClass::Variant` (a partial
specialisation of the `FieldClassOf` trait on `Variant<...>`), so a `VE_FIELD` naming a
`Variant` member picks the right class with no per-field annotation.

## Tests — `tests/unit/reflection_variant.cpp` (new)

Define a throwaway pair of reflected structs (`TestA { i32 X; }`, `TestB { f32 Y; }`)
and a `Variant<TestA, TestB>` aliased through `VE_VARIANT`. Register them into a local
`TypeRegistry` and assert through the `TypeInfo` thunks:

- A default `Variant` reports `ActiveType() == InvalidTypeId`, `ActivePtr() == nullptr`.
- `SetActive(TypeIdOf<TestB>())` returns non-null, `ActiveType()` becomes `TestB`'s id,
  and writing through the returned pointer mutates the variant.
- `SetActive` of an id not among the alternatives returns `nullptr` and leaves the prior
  state untouched.
- `VariantClear` over a populated variant returns it to `ActiveType() == InvalidTypeId`.
- `Alternatives()` lists exactly `{ TestA, TestB }` in order.
- The `MoveConstruct`/`Destruct` thunks recorded for the variant round-trip a populated
  value (move it, read it back).

Add `Variant.h` to the `include_hygiene` header set.

## Acceptance

- Clean build; `ctest -L unit` green including the new variant test.
- `Variant<Ts...>` default-constructs empty; the five `TypeInfo` thunks
  (`VariantActiveType`/`VariantActivePtr`/`VariantActivePtrConst`/`VariantSetActive`/`VariantClear`)
  and `VariantAlternatives` are populated by the `Register<T>()` variant branch for a
  `VE_VARIANT` type and null/empty otherwise.
- `FieldClassOf<Variant<...>>()` is `Variant`; `Register<PrimitiveComponent-like>()`
  transitively registers the variant and its alternatives with no manual ordering.
- `include_hygiene` builds (only `<variant>` added beyond existing public reflection
  headers).
