# Plan 01 — The spawn-resolve thunk seam

**Goal:** the reusable mechanism. A component can declare a **resolver** —
`void (component, Scene&, Entity, AssetManager&)` — that `Prefab::SpawnInto` fires
automatically after the component is populated, and that any runtime path can fire
through one public `ResolveComponents(Scene&, Entity, AssetManager&)`. This plan
ships **no** primitive code; it is the generic seam the rest of the planset rides.
It is independently unit-testable with a throwaway, device-free resolver.

## Why this is its own plan

The thunk is the deliverable that outlives the primitive. Landing it first — with
its own test, decoupled from any GPU resource — fixes the contract every later plan
and any future resolve-time component depends on, and keeps the mechanism's
correctness (the post-populate pass, the fetch-fresh-by-`TypeId` rule, the layering)
provable without a device.

## `TypeInfo::SpawnResolve` — `engine/include/Veng/Reflection/TypeRegistry.h`

One optional function pointer beside the existing variant thunks, default null:

```cpp
// Forward declarations — no upward include; the typedef names these as opaque refs.
namespace Veng { class Scene; class AssetManager; struct Entity; }

struct TypeInfo
{
    // … existing members, including the Variant* thunks …

    /// @brief Optional: fired by Prefab::SpawnInto (and ResolveComponents) after the
    ///        component is populated, to generate/assign derived resources. Null for a
    ///        component that declares no resolver.
    ///
    /// The erased form the registry stores; authors write a typed resolver
    /// (`void(T&, Scene&, Entity, AssetManager&)`) and VE_RESOLVE generates the
    /// void*→T* trampoline assigned here.
    void (*SpawnResolve)(void* component, Scene& scene, Entity entity, AssetManager& manager)
        = nullptr;
};
```

The forward declarations are the **only** addition to this header — no new include,
so `include_hygiene` stays green and `Reflection` gains no upward include (decision
2 in the planset README). A function-pointer *typedef* needs only declared, not
complete, parameter types; the concrete resolvers live where `Scene`/`AssetManager`
are complete. The parameter set `(void*, Scene&, Entity, AssetManager&)` is
**frozen**: a resolver reaches anything further through `Scene`/`AssetManager`
(`manager.GetContext()`, `manager.GetTasks()`), never by adding a parameter — keeping
this the one forward-decl compromise the reflection core carries.

## The opt-in authoring seam — typed resolver, erased thunk

An author **never sees `void*`.** A component declares a fully-typed resolver — its
own type spelled out, like the lifecycle thunks' internal `static_cast<T*>` — and the
macro generates the erasing trampoline at the authoring site:

```cpp
// Authored next to the component; defined in a Scene/Asset-layer .cpp.
void ResolveThing(ThingComponent& thing, Scene& scene, Entity entity, AssetManager& manager);

VE_RESOLVE(ThingComponent, ResolveThing);
```

`VE_RESOLVE(Type, Fn)` specialises a `VengResolver<Type>` trait whose static `Thunk`
does the one cast and forwards to the typed `Fn`:

```cpp
#define VE_RESOLVE(Type, Fn)                                                       \
    template <> struct VengResolver<Type> {                                        \
        static void Thunk(void* c, Veng::Scene& s, Veng::Entity e, Veng::AssetManager& m) \
        { Fn(*static_cast<Type*>(c), s, e, m); }                                    \
    };
```

The trampoline is generated **in the component's translation unit**, where `Type`,
`Scene`, `Entity`, and `AssetManager` are complete — not in `RegisterImpl`, which
would need `Entity` complete (an `Entity.h` include) to form the typed call. So the
typed signature and the forward-decl-only Reflection layer both hold.

`RegisterImpl<T>` wires it with one more `if constexpr`, parallel to the variant
branch — a plain function-pointer assignment needing no completeness:

```cpp
if constexpr (HasSpawnResolver<T>)   // VengResolver<T> is specialised
{
    info.SpawnResolve = &VengResolver<T>::Thunk;
}
```

`HasSpawnResolver<T>` detects the `VengResolver<T>` specialisation (e.g. `requires {
&VengResolver<T>::Thunk; }`); a type without a `VE_RESOLVE` leaves `SpawnResolve`
null. The typed resolver `Fn` is *declared* where the component is declared (its
signature needs only forward-declared `Scene`/`AssetManager` and a complete `Entity`)
and *defined* in a `.cpp` in the Scene/Asset layer — so the component header pulls no
`Scene`/`AssetManager` include.

**The `VE_RESOLVE` specialisation must be visible at every `Register<T>()` site** —
it travels with the component's header, beside its `VE_REFLECT`. If it is not in
scope where `Register<T>()` is instantiated, `HasSpawnResolver<T>` detects nothing,
`SpawnResolve` stays null, and the resolver **silently never fires, with no error** —
the one way this seam can fail quietly.

## `Scene::TryGetComponent(Entity, TypeId) → void*`

The resolve pass must fetch a component's storage **fresh by `TypeId` at fire time**
(a resolver may `Add` a `MeshRenderer`, and a held pool pointer can dangle across an
intervening pool growth). `Scene` already has the private erased `TryGetRaw(Entity,
TypeId)`; expose a thin public wrapper:

```cpp
/// @brief Type-erased component fetch: the storage for `id` on `entity`, or nullptr
///        if absent. The TypeId sibling of TryGet<T>; used by the spawn-resolve pass.
[[nodiscard]] void* TryGetComponent(Entity entity, TypeId id) { return TryGetRaw(entity, id); }
```

