# Plan 01 — Entity + Scene core + `TypeRegistry` lifecycle

**Goal:** stand up the ECS core — a generational `Entity` handle, a `Scene` that
owns entities and **type-erased sparse-set** component pools, and a `TypeRegistry`
that mints a `ComponentId` per registered component type and stores its
**lifecycle** (default-construct, destruct, move-construct). Expose the typed
façade `Add<T>`/`Remove<T>`/`Get<T>`/`TryGet<T>`/`Has<T>` over the erased pools.
Pure CPU; proven by unit tests and stale-handle death tests. **No sample change.**

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

The registry owns one `TypeDescriptor` per registered component type. This plan
adds only the **lifecycle** members; plan 03 adds the field descriptors.

```cpp
using ComponentId = u32;
inline constexpr ComponentId InvalidComponentId = ~0u;

struct TypeDescriptor
{
    string      Name;
    usize       Size;
    usize       Align;
    void      (*DefaultConstruct)(void* dst);
    void      (*Destruct)(void* obj);
    void      (*MoveConstruct)(void* dst, void* src);  // swap-and-pop on remove
    ComponentId Id = InvalidComponentId;               // assigned by the registry
    // vector<FieldDescriptor> Fields;                 // added in plan 03
};

class TypeRegistry
{
public:
    template <class T> ComponentId Register(string name);  // synthesises lifecycle from T
    template <class T> ComponentId IdOf() const;           // cached per T; asserts if unregistered

    const TypeDescriptor& Descriptor(ComponentId) const;
    usize                 Count() const;
};
```

`Register<T>` synthesises the three lifecycle thunks from `T` (`new (dst) T{}`,
`obj->~T()`, `new (dst) T{std::move(*src)}`) and mints the next `ComponentId`.
`IdOf<T>()` caches the id in a function-local `static` keyed by `T` so the typed
façade resolves `T → ComponentId` once. (The cache is per-`T`, not per-registry —
veng is single-`TypeRegistry`; `IdOf` asserts the type was registered.)

The registry is **engine-owned**: `Application` constructs it and exposes
`TypeRegistry& GetTypeRegistry()`, threading it into `Scene::Create` — the same
explicit-dependency discipline as `Context`/`AssetManager`/`TaskSystem`. No global.

## Type-erased sparse-set pool (impl, `engine/src/Scene/ComponentPool.*`)

One pool per registered component type actually used in a `Scene`, created lazily on
first `Add`. Classic sparse-set:

- `vector<u32> m_Sparse;` — entity index → dense slot (or sentinel).
- `vector<Entity> m_Dense;` — packed entity list (iteration order).
- a raw byte buffer of `Count * Descriptor.Size` — the packed component data.

`Add` appends (placement-construct via `DefaultConstruct`, then the typed façade
move-assigns the caller's value). `Remove` is **swap-and-pop**: `MoveConstruct` the
last element into the removed slot, `Destruct` the tail, fix the sparse entries.
`Get` indexes dense via sparse. All byte math goes through `Descriptor.Size`/`Align`.

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
pools) and frees the slot. The templated members resolve `T → ComponentId` via the
registry and forward to the erased pool. Every `Entity`-taking call asserts
`IsAlive`.

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
- **`TypeRegistry`:** `Register`/`IdOf`/`Descriptor` round-trip; distinct types get
  distinct ids.

`veng_death` (`-L death`): accessing `Get<T>` on a stale `Entity`, and on an entity
lacking the component, each abort with the expected assert message.

`include_hygiene`: add `Veng/Scene/Entity.h`, `Veng/Scene/Scene.h`,
`Veng/Reflection/TypeRegistry.h` to the manifest.

## Acceptance

Clean build; `ctest -L unit` and `-L death` green; `include_hygiene` builds. No
behaviour change to the sample. Commit: `Plan 01: ECS core — Entity, Scene,
type-erased component pools, TypeRegistry lifecycle`.
