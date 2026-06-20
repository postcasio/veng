# Plan 01 — Scene spatial-version, recursive destroy + `const` iteration

**Goal:** recover the "did anything spatial change?" signal a rebuilt-on-demand tree needs from
the immediate-mode ECS — a single Scene-owned **spatial version counter** the broadphase (Plan 03)
compares each frame to decide whether to rebuild — the `const` iteration path a read-only consumer
needs so it does not bump the version just by looking, and a **recursive `DestroyEntity`** that
closes the dangling-`Parent` crash the broadphase's rebuild would otherwise hit. Pure `Scene` work,
fully device-free. Independent of Plan 02.

## What lands

### The spatial version counter ([engine/include/Veng/Scene/Scene.h](../../engine/include/Veng/Scene/Scene.h) + src)

A `Scene` bumps a monotonic counter on each mutation of a **spatial** pool. The spatial pools are
the three the broadphase reads — `Transform`, `Parent`, `MeshRenderer` — because those alone decide
whether an entity is a draw candidate and where its world bound sits.

`Scene` gains (private) a `u64 m_SpatialVersion = 0` and an inline bump helper
`BumpSpatial() { ++m_SpatialVersion; }`. No `mutable` is needed — every bump happens inside an
already-non-`const` mutation method or the non-`const` access funnel.

The **bump sites**, all in the `Scene` impl, gated on the relevant `TypeId`:

- `CreateEntity()` — no bump (a bare entity is not yet a candidate; the `Add` of its spatial
  components bumps). `DestroyEntity(e)` — `BumpSpatial()` **iff** `e` holds a spatial component
  (a destroyed `Light` changes no candidate; checked by `Has` over the spatial pools before the
  pools are cleared).
- `AddRaw(e, id)` / `RemoveRaw(e, id)` — `BumpSpatial()` **iff** `id` is a spatial pool's
  (`Transform`, `Parent`, or `MeshRenderer`).
- The **non-`const`** `TryGetRaw(e, id)` — `BumpSpatial()` **iff** `id` is a spatial pool's.
  This is the single funnel every non-`const` component access routes through (`Get<T>`,
  `TryGet<T>`, `Add<T>`'s returned reference, and the `View`/`Each` iterators all resolve through
  it — [Scene.h:101](../../engine/include/Veng/Scene/Scene.h),
  [:166](../../engine/include/Veng/Scene/Scene.h),
  [:330](../../engine/include/Veng/Scene/Scene.h)). So a non-`const` `Get<Transform>` or a
  `View<Transform, …>` iteration bumps the version; a `View<Light>` does not. The bump is
  idempotent-by-design — the broadphase reads only the *final* value once per frame, so a
  `View<Transform>` over the resident set bumping many times is harmless (it rebuilds once).
- `ForEachComponent(e, fn)` ([Scene.cpp:107](../../engine/src/Scene/Scene.cpp)) — `BumpSpatial()`
  **iff** the visited pool's `id` is a spatial pool's. This path iterates `m_Pools` and hands out
  a mutable `void*` **without** going through `TryGetRaw` — it is the editor inspector's edit
  funnel. Without this, moving an object in the inspector would leave every broadphase over the
  scene stale.

The spatial `TypeId`s compare against `TypeIdOf<Transform>()` / `TypeIdOf<Parent>()` /
`TypeIdOf<MeshRenderer>()` — a compile-time constant read off the type trait
([TypeRegistry.h:97](../../engine/include/Veng/Reflection/TypeRegistry.h)), **not** a registry
lookup — so the bump check in these hot mutation paths is a plain integer comparison.

```cpp
// Monotonic counter the Scene bumps whenever a spatial pool (Transform,
// Parent, or MeshRenderer) is structurally changed or accessed non-const (a
// potential in-place edit). A broadphase compares it against the version it
// last built against: equal means nothing spatial moved and the tree stands;
// changed means rebuild. A non-const access bumps it even when it was a read,
// so the bump never misses a write; read-only consumers use the const
// View/Each path to avoid bumping.
[[nodiscard]] u64 GetSpatialVersion() const { return m_SpatialVersion; }
```

