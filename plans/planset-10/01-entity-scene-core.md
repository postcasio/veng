# Plan 01 — Entity + Scene core + `TypeRegistry` lifecycle

**Goal:** stand up the ECS core — a generational `Entity` handle, a `Scene` that
owns entities and **type-erased sparse-set** component pools, and a `TypeRegistry`
that records each registered type under its **stable, authored `TypeId`** (the same
`AssetId` discipline — hardcoded for engine types, tool-minted for game types) and
stores its **lifecycle** (default-construct, destruct, move-construct). Expose the
typed façade
`Add<T>`/`Remove<T>`/`Get<T>`/`TryGet<T>`/`Has<T>` over the erased pools. Pure CPU;
proven by unit tests and stale-handle death tests. **No sample change.**

The registry is **generic over any type, not just components** (the field-type
leaves and nested structs reflection later describes live in the same `TypeId`
space). This plan adds only the lifecycle slice; the `FieldClass`/`Fields`/editor-
metadata members and `VE_REFLECT` arrive in plan 03. A **component is simply a
reflected type a `Scene` keeps a pool for** — there is no separate component-id
space.

## Why this is its own plan

The storage model and the registry are the load-bearing mechanism everything else
hangs off: queries (02) iterate the pools, reflection (03) enriches the registry,
the sample (04) drives the typed API. Isolating it lets the type-erasure decision —
raw bytes manipulated through descriptor function pointers, because component types
are runtime-registered — be proven against a purpose-built test before any
component types or rendering exist.

## `Entity` — `engine/include/Veng/Scene/Entity.h`

```cpp
struct Entity
{
    u32 Index      = InvalidIndex;
    u32 Generation = 0;

    static constexpr u32 InvalidIndex = ~0u;
    static const Entity  Null;            // { InvalidIndex, 0 }

    bool IsNull() const { return Index == InvalidIndex; }
    bool operator==(const Entity&) const = default;
};
```

The generation distinguishes a live handle from one whose slot was reused after
`DestroyEntity`. Accessing a component on a stale `Entity` is API misuse → a fatal
`VE_ASSERT`, never silent UB. (Index+generation may be packed into a `u64` as an
impl detail; the public shape is the struct above.)

## `TypeRegistry` (lifecycle slice) — `engine/include/Veng/Reflection/TypeRegistry.h`

The registry owns one `TypeInfo` per registered type. This plan adds only the
**lifecycle** members; plan 03 adds `FieldClass`, the field descriptors, and the
editor-metadata slots.

```cpp
using TypeId = u64;                          // stable id, authored like AssetId
inline constexpr TypeId InvalidTypeId = 0;   // 0 is reserved; minted ids are non-zero

struct TypeInfo
{
    string  Name;          // logs / editor display only — never the persisted key
    usize   Size;
    usize   Align;
    void  (*DefaultConstruct)(void* dst);
    void  (*Destruct)(void* obj);
    void  (*MoveConstruct)(void* dst, void* src);  // swap-and-pop on remove
    TypeId  Id = InvalidTypeId;                     // the authored stable id
    // FieldClass Class; vector<FieldDescriptor> Fields;  // added in plan 03
};

// A type declares its stable id through a trait. This plan needs only the Id member;
// plan 03 enriches the same trait with Class/Fields and the VE_REFLECT macro that
// generates it. In this plan the id is declared by a minimal `VE_TYPE(T, 0x…ULL)`
// (or a hand specialisation in a test).
template <class T> struct VengReflect;        // { static constexpr TypeId Id = …; }

class TypeRegistry
{
public:
    // Records the TypeInfo under its authored Id (VengReflect<T>::Id); fatal-asserts
    // on a colliding Id. Plan 03 adds the fielded + trait-driven overloads.
    template <class T> TypeId Register(string name);
    template <class T> constexpr TypeId IdOf() const { return VengReflect<T>::Id; }

    const TypeInfo& Info(TypeId) const;
    bool            IsRegistered(TypeId) const;
    usize           Count() const;
};
```

`Register<T>` synthesises the three lifecycle thunks from `T` (`new (dst) T{}`,
`obj->~T()`, `new (dst) T{std::move(*src)}`) and records the `TypeInfo` under `T`'s
**authored stable `TypeId`** (a `0x…ULL` literal for engine types, `vengc
generate-id` for game types — exactly the `AssetId` discipline). Registering two types
with the same id is a **fatal collision assert**. `IdOf<T>()` is a **compile-time
constant** — it reads `VengReflect<T>::Id`, not a runtime-minted value — so there is
no per-`T` cache and no dependence on registration order or a shared registry
instance: the same literal compiles identically in the engine and in a `dlopen`ed
module. (This is why a compiler `type_hash` is rejected — not because runtime minting
is needed, but because an authored literal is both a constant *and* collision-safe,
which a hash is not.)

