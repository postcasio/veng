# planset-10 — scene / entity model (ECS)

**Phase goal:** give veng a runtime **scene** — entities, components, a transform
hierarchy, and a camera — built on a **hand-rolled sparse-set ECS** that supports
**game-defined component types**. This is **future area 7**
([future/README.md](../future/README.md)), the one prerequisite the scene
renderer (area 8) and the editor's scene view (area 6) cannot skip: the scene
renderer's per-frame input is a `Scene` viewed through a `Camera`.

It is **one coherent stream**, not three like planset-9 — a single subsystem grown
in small per-plan increments. No GPU work lands until the sample migration (plan
04); plans 01–03 are pure-CPU and driver-free.

## What this delivers, and what it deliberately holds back

A `Scene` is a runtime **ECS world**: a generational `Entity` handle, type-erased
per-component **sparse-set** storage, templated `Add`/`Remove`/`Get`/`Has`, and
multi-component **queries**. Component *types* are registered into an
engine-owned **`TypeRegistry`** that mints a `ComponentId` and stores each type's
**lifecycle** (construct/destruct/move) and — pulled forward from the editor's
deferred reflection layer — its **field descriptors**, so a generic walk can
serialize any component without knowing its C++ type. Engine builtins (`Name`,
`Transform`, `Parent`, `Camera`, `MeshRenderer`) are pre-registered identically to
a game's own types; a game registers its components through the same public
`Register<T>` call.

What it **holds back** (named, not silently dropped):

- **The cooked `.scene` asset** (source `*.scene.json` → cooker importer → runtime
  loader). Deferred because cooking a scene that contains *game-defined* components
  needs the cooker to obtain those component descriptors — i.e. the cooker would
  have to load the game module. That is the hardest open problem here and gets its
  own pass. v1 builds a `Scene` in code; the reflection layer (plan 03) is shaped
  so the serializer drops onto it later with no rework.
- **A systems framework** (registered, scheduled update systems). v1 is **storage
  + queries**; the app writes its own update loops over `Each`/`View`. The query
  API is the seam a scheduler layers onto later.
- **Archetype storage / large-scene perf.** Sparse-set is correct and simple;
  archetype/chunk iteration is a later optimization behind the same API.
- **Dirty-flag transform propagation.** v1 recomputes world matrices on demand;
  incremental invalidation is an optimization, not a contract change.
- **The deferred `SceneRenderer`** (area 8, the next planset) and **`SceneView`**
  (its input contract). This planset defines `Scene` and `Camera`; the renderer
  planset defines how it consumes them.
- **Editor inspectors** (area 6). The reflection layer is built to serve them, but
  the inspector UI is the editor planset's job.

## Relationship to planset-9 (the module model)