## `SpawnInto`'s post-populate resolve pass — `engine/src/Asset/Prefab.cpp`

The existing populate loop is unchanged: create every entity, then `ReadFields` +
the field-rehydration `Resolve` walk per component. After that loop completes, add a
**second** pass over the spawned entities that fires each resolver-bearing
component's `SpawnResolve`:

```cpp
// 2b. Run each spawned component's resolver, now that every entity and component
//     exists. A resolver may add a component (a MeshRenderer), so fetch the
//     component storage fresh by TypeId rather than holding a pool pointer.
for (usize i = 0; i < m_Entities.size(); ++i)
{
    const Entity entity = spawned[i];
    for (const Component& component : m_Entities[i].Components)
    {
        const TypeInfo& typeInfo = registry.Info(component.Type);
        if (typeInfo.SpawnResolve != nullptr)
        {
            void* slot = scene.TryGetComponent(entity, component.Type);
            typeInfo.SpawnResolve(slot, scene, entity, manager);
        }
    }
}
```

It iterates the **prefab's** known component list (not `ForEachComponent`), so a
resolver adding a `MeshRenderer` is not a structural change to anything being
iterated. The pass runs before the hierarchy-link rebuild (step 3) — a resolver
adding a renderable does not affect parent edges.

A component a resolver *adds* is **not** itself resolved in this pass (the loop walks
only the prefab's authored components); a resolver needing its own output resolved
must do so inline. Chaining resolvers — a resolver whose added component itself bears
a `SpawnResolve` — is out of scope (there is no topological pass).

## `ResolveComponents(Scene&, Entity, AssetManager&)` — the runtime entry point

The same fire logic for a **single** entity, public, for components created outside
a spawn (the editor's add/edit, game code). It lives beside the resolve declarations
(`Veng/Scene/Resolve.h` — the planset-26 `PrimitiveResolve.h` renamed; it now hosts
the generic `ResolveComponents` alongside the concrete resolvers and, after plan 02,
`CreatePrimitiveMesh`/`BuildShapeMeshData`):

```cpp
/// @brief Fires every resolver-bearing component on `entity`, generating/assigning
///        its derived resources. Idempotent in effect for an unchanged recipe (it
///        rebuilds and reassigns); call it after adding or editing a component that
///        carries a SpawnResolve. Prefab::SpawnInto runs this per spawned entity.
void ResolveComponents(Scene& scene, Entity entity, AssetManager& manager);
```

It enumerates the entity's components (via `ForEachComponent`, collecting
`(TypeId, slot)` first, then firing **outside** the walk so a resolver's `Add` is not
a structural change mid-iteration) and fires each non-null `SpawnResolve`.
`SpawnInto`'s pass and this share one helper.

## Tests

The mechanism splits across two bands: firing a resolver needs an `AssetManager`,
whose only constructor takes a `Renderer::Context&` (a device) — so even a resolver
whose *body* touches no GPU cannot be fired without one. The wiring is provable
device-free; the firing is not.

**Device-free wiring — `tests/unit` (`ctest -L unit`).** A throwaway `struct
ResolveProbe { i32 Marker = 0; }` with a `VE_RESOLVE`'d resolver that sets `Marker =
42` proves the *registration* wiring with no device:

- **Opt-in wires the pointer.** `Register<ResolveProbe>()` leaves
  `TypeInfo::SpawnResolve` non-null; `HasSpawnResolver<ResolveProbe>` is true.
- **Opt-out is null.** A component without `VE_RESOLVE` has `SpawnResolve == null`.
- **`include_hygiene`** compiles `TypeRegistry.h` with only the forward declarations
  added — green.

**Firing — `tests/gpu` (beside `prefab_spawn.cpp`).** The same `ResolveProbe` (its
resolver, in one case, `scene.Add`s a second test component to prove an `Add` from
inside a resolver is safe), driven through a real `AssetManager` on the GPU fixture:

- **Spawn fires it.** A hand-built prefab carrying `ResolveProbe` (a `WriteFields`
  record, as the existing prefab tests do), spawned via `SpawnInto`, has
  `Marker == 42` on the spawned entity.
- **`ResolveComponents` fires it.** Add `ResolveProbe` to a live entity, call
  `ResolveComponents`, assert `Marker == 42`.
- **Opt-out does nothing.** A prefab of non-resolver components spawns with no extra
  work and touches no device — the `prefab_spawn` "no device touched" property holds
  for a non-resolver prefab. (The resolver body sets a field, so this firing test
  exercises no GPU itself; it lives in the `gpu` band only because constructing the
  `AssetManager` does.)

## Acceptance

- Clean build; `ctest -L unit` green (the device-free wiring test) and the `gpu`
  firing test green (skips with no device); `include_hygiene` green.
- `TypeInfo` carries an optional `SpawnResolve`, wired by `RegisterImpl` from an
  opt-in `VengReflect<T>` seam, null for non-opted types.
- `Prefab::SpawnInto` fires resolvers in a post-populate pass, fetching storage
  fresh by `TypeId`; a prefab with no resolver-bearing component does zero extra work
  and touches no device (the `prefab_spawn` invariant holds).
- `ResolveComponents(Scene&, Entity, AssetManager&)` fires the same logic for a
  single entity; `Scene::TryGetComponent` exposes the erased fetch.
- `Reflection` gains no upward include; the layering check holds.