The registry is **engine-owned**: `Application` constructs it and exposes
`TypeRegistry& GetTypeRegistry()`, threading it into `Scene::Create` — the same
explicit-dependency discipline as `Context`/`AssetManager`/`TaskSystem`. No global.
Registration is **main-thread, startup-only** (engine builtins at `Application`
construction, a game's own types in `OnInitialize`) and must complete before any
`Scene` use; it is not safe to register concurrently with task-system workers
touching scenes.

## Type-erased sparse-set pool (impl, `engine/src/Scene/ComponentPool.*`)

One pool per type actually `Add`ed to an entity in a `Scene`, created lazily on first
`Add` and **keyed by `TypeId`** (`m_Pools[TypeId]` — a sparse array or map; there is
no dense component-id index). Classic sparse-set:

- `vector<u32> m_Sparse;` — entity index → dense slot (or sentinel).
- `vector<Entity> m_Dense;` — packed entity list (iteration order).
- a raw byte buffer of `Count * Info.Size` — the packed component data.

`Add` appends (placement-construct via `DefaultConstruct`, then the typed façade
move-assigns the caller's value). `Remove` is **swap-and-pop**: `MoveConstruct` the
last element into the removed slot, `Destruct` the tail, fix the sparse entries.
`Get` indexes dense via sparse. All byte math goes through `Info.Size`/`Align`.

## `Scene` — `engine/include/Veng/Scene/Scene.h`

```cpp
class Scene
{
public:
    static Unique<Scene> Create(TypeRegistry& registry);

    Entity CreateEntity();
    void   DestroyEntity(Entity);          // removes its components, recycles the slot
    bool   IsAlive(Entity) const;

    template <class T> T&   Add(Entity, T value = {});
    template <class T> void Remove(Entity);
    template <class T> T*   TryGet(Entity);          // nullptr if absent
    template <class T> T&   Get(Entity);             // asserts present
    template <class T> bool Has(Entity) const;
};
```

Entities use a generational free-list: `CreateEntity` reuses a freed index and
bumps its generation; `DestroyEntity` removes every component (walking the live
pools) and frees the slot. The templated members resolve `T → TypeId` via the
registry and forward to the erased pool (creating the pool on first `Add`). Every
`Entity`-taking call asserts `IsAlive`.

`Scene` is `Unique` (single owner, nothing holds a `Ref` to it).

## Tests

`veng_unit` (`-L unit`, driver-free):

- **Entity lifecycle:** create/destroy, index reuse bumps generation, `IsAlive`
  false for a destroyed handle, `Null` is never alive.
- **Component add/get/remove:** `Add`/`Get`/`Has`/`TryGet`/`Remove` round-trips;
  `TryGet` returns `nullptr` when absent.
- **Sparse-set swap-and-pop:** add N, remove from the middle, assert the survivors
  and their values are intact and densely packed (a non-trivially-movable component
  proves `MoveConstruct`/`Destruct` are called correctly — e.g. one wrapping a
  `string`).
- **DestroyEntity** removes all of an entity's components from every pool.
- **`TypeRegistry`:** `Register`/`IdOf`/`Info` round-trip; `IdOf<T>()` equals the
  authored `VengReflect<T>::Id` literal and is usable in a `constexpr` context;
  distinct types get distinct ids; a non-component type (e.g. a plain leaf struct)
  registers and gets a `TypeId` just like a component, proving the registry is
  generic.

`veng_death` (`-L death`): accessing `Get<T>` on a stale `Entity`, on an entity
lacking the component, and registering two types under a colliding `TypeId`, each
abort with the expected assert message.

`include_hygiene`: add `Veng/Scene/Entity.h`, `Veng/Scene/Scene.h`,
`Veng/Reflection/TypeRegistry.h` to the manifest.

## Acceptance

Clean build; `ctest -L unit` and `-L death` green; `include_hygiene` builds. No
behaviour change to the sample. Commit: `Plan 01: ECS core — Entity, Scene,
type-erased component pools, TypeRegistry lifecycle`.