> **Not a bump site:** the `const` `TryGetRaw` overload, `DensePtr` (const-only — it returns the
> dense `Entity` array, never component storage), `PoolCount`, and every read primitive. Decision
> 2 rests on the `const` accessors never bumping.

### `const` `View` / `Each` ([engine/include/Veng/Scene/Scene.h](../../engine/include/Veng/Scene/Scene.h))

`View<Ts...>()` / `Each<Ts...>(fn)` are non-`const` only today — the iterator resolves through the
non-`const` `TryGetRaw` ([Scene.h:330](../../engine/include/Veng/Scene/Scene.h)), so even a pure
read forces a non-`const` `Scene` (the renderer `const_cast`s for it). Add `const`-qualified
overloads yielding `const Ts&`:

```cpp
template <class... Ts> [[nodiscard]] SceneView<const Ts...> View() const;
template <class... Ts, class Fn> void Each(Fn&& fn) const; // fn(Entity, const Ts&...)
```

**Pin the shape:** `SceneView` becomes templatable over `const`-qualified `Ts`. Its iterator's
component resolve changes from `static_cast<Ts*>(m_Scene->TryGetRaw(...))` to a cast that, when
`Ts` is `const`, binds the **`const`** `TryGetRaw` overload and yields `const Ts&`; `TypeId`
resolution uses `IdOf<std::remove_const_t<Ts>>()`. The non-`const` overload is unchanged. The
contract: **a `const` iteration only ever calls the `const` accessors, so it never bumps the
version.**

Then **drop the renderer's `const_cast`**: the light pack at
[SceneRenderer.cpp:1579](../../engine/src/Renderer/SceneRenderer.cpp)
(`const_cast<Scene&>(view.World).View<Light>()`) becomes `view.World.View<Light>()` against the new
`const` overload (the loop body reads `light` only — const-clean).

### Recursive `DestroyEntity` ([engine/src/Scene/Scene.cpp](../../engine/src/Scene/Scene.cpp))

`DestroyEntity(e)` today removes only `e`'s own components and recycles its slot
([Scene.cpp:43](../../engine/src/Scene/Scene.cpp)), so destroying a parent leaves every child
holding a `Parent` that points at a now-recycled slot — a dangling up-link `WorldMatrix` fatally
asserts on ([Transforms.cpp:29](../../engine/src/Scene/Transforms.cpp)). The broadphase's rebuild
reaches that walk through `GatherMeshes`, so the latent crash becomes reachable on a rebuild frame.
Fix it at the source: make `DestroyEntity` **recursive** — it destroys `e`'s entire subtree.

`Parent` ([Components.h:38](../../engine/include/Veng/Scene/Components.h)) is an up-link only (no
child index), so the implementation gathers the subtree first, then tears down — never
iterating-and-destroying (a structural change mid-iteration is illegal):

1. **Collect the subtree.** Seed a worklist with `e`; repeatedly scan the `Parent` pool for entities
   whose `Parent` value is in the collected set, adding them, until a pass adds none (a breadth-first
   closure over the up-links).
2. **Tear down.** For each collected entity, remove its components from every pool and recycle its
   slot (the existing per-entity teardown, generation-bumped), and `BumpSpatial()` once if any
   collected entity held a spatial component.