planset-9 (stream A) gives a game a C-ABI module entry (`VengModuleRegister`) and
deliberately **deferred the reflection layer** to "the editor planset," its host
struct carrying no `TypeRegistry`. An ECS with game-defined components **is** that
deferred layer's first real consumer (serialization needs each component's shape),
so this planset pulls a minimal reflection layer forward — consciously ahead of
the editor, exactly as [game-module.md](../future/game-module.md)'s open decision
anticipated ("*leaning engine, since a scene/entity model would want descriptors
for save/load regardless*").

The two plansets are **independent in build order**. Component registration here is
a plain engine API (`TypeRegistry::Register<T>`). In today's single-exe sample the
game calls it directly at startup; under planset-9's module model the same call
routes through `VengModuleRegister`, which means adding a `TypeRegistry&` to
`VengModuleHost` — an **additive** change to that boundary (planset-9 decision 5
reserved exactly this). That host wiring is the **integration seam**, recorded but
**not built here** unless planset-9 has already landed; the ECS and its
registration are fully exercisable in the single-exe world without it.

## Decisions

1. **Hand-rolled sparse-set ECS, not EnTT, not archetype.** A from-scratch
   sparse-set fits the house style and — decisively — lets component identity be a
   `ComponentId` **minted at registration time**, stable across the eventual module
   (`dlopen`) boundary, rather than a compiler `type_hash` that is fragile across
   shared libraries. It also keeps the serialization/reflection seam fully under
   veng's control. Archetype storage is more cache-friendly for large scenes but
   far more complex, and runtime-registered component types complicate its layout —
   a later optimization behind the same API, not a v1 shape.

2. **Component storage is type-erased.** Because component types are
   runtime-registered, a pool stores raw bytes sized by `TypeDescriptor::Size` and
   manipulates them through the descriptor's lifecycle function pointers
   (default-construct, destruct, move-construct). The templated `Add<T>`/`Get<T>`
   API is a thin typed façade over the erased pool, resolving `T` → `ComponentId`
   once per type.

3. **`Entity` is a generational handle.** `{ u32 Index; u32 Generation; }` — the
   generation invalidates a handle whose slot was reused, so a stale `Entity` is
   detected (a fatal `VE_ASSERT` on access, not silent UB). `Entity::Null` is the
   empty handle.

4. **The `TypeRegistry` is engine-owned and explicitly threaded.** A component type
   is process-wide (the same across every `Scene`), so `Application` owns the
   registry (`GetTypeRegistry()`) and threads a `TypeRegistry&` into `Scene::Create`
   — the same explicit-dependency discipline as `Context`/`AssetManager`/
   `TaskSystem` (no globals; area 3 stays closed).

5. **Reflection is hand-written descriptors v1.** Field descriptors are written by
   hand (`game-module.md`'s recommendation); a `VE_REFLECT` sugar macro or codegen
   is a later convenience, not a v1 dependency. Field leaf types are an **open
   `TypeId`** (engine builtins — scalars, `vec2/3/4`, `quat`, `mat4`,
   `AssetHandle<Texture/Mesh/Material>` — pre-registered identically to a game's
   own types), **not** a closed engine enum, so a game shipping a new field type
   extends the vocabulary with no engine change. A small **closed `FieldClass`**
   meta-kind carries generic handling. Component inheritance, when it comes, is
   **single, non-virtual, base at offset 0, walked base-first** (recorded
   direction; v1 components are flat structs). This matches the direction
   planset-9 fixed for the editor.

6. **`Scene` is `Unique`, single-owner.** Nothing holds a `Ref` to a `Scene`; the
   app owns it and the (future) N `SceneRenderer`s read it per frame as
   `const Scene&`. `Scene::Create(TypeRegistry&) → Unique<Scene>`, per
   `docs/ownership.md`.

7. **Transform is a component; hierarchy is a `Parent` link.** `Transform` holds
   **local** TRS (`vec3 Position`, `quat Rotation`, `vec3 Scale`); a `Parent`
   component holds the parent `Entity`. World matrices are computed by a walk
   (roots → children, `world = parent.world * local`). v1 recomputes on demand; no
   dirty-flag cache.

8. **`Camera` is a value type; `CameraComponent` wraps it for the ECS.** `Camera`
   builds view + projection (the thing the future `SceneView` carries);
   `CameraComponent` lets a camera live on an entity and derive its view from the
   entity's world transform. The sample uses `Camera` to replace its hand-rolled
   MVP.

9. **`MeshRenderer` is the bridge to rendering.** `MeshRenderer { AssetHandle<Mesh> }`
   — the mesh owns its materials (planset-7), so the renderer queries
   `(world Transform, MeshRenderer)` and draws each submesh with its material. v1
   keeps hello-triangle's existing `RenderGraph` forward draw, now **sourced from
   the scene**; the deferred pipeline is the next planset.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| Generational `Entity`; `Scene` world (entity free-list + type-erased sparse-set component pools); templated `Add`/`Remove`/`Get`/`Has`; multi-component `View`/`Each` queries | A systems/scheduler framework; archetype storage; deferred entity-destruction batching beyond what correctness needs |
| `TypeRegistry` (engine-owned, threaded): `ComponentId` minting, per-type lifecycle (construct/destruct/move); public `Register<T>` for **game-defined** components | The module-ABI host wiring (`TypeRegistry&` on `VengModuleHost`) — additive seam, lands with/after planset-9 |
| Reflection: `TypeId` (open) + `FieldClass` (closed) + `FieldDescriptor`/`TypeDescriptor`; hand-written descriptors for builtins; a generic field-walk proven by an in-memory serialize round-trip | The cooked `.scene` asset (source/cooker/loader); editor inspectors; `VE_REFLECT` macro / codegen; component inheritance |
| Builtins: `Name`, `Transform` (local TRS), `Parent` + world-matrix walk, `Camera` value type + `CameraComponent`, `MeshRenderer` | Dirty-flag transform caching; skinning/animation; scene graph beyond a parent link |
| Sample migration: hello-triangle builds a `Scene` (one entity: `Transform` + `MeshRenderer` + a game-defined `Spinner`), drives rotation through a query, renders via `Camera` | The deferred `SceneRenderer` and `SceneView` (next planset); multi-camera / N-renderer editor wiring |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Entity + Scene core + `TypeRegistry` lifecycle](01-entity-scene-core.md) | Generational `Entity`; `Scene` (entity free-list + type-erased sparse-set pools); `TypeRegistry` minting `ComponentId` + per-type construct/destruct/move; templated `Add`/`Remove`/`Get`/`Has`. Pure CPU; unit + death tests. No sample change. | proposed |
| 02 | [Queries + builtin components + transform hierarchy](02-queries-hierarchy.md) | `View<T...>`/`Each`; builtins `Name`, `Transform` (local TRS), `Parent`; the world-matrix walk. Engine pre-registers builtins (lifecycle only). Unit tests. No sample change. | proposed |
| 03 | [Reflection: `TypeId` + `FieldDescriptor`/`TypeDescriptor`](03-reflection-layer.md) | The open `TypeId` vocabulary (engine field-type builtins) + closed `FieldClass`; `FieldDescriptor`/`TypeDescriptor`; hand-written field descriptors for the builtins; a generic serialize/deserialize **round-trip** in memory proving save/load-readiness. The consciously-pulled-forward layer. | proposed |
| 04 | [`Camera` + `MeshRenderer` + game-defined registration + sample](04-camera-mesh-sample.md) | `Camera` value type + `CameraComponent`; `MeshRenderer`; the public `Register<T>` path for game-defined components; migrate hello-triangle to a `Scene` + `Camera`, with a game-defined `Spinner` updated over a query. The one GPU-touching plan. | proposed |
| 05 | [Docs + roadmap re-cut](05-docs-roadmap.md) | `plans/README.md` (planset-10 line), `future/README.md` (area 7 done; area 8 unblocked for `Scene`/`Camera`; cooked-scene-asset, systems, and the module-ABI registration seam remain future), `CLAUDE.md` (a Scene/ECS section). | proposed |

## Dependencies & dispatching

One ordered chain — each plan builds on the last:

```
01 (entity/scene/registry core) ──► 02 (queries + builtins + hierarchy)
   ──► 03 (reflection layer) ──► 04 (camera + mesh + game-defined + sample) ──► 05 (docs)
```

- **Recommended single-threaded order:** `01 → 02 → 03 → 04 → 05`, the house
  "one plan per session" cadence.
- **Keep on the main thread:** the contract-setting plans — **01** (the storage +
  registry shape, the type-erasure decision), **03** (the reflection vocabulary,
  the open-`TypeId`/closed-`FieldClass` split), and **04** (the sample shape + the
  game-defined registration path) — plus **05** (docs).
- **Good `model: sonnet` delegation** once those contracts are fixed: **02**'s
  query iteration + the transform-hierarchy walk (mechanical given 01's storage),
  and **03**'s per-builtin hand-written field descriptors + the round-trip test
  harness (given the vocabulary 03 sets). Keep the `TypeId`/`FieldClass` design and
  the sparse-set internals on the main thread.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in
the same pass (only plan 04 touches it) → verify (clean build, `ctest` green, smoke
binary writes a correct-sized PPM, 1280×720 RGB ≈ 2,764,816 bytes) → update this
table → one commit per plan, `Plan NN: <summary>` with a `Co-Authored-By` trailer
(`planset-10:` for the docs plan).

- **Public headers stay backend-free.** All of this planset's headers
  (`Veng/Scene/*`, `Veng/Reflection/*`) are pure CPU — no `vk::`/VMA/GLFW — and are
  **hand-added to `tests/include_hygiene.cpp`** (a manual `#include` manifest, not
  auto-discovered). Every plan keeps `include_hygiene` green.
- **Plans 01–03 add no GPU work** — their tests are driver-free and live in
  `veng_unit` (`-L unit`), with stale-handle / misuse paths in the death suite
  (`-L death`). **Plan 04 draws** (the migrated sample) and must pass the
  `VE_DEBUG` validation gate (`ctest --test-dir build-debug -L validation`); it does
  not change the render path materially, so **it may not widen the allowlist** (it
  is empty).
- **The smoke PPM is non-deterministic** — verify size + exit 0, never
  golden-compare. (The smoke pose is fixed, so the existing `smoke_golden` check
  still applies; if sourcing the transform from the `Scene` moves a pixel,
  regenerate the golden per `CLAUDE.md` and call it out in plan 04.)
- **The engine gains no new third-party dependency** — the ECS, the registry, and
  the reflection layer are all in-house; `assetformat`/`libveng` stay as light as
  today.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

veng has a runtime `Scene`: entities, type-erased sparse-set components, queries, a
transform hierarchy, a `Camera`, and **game-defined component types** registered
through the same path as the engine's own, each carrying hand-written field
descriptors a generic walk can serialize. hello-triangle builds its geometry as a
one-entity scene and renders through a `Camera`. Update
[plans/README.md](../README.md) with the planset-10 line and mark **area 7
delivered** in [future/README.md](../future/README.md), noting that **area 8 (the
deferred scene renderer)** now has its `Scene`/`Camera` prerequisite and is the
natural next planset, and that the **cooked `.scene` asset**, a **systems
framework**, and the **module-ABI component-registration seam** (additive on
planset-9's `VengModuleHost`) remain future.
