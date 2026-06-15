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
multi-component **queries**. Types are registered into an engine-owned
**`TypeRegistry`** — generic over *any* reflected type, each carrying a **stable
`TypeId` authored exactly like an `AssetId`** (hardcoded for engine types,
tool-minted for game types) and storing its **lifecycle** (construct/destruct/move)
and — pulled forward from the editor's deferred reflection layer — its **field
descriptors** (with optional editor metadata), so a generic walk can serialize any
value without knowing its C++ type. A **component is simply a reflected type a `Scene`
pools**; there is no separate component-id space. Engine builtins (`Name`,
`Transform`, `Parent`, `Camera`, `MeshRenderer`) are pre-registered identically to a
game's own types; a game registers its components through the same public path — a
**`VE_REFLECT`** describe-block read back by `Register<T>()`.

What it **holds back** (named, not silently dropped):

- **The cooked `.scene` asset** (source `*.scene.json` → cooker importer → runtime
  loader). Deferred because cooking a scene that contains *game-defined* components
  needs the cooker to obtain those component descriptors — i.e. the cooker would have
  to load the game module. That is **the prioritized next planset**:
  [future area 10 — cooker-side module reflection](../future/README.md#10-cooker-side-module-reflection--the-cooker-loads-the-game-module),
  which `dlopen`s the module to reflect its types. v1 builds a `Scene` in code; the
  reflection layer (plan 03) — its name-keyed, schema-tolerant serializer — is shaped
  so that asset drops onto it with no rework.
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
- **Editor inspectors** (area 6). The reflection layer — including the per-field
  editor metadata (`DisplayName`/`Tooltip`/range/etc.) — is built to serve them, but
  the inspector UI itself is the editor planset's job.
- **Inline-annotation reflection (C++26 P2996/P3394).** The `VE_REFLECT` describe-block
  is the authoring surface now; migrating it to inline `[[=…]]` member decorators read
  by static reflection waits on AppleClang support and is a mechanical re-port behind
  the same descriptor shape.
- **Unifying `ShaderInterface`/`MaterialField` onto the reflection layer — named
  follow-on.** A later planset re-expresses the GPU-data field tables on this
  reflection layer so the editor inspects material params through the same walker.
  It is deliberately *not* here: those tables describe **GPU data layout** (std140/430
  offsets, descriptor slots) reflected **offline by the cooker from Slang**, a
  different mechanism from `offsetof`-based CPU-struct reflection authored by
  `VE_REFLECT`; folding it in would drag the cooker and the material runtime into this
  CPU-only ECS stream. The reflection layer is built generic enough to host it when
  that planset comes.

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
   sparse-set fits the house style and — decisively — lets type identity be a
   **stable `TypeId` authored exactly like an `AssetId`** (see decision 5): a
   hardcoded `u64` literal for engine types, tool-minted for game types. That id is a
   compile-time constant *and* byte-identical across the eventual module (`dlopen`)
   boundary, where a compiler `type_hash` is neither stable nor collision-free across
   TUs/compilers. It also keeps the serialization/reflection seam fully under veng's
   control. Archetype storage is more cache-friendly for large scenes but far more
   complex, and runtime-registered component types complicate its layout — a later
   optimization behind the same API, not a v1 shape.

2. **Component storage is type-erased.** Because component types are
   runtime-registered, a pool stores raw bytes sized by `TypeInfo::Size` and
   manipulates them through the descriptor's lifecycle function pointers
   (default-construct, destruct, move-construct). The templated `Add<T>`/`Get<T>`
   API is a thin typed façade over the erased pool, resolving `T` → `TypeId`
   (a compile-time constant) once per type. A `Scene` keys its pools by `TypeId`; an
   internal dense pool index (assigned at registration, never persisted) is an
   optional optimization, not part of the identity. There is no separate component-id
   space — see decision 5.

3. **`Entity` is a generational handle.** `{ u32 Index; u32 Generation; }` — the
   generation invalidates a handle whose slot was reused, so a stale `Entity` is
   detected (a fatal `VE_ASSERT` on access, not silent UB). `Entity::Null` is the
   empty handle.

4. **The `TypeRegistry` is engine-owned and explicitly threaded.** A reflected type
   is process-wide (the same across every `Scene`), so `Application` owns the
   registry (`GetTypeRegistry()`) and threads a `TypeRegistry&` into `Scene::Create`
   — the same explicit-dependency discipline as `Context`/`AssetManager`/
   `TaskSystem` (no globals; area 3 stays closed).

5. **Reflection describes *any* type; "component" is a role, not a separate
   registry. `TypeId` is a stable id authored like `AssetId`.** The reflection layer
   (`Veng/Reflection/`) is deliberately generic: a `TypeRegistry` holds one `TypeInfo`
   per registered type — its name, size/align, lifecycle thunks, `FieldClass`, and
   field descriptors — keyed by a single **`TypeId`**. *Every* reflected thing lives
   in this one id space: the leaf field types (`Bool`, `F32`, `I32`, `U32`, `U64`,
   `Vec2/3/4`, `Quat`, `Mat4`, `String`, `AssetHandle<Texture/Mesh/Material>`), nested
   structs, and components alike. There is **no separate `ComponentId`** — *a component
   is simply a reflected type a `Scene` has a pool for* (a `Scene` lazily makes a
   `TypeId`-keyed pool the first time a type is `Add`ed to an entity). This keeps
   reflection usable beyond the ECS (nested struct fields, asset/settings structs, and
   — named future work, not unified here — the existing `ShaderInterface`/
   `MaterialField` tables).

   **`TypeId` is a stable `u64`, authored exactly like `AssetId`** — `vengc
   generate-id` mints the value, engine builtins (leaves *and* components) carry
   hardcoded `0x…ULL` hex literals checked into the engine like the core pack's
   built-in asset ids, JSON keeps decimal, and a game mints its own. It is therefore a
   compile-time constant (`TypeIdOf<T>()` reads it off a trait), persisted directly
   (a `.scene` stores a component's `TypeId`, not a name — consistent with cooked data
   storing ids), and identical across the `dlopen` boundary. Two types claiming the
   same id is a fatal collision assert. The `TypeInfo.Name` string is for logs/editor
   display only, never the persisted key.

   `TypeId` is **open** (a game registers a new leaf/struct/component with no engine
   change; the leaves above are pre-registered identically to a game's own types).
   `FieldClass` is a small **closed** meta-kind (`Scalar`, `Vector`, `Quaternion`,
   `Matrix`, `String`, `AssetHandle`, `Reference`, `Struct`, `Enum`) a generic walker
   switches on — `Reference` is an intra-scene `Entity` reference the future loader
   remaps. The serializer and the future editor inspector share this one vocabulary.
   Component inheritance, when it comes, is **single, non-virtual, base at offset 0,
   walked base-first** (recorded direction; v1 types are flat structs).

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

10. **`VE_REFLECT` is the v1 authoring surface; `FieldDescriptor` carries editor
    metadata.** Field descriptors are authored through a **describe-block** macro
    (`VE_REFLECT(T, <TypeId>) … VE_FIELD(member, …) … VE_REFLECT_END()`) placed next
    to the struct — it carries the type's stable `TypeId` literal (decision 5),
    derives each field's `Offset` via `offsetof`, and resolves each field's leaf
    `TypeId` + `FieldClass` from the field type's trait at compile time, so only the
    field *names* are restated, once. The macro specialises a `VengReflect<T>` trait
    that the zero-arg `Register<T>()` reads at startup; the raw
    `Register<T>(name, cls, fields)` overload stays for hand-authoring and leaf-type
    pre-registration. A referenced type whose schema isn't registered yet is
    auto-registered from its trait on reference (recursively for nested structs), so
    there is **no registration-ordering burden**. The macro is pure in-house
    preprocessor (no third-party dep).
    `FieldDescriptor` additionally carries **optional, default-empty editor metadata**
    — `DisplayName`, `Tooltip`, `Min`/`Max`/`Step`, `Hidden`, `ReadOnly`, `Category`
    — which the serializer **ignores** (it reads only `Name`/`Type`/`Offset`); the
    serialization key (`Name`) is kept separate from the human label (`DisplayName`)
    so relabelling never breaks on-disk compatibility. The slots are built now to
    serve the future editor (area 6) **without** building any inspector UI. `VE_REFLECT`
    and this metadata struct are the deliberate **forward-port target for C++26
    annotations (P3394)** — when the toolchain (AppleClang) gains P2996/P3394, the
    block migrates to inline `[[=…]]` decorators read by static reflection, a
    mechanical change behind the same `TypeInfo`/`FieldDescriptor` shape.

11. **Serialization is schema-driven and tolerant.** The generic walk reads only the
    descriptors, never a concrete type. A component blob is keyed at the top by the
    component's stable `TypeId`; *within* a blob, fields are written **name-keyed**
    (`{ field-name, value }`), so adding, removing, or reordering a field does not
    corrupt older data — a field absent on load keeps its default-constructed value.
    The walk **recurses** into `Struct`-class fields (via the nested type's `Fields`),
    serializes an `AssetHandle` field as its underlying `AssetId` (rehydration is the
    deferred loader's job), serializes an `Enum` field as its underlying integer, and
    records a `Reference` (`Entity`) field as `{ Index, Generation }` for the loader to
    remap. Type identity on disk is the `TypeId`; field identity within a type is the
    name. (Per-field stable ids are deliberately *not* minted — fields are local to a
    type, names suffice.)

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| Generational `Entity`; `Scene` world (entity free-list + `TypeId`-keyed type-erased sparse-set component pools); templated `Add`/`Remove`/`Get`/`Has`; multi-component `View`/`Each` queries | A systems/scheduler framework; archetype storage; deferred entity-destruction batching beyond what correctness needs |
| `TypeRegistry` (engine-owned, threaded): generic over **any** reflected type, each a stable `u64` `TypeId` authored like an `AssetId` + per-type lifecycle (construct/destruct/move); public `Register<T>`/`VE_REFLECT` for **game-defined** types; a component is just a reflected type a `Scene` pools | The module-ABI host wiring (`TypeRegistry&` on `VengModuleHost`) — additive seam, lands with/after planset-9; unifying `ShaderInterface`/`MaterialField` onto it (named follow-on) |
| Reflection: one open `TypeId` space (leaves + structs + components) + closed `FieldClass`; `FieldDescriptor` (with optional editor metadata) / `TypeInfo`; `VE_REFLECT` describe-block authoring; a tolerant, name-keyed, recursive generic field-walk proven by an in-memory serialize round-trip | The cooked `.scene` asset (source/cooker/loader); editor inspector UI; inline `[[=…]]` annotation reflection (P2996/P3394); component inheritance |
| Builtins: `Name`, `Transform` (local TRS), `Parent` + world-matrix walk, `Camera` value type + `CameraComponent`, `MeshRenderer` | Dirty-flag transform caching; skinning/animation; scene graph beyond a parent link |
| Sample migration: hello-triangle builds a `Scene` (one entity: `Transform` + `MeshRenderer` + a game-defined `Spinner`), drives rotation through a query, renders via `Camera` | The deferred `SceneRenderer` and `SceneView` (next planset); multi-camera / N-renderer editor wiring |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Entity + Scene core + `TypeRegistry` lifecycle](01-entity-scene-core.md) | Generational `Entity`; `Scene` (entity free-list + `TypeId`-keyed type-erased sparse-set pools); `TypeRegistry` recording each reflected type under its stable authored `TypeId` + per-type construct/destruct/move; templated `Add`/`Remove`/`Get`/`Has`. Pure CPU; unit + death tests. No sample change. | done |
| 02 | [Queries + builtin components + transform hierarchy](02-queries-hierarchy.md) | `View<T...>`/`Each`; builtins `Name`, `Transform` (local TRS), `Parent`; the world-matrix walk. Engine pre-registers builtins (lifecycle only). Unit tests. No sample change. | proposed |
| 03 | [Reflection: open `TypeId` + `FieldDescriptor`/`TypeInfo` + `VE_REFLECT`](03-reflection-layer.md) | The single open `TypeId` space (leaves + structs + components) + closed `FieldClass`; `FieldDescriptor` (with editor-metadata slots) / `TypeInfo`; the `VE_REFLECT` describe-block re-authoring the builtins; a generic serialize/deserialize **round-trip** in memory proving save/load-readiness. The consciously-pulled-forward layer. | proposed |
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
  registry shape, the type-erasure decision, the single `TypeId` space), **03** (the
  reflection vocabulary, the open-`TypeId`/closed-`FieldClass` split, the reflection-
  is-generic-not-component decision, the `VE_REFLECT`/metadata surface), and **04**
  (the sample shape + the game-defined registration path) — plus **05** (docs).
- **Good `model: sonnet` delegation** once those contracts are fixed: **02**'s
  query iteration + the transform-hierarchy walk (mechanical given 01's storage),
  and **03**'s per-builtin descriptors + the `VE_REFLECT` `FOR_EACH` preprocessor
  machinery + the round-trip test harness (given the vocabulary 03 sets). Keep the
  `TypeId`/`FieldClass` design, the reflection/component split, and the sparse-set
  internals on the main thread.

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