The `WorldMatrix` dangling-`Parent` assert **stays**. With recursive destroy a child can no longer
outlive its parent through `DestroyEntity`, so a dangling `Parent` now arises only from genuine
misuse (manually `Add<Parent>` pointing at a stale or never-existent entity) — exactly what the
assert should catch. A "detach / reparent, keep the children" gesture is a distinct explicit
operation (it must rebake each child's local transform to preserve world position) and is **not**
added here.

## Decisions

1. **Scoped to the three spatial pools, by `TypeId`.** `Transform` and `Parent` set the world
   matrix; `MeshRenderer` sets candidacy and the mesh bound. Bumping on every pool's mutation would
   force a broadphase rebuild on an unrelated `Light` or gameplay edit — correct but needless.

2. **Non-`const` access is the write proxy; there is no setter to hook.** The ECS hands out
   `Transform&` and never sees the write, so "was this a write?" is unanswerable at the mutation
   site. Treating any non-`const` spatial access (including `ForEachComponent`'s erased pointer) as
   a potential write is the only signal available without a new mutation API, and it is **safe**
   (over-bump, never under). This is the access-as-write change-tick Bevy (`Changed<T>`) and Unity
   DOTS (per-chunk change versions) ship; reduced here to a single counter. The `const` path is the
   read-only escape hatch.

3. **A counter, not a journal.** A rebuild-on-change broadphase (Plan 03) needs only "did anything
   move?", not "which entities" — a from-scratch rebuild re-derives the whole tree. So the signal is
   one monotonic `u64`, not a per-entity entry list with cursors and trimming. Per-entity deltas are
   what *incremental* maintenance would need; that is the named refinement, and its signal can be
   added then without disturbing this one (the counter is the global gate either way).

4. **`const View`/`Each` is a general win.** Read-only scene iteration (renderer, editor queries,
   read systems) the non-`const`-only `View` forced a `const_cast` for. Correct independent of the
   broadphase; the broadphase is just the first consumer that *requires* it (to avoid bumping the
   version every frame it reads the scene).

5. **`DestroyEntity` is recursive — closing the dangling-`Parent` crash at the source.** Destroying
   a parent destroys its subtree: the convention every scene-graph engine ships (Unity `Destroy`,
   Godot `queue_free`), and the only option with no orphaned entities, no silent world-space teleport
   of children, and no inert dangling refs. The alternatives — a tolerant `WorldMatrix` that treats a
   dead parent as a root, or reparenting children up a level — either move a child's rendered position
   silently or need the same O(N) child discovery with a messier data model. The O(N) subtree gather
   runs only on destroy (not a hot path); it is O(children) once a down-index exists, which the
   [hierarchy redesign](../future/hierarchy.md) names.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/Scene.h` | `u64 m_SpatialVersion`; `BumpSpatial()`; `GetSpatialVersion()`; `const` `View`/`Each`; `const`-correct `SceneView`. |
| `engine/src/Scene/Scene.cpp` | The bump sites (`DestroyEntity`, `AddRaw`/`RemoveRaw`, non-`const` `TryGetRaw`, `ForEachComponent`, spatial pools only); recursive `DestroyEntity` (gather subtree over `Parent` up-links, tear down bottom-up). |
| `engine/src/Renderer/SceneRenderer.cpp` | Drop the light-pack `const_cast`; iterate `view.World.View<Light>()` through the `const` overload. |
| `tests/unit/scene_version.cpp` (new) + the unit suite source list | Device-free bump/no-bump matrix. |

## Verification

- Clean build; `include_hygiene` still compiles `Veng/Scene/Scene.h` (pure `Scene` + glm, no
  backend leak).
- **`tests/unit/scene_version.cpp`** (device-free, no ICD):
  - `Add`/`Remove` of `Transform`, `Parent`, and `MeshRenderer` each bump `GetSpatialVersion()`;
    `DestroyEntity` of a spatial-holding entity bumps; `Add<Light>` / `DestroyEntity` of a
    `Light`-only entity does **not** bump.
  - A non-`const` `Get<Transform>(e)`, a non-`const` `View<Transform>()` / `Each<Transform>()`, and
    a `ForEachComponent` walk over an entity holding a `Transform` each bump the version; a
    non-`const` `View<Light>()` does **not**.
  - A **`const`** `View<Transform>()` / `Each<Transform>()` and a `const` `TryGet<Transform>()`
    leave `GetSpatialVersion()` **unchanged** — the property the broadphase depends on.
  - Monotonicity: the version only ever increases; equal-version frames mean no spatial mutation
    occurred.
  - **Recursive destroy:** build parent→child→grandchild; `DestroyEntity(root)` leaves all three
    dead (`IsAlive` false) and their slots recycled; a sibling subtree under a different root is
    untouched; a child's `WorldMatrix` is never queried against a destroyed parent (the subtree went
    with it). Destroying a leaf entity destroys only itself.
- The renderer's `const`-overload migration builds and the light pack packs identically
  (`smoke_golden` unchanged — this plan renders nothing new).
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present; validation
  gate clean under `VE_DEBUG`.
