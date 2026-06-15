# Plan 02 — Queries + builtin components + transform hierarchy

**Goal:** make the `Scene` iterable and give it its first builtin components. Add
multi-component **queries** (`View<T...>` / `Each<T...>(fn)`), the builtins `Name`,
`Transform` (local TRS), and `Parent`, and the **world-matrix walk** that resolves
the hierarchy. Engine pre-registers the builtins (lifecycle only; field descriptors
come in plan 03). Pure CPU; unit-tested. **No sample change.**

## Why this is its own plan

Queries are the access pattern every consumer (the renderer, the future systems
layer, the sample) uses; the transform hierarchy is the first real multi-component
relationship and the first non-trivial algorithm over the storage. Both build
directly on plan 01's pools and nothing else, so they isolate cleanly from the
reflection vocabulary (03) and the rendering bridge (04).

## Queries — `engine/include/Veng/Scene/Scene.h` (extends plan 01)

```cpp
template <class... Ts> void Each(function<void(Entity, Ts&...)> fn);
template <class... Ts> View<Ts...> View();   // range-for over (Entity, Ts&...)
```

An iteration visits every entity that has **all** of `Ts...`. Implementation picks
the **smallest** of the `Ts...` pools as the driver and tests membership in the rest
via their sparse arrays — the standard sparse-set query, no archetype bookkeeping.
`Each` is the simple callback form; `View` is the range-for form for code that wants
`break`/early-out. Mutating a component through the `Ts&` is fine; structural changes
(adding/removing components, destroying entities) **during** iteration are not — a
present-tense constraint stated in the header, matching the single-threaded model.

## Builtin components — `engine/include/Veng/Scene/Components.h`

```cpp
struct Name      { string Value; };
struct Transform { vec3 Position{0}; quat Rotation{1,0,0,0}; vec3 Scale{1}; };
struct Parent    { Entity Value = Entity::Null; };
```

`Transform` is **local** (relative to the parent, or to world for a root). The
engine registers these into the `TypeRegistry` at `Application` startup, through the
same `Register<T>` a game uses — builtins are not special-cased. (`Camera`,
`CameraComponent`, and `MeshRenderer` arrive in plan 04.)

## World-matrix walk — `engine/src/Scene/Transforms.*`

```cpp
mat4 LocalMatrix(const Transform&);              // T * R * S
mat4 WorldMatrix(const Scene&, Entity);          // walks Parent chain, root → entity
void ComputeWorldMatrices(const Scene&, vector<mat4>& out);  // all transformed entities
```

`WorldMatrix` composes `parent.world * local` up the `Parent` chain. v1 computes on
demand — `ComputeWorldMatrices` does one pass for a renderer that wants every world
matrix at once; there is **no dirty-flag cache** (an optimization for later, behind
this same API). A `Parent` cycle is API misuse → a fatal `VE_ASSERT` (the walk
detects revisiting an entity); a `Parent` pointing at a dead entity asserts too.

## Tests

`veng_unit` (`-L unit`):

- **Single- and multi-component queries:** populate a mix, assert `Each<A>`,
  `Each<A,B>`, `Each<A,B,C>` visit exactly the entities holding all named
  components, in dense order; the empty-result case visits nothing.
- **Smallest-pool driver:** a query where the component sets differ in size still
  visits the correct intersection (guards the optimization).
- **`View` early-out:** range-for with a `break` stops without visiting the rest.
- **`LocalMatrix`:** known TRS → expected matrix (a transformed point lands where
  expected).
- **`WorldMatrix`:** a 3-deep parent chain composes correctly; a root equals its
  local; reparenting changes the result.
- **`ComputeWorldMatrices`:** matches per-entity `WorldMatrix` for a small forest.

`veng_death` (`-L death`): a `Parent` cycle and a `Parent` → dead entity each abort
with the expected message.

`include_hygiene`: add `Veng/Scene/Components.h`.

## Acceptance

Clean build; `ctest -L unit` / `-L death` green; `include_hygiene` builds. Sample
unchanged. Commit: `Plan 02: ECS queries, builtin components, transform hierarchy`.
