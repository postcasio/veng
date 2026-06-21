# planset-27 — self-resolving components (the spawn-resolve thunk)

**Phase goal:** load any asset or spawn any prefab and have **every** dependent
resource — cooked *or* generated — stream in with **no further caller
intervention**. Today a cooked dependency graph already auto-streams (a prefab's
loader fans every embedded `AssetHandle` out through `Load`, recursively), but a
**generated** resource does not: a `PrimitiveComponent` carries a *recipe*, not an
`AssetId`, so the cascade cannot reach its mesh. Closing that gap is the whole
phase. The deliverable that outlives the primitive use case is a generic
**per-component spawn-resolve thunk** on `TypeInfo`: a component can declare a
resolver that `Prefab::SpawnInto` fires automatically after a component is
populated, turning "a recipe became a renderable" into a property of the spawn
path rather than a separate pass the caller must remember to run.

This **supersedes** [planset-26](../planset-26/README.md)'s plan 07 resolution
model. That planset shipped `ResolvePrimitiveMeshes(Scene&, AssetManager&,
PrimitiveMeshCache&)` — an explicit scene-scan the app calls after `SpawnInto` and
the prefab editor calls **every frame**, backed by a caller-owned, render-thread
`PrimitiveMeshCache` that dedups identical recipes and prunes orphaned entries. It
works, but it makes the caller own a cache *and* remember to resolve, and it
special-cases one component through a bespoke entry point. planset-27 removes all
three: the scan, the cache, and the caller responsibility.

It changes no on-disk format and no cooked artifact. `PrimitiveComponent`, its
variant recipe, `BuildShapeMeshData`, the async `Mesh::CreateAsync`, and
`AssetManager::CreateAsync` are all unchanged from planset-26 — only *how* the
recipe becomes a `MeshRenderer.Mesh` changes.

It also folds in two self-contained cleanups that ride the same open code. The four
extra `Primitives::` shapes planset-26 deferred — **cylinder, cone, torus, capsule**
— are added to the generators and the `PrimitiveShapeVariant` so a prefab can author
them and the new resolve path streams them in for free (plan 04). A **component
naming pass** (plan 05) makes every component a bare noun — `PrimitiveComponent` →
`Primitive`, `CameraComponent` → `Camera`, and the mislabelled value-type `Camera`
(the render-ready view-projection) → `CameraView` — codifying the rule in CLAUDE.md.
And the asset-factory surface is finished and split along the seam it actually has:
**runtime async build factories for `Texture` and `Material`** (plan 06, the
`Task<Ref<T>>` siblings `Mesh` already has), then plan 07 draws the **`Create` /
`Build`** line — a *low-level GPU resource* constructs from a descriptor
(`Create(const XInfo&)`, sync), a *higher-level engine asset* is produced from CPU data
it uploads (`Build` async / `BuildSync` blocking, the `Load`/`Upload` rule), and
`AssetManager::CreateAsync<T>` folds into an `Adopt(Task<Ref<T>>)` overload. None of the
four touch the resolve *seam*; they ride code plans 02–06 already have open.

## The decisions that shape this planset

1. **A generic spawn-resolve thunk, not a primitive special-case.** `TypeInfo`
   gains one optional function pointer — `void (*SpawnResolve)(void* component,
   Scene&, Entity, AssetManager&)` — populated for a component that opts in, null
   otherwise. This is the **same pattern** as the existing variant thunks
   (`VariantActiveType`/`VariantSetActive`/…): an optional `TypeInfo` function
   pointer wired by an `if constexpr` branch in `RegisterImpl<T>`. The variant
   thunks key off the *mandatory* `VengReflect<T>` specialization; the resolver
   keys off a *separate, optional* `VengResolver<T>` trait (the `VE_RESOLVE`
   macro), so the typed trampoline lives in the component's translation unit — the
   wiring shape is identical, the detection trait is not the same one.
   `Prefab::SpawnInto` fires whatever
   resolver a component declares; it never names `PrimitiveComponent`. Any future
   resolve-time component — a spline that bakes a mesh, a terrain patch, a
   particle-emitter that allocates a buffer — rides the identical seam. The stored
   pointer is `void*`-erased (one `TypeInfo`, many component types), but an **author
   never sees `void*`**: a resolver is written fully typed —
   `void(T&, Scene&, Entity, AssetManager&)` — and `VE_RESOLVE` generates the
   `void*`→`T*` trampoline, exactly as the existing lifecycle thunks
   (`DefaultConstruct`/`Destruct`) cast internally.

2. **The thunk's type names higher-layer types through forward declarations, the
   one layering compromise.** A resolver inherently needs a `Scene&` and an
   `AssetManager&`, which the foundational `Reflection` layer must not *include*.
   `TypeRegistry.h` forward-declares `class Scene; class AssetManager; struct
   Entity;` and uses them only as opaque references in the function-pointer
   typedef — no upward include, so `include_hygiene` stays green and the layering
   (no upward *includes*) holds. The variant thunks avoid this by speaking pure
   reflection vocabulary (`void*`/`TypeId`); a resolver cannot, and a forward-decl
   typedef is the minimal price. The alternative — a parallel resolver registry in
   the Scene layer keyed by `TypeId` — keeps `Reflection` pristine but splits type
   registration across two tables and two registration entry points; one
   `TypeInfo`, one `Register<T>()` path is the better trade. The typedef's
   parameter set is **frozen** at `(void*, Scene&, Entity, AssetManager&)`:
   anything a resolver needs beyond these is reached *through* `Scene`/`AssetManager`
   (`manager.GetContext()`, `manager.GetTasks()`), never by widening the signature —
   so this stays the *one* compromise rather than an accreting set of forward-declared
   engine types in the reflection core.

3. **Resolution fires in a post-populate pass, once per resolver-bearing
   component.** `SpawnInto` first creates every entity and `ReadFields` +
   rehydrates (the existing `Resolve` field walk) every component, **then** walks
   the spawned entities a second time and fires each component's `SpawnResolve`.
   The second pass guarantees the whole spawned subtree exists before any resolver
   runs (a resolver may read sibling entities), and — because a resolver may add a
   component (a `MeshRenderer`) — it fetches each component's storage fresh by
   `TypeId` at fire time rather than holding a pool pointer across intervening
   `Add`s. A prefab with **no** resolver-bearing component does zero extra work and
   touches no device, so the existing `prefab_spawn` "the body touches no device"
   property is preserved for every non-procedural prefab.

4. **The runtime/editor entry point is `ResolveComponents(Scene&, Entity,
   AssetManager&)`.** A component created *outside* a prefab spawn — the editor's
   "Add Component", an inspector edit to a recipe, game code calling
   `scene.Add<PrimitiveComponent>` — runs the **same** resolver through one public
   call that fires every resolver-bearing component on a single entity. `SpawnInto`
   uses it internally per spawned entity; the editor calls it on add/edit. There is
   exactly one resolve code path. `ResolveComponents` and the concrete resolvers
   live in `Veng/Scene/Resolve.h` (the planset-26 `PrimitiveResolve.h`, renamed):
   the generic entry point is primitive-agnostic, so it does not live in a
   primitive-named header.

5. **`CreatePrimitiveMesh(AssetManager&, const PrimitiveShapeVariant&) →
   AssetHandle<Mesh>` replaces the generic "procedural create" sketch.** The
   concrete builder is a free function in the primitive layer — the lift-out of the
   `BuildShapeMeshData` → `Mesh::CreateAsync` → pending-handle body currently inline
   in `ResolvePrimitiveMeshes`. It is a free function (not an `AssetManager` method)
   so the manager stays primitive-agnostic, keeping the dependency direction
   primitive → asset (the same reason `SpawnInto` lives on `Prefab`, not `Scene`).
   The `PrimitiveComponent` resolver is a thin wrapper over it: build the active
   shape with `CreatePrimitiveMesh`, skip an empty variant, then add (if absent) and
   set the entity's `MeshRenderer.Mesh`.

6. **No dedup cache — generation is the consumer's to share.** `CreatePrimitiveMesh`
   always builds; identical recipes do **not** auto-merge to one GPU mesh. Dedup was
   a memory/upload optimization, not correctness, and the returned `AssetHandle` is
   shareable — a consumer that wants N entities on one mesh calls `CreatePrimitiveMesh`
   once and assigns the handle N times. The `PrimitiveMeshCache`, `ShapeKey`, its
   `std::hash`, and the orphan-prune all delete. The single case this gives up — one
   prefab hand-authored with N *identical* primitive entities builds N meshes — is a
   rare authoring shape over cheap meshes; recipe-keyed dedup can return *inside*
   `CreatePrimitiveMesh` later, behind the same signature, if it is ever measured to
   matter.

7. **Dropping the cache forces the editor event-driven; that is the intended
   trade.** The planset-26 editor re-scanned the whole scene **every frame**, safe
   only because the cache made the scan idempotent. Without the cache a blind
   per-frame resolve would rebuild meshes every frame, so the editor moves to
   explicit triggers: spawn resolves automatically (decision 3); "Add Component" and
   an inspector edit each call `ResolveComponents` for the touched entity. The churn
   while dragging a parameter slider is **identical** to planset-26 (which also
   rebuilt on every distinct key), but the steady state stops scanning the scene
   each frame.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| `TypeInfo::SpawnResolve` thunk + the opt-in authoring seam + `RegisterImpl` `if constexpr` wiring | A general per-component update/tick system (resolvers run at spawn/edit, not per frame) |
| `SpawnInto`'s post-populate resolve pass; a public `ResolveComponents(Scene&, Entity, AssetManager&)` | A resolver dependency-ordering / topological pass (resolvers are independent today) |
| `Scene::TryGetComponent(Entity, TypeId) → void*` (the erased fetch the resolve pass needs) | Re-resolving on cooked-asset hot-reload (the async re-cook path stays future) |
| `CreatePrimitiveMesh(AssetManager&, const PrimitiveShapeVariant&)` free function | A second procedural component (the seam is generic; no new rider is built here) |
| The `PrimitiveComponent` resolver registered on the type | Storing a baked primitive as an `AssetId`-addressable mesh |
| **Delete** `PrimitiveMeshCache`, `ShapeKey` + its `std::hash`, `ResolvePrimitiveMeshes`, the prune | Editing the cooked on-disk prefab/mesh format (untouched) |
| Migrate the app (drop the post-spawn resolve + the cache member) and the editor (per-frame scan → on-edit triggers) | A bespoke merged "mesh source" inspector control |
| Four new `Primitives::` shapes (cylinder/cone/torus/capsule) + their variant alternatives, riding plan 02's open path | Nested prefabs (`AssetHandle<Prefab>` fields) — thematically apt but its own planset |
| Component naming pass: bare-noun components (`Primitive`/`Camera`), value `Camera` → `CameraView`, the CLAUDE.md rule | A nested-prefab spawn (covered above) |
| Runtime async build factories for `Texture`/`Material` (the `Task<Ref<T>>` siblings of `Mesh::CreateAsync`) | A concrete runtime texture/material *generator* (the factories ship without an in-tree consumer) |
| The `Create`-resource / `Build`-asset split: asset builds → `Build`/`BuildSync`, `AssetManager::CreateAsync` → `Adopt(Task)`, the CLAUDE.md rule | Renaming the low-level `Create(const XInfo&)` GPU-resource factories (they keep `Create`) |
| Unit (CPU, throwaway resolver + new-shape geometry) + GPU (primitive spawn) tests; docs/CLAUDE updates | Container/array `FieldClass`; the `ShaderInterface`/`MaterialField` unification — separate reflection decisions |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [The spawn-resolve thunk seam](01-spawn-resolve-thunk.md) | Add `TypeInfo::SpawnResolve` (typed over forward-declared `Scene`/`Entity`/`AssetManager`); the opt-in authoring seam off `VengReflect<T>`; the `if constexpr` wiring in `RegisterImpl`; a public `Scene::TryGetComponent(Entity, TypeId)`; `SpawnInto`'s post-populate resolve pass; and the public `ResolveComponents(Scene&, Entity, AssetManager&)`. Unit-tested with a throwaway, device-free resolver component; `include_hygiene` green. | done |
| 02 | [`CreatePrimitiveMesh` + the primitive resolver; delete the cache](02-primitive-resolver.md) | Lift the inline build into `CreatePrimitiveMesh(AssetManager&, const PrimitiveShapeVariant&) → AssetHandle<Mesh>`; write + register the `PrimitiveComponent` resolver (build the active shape, add/set `MeshRenderer.Mesh`); **delete** `PrimitiveMeshCache`, `ShapeKey` + `std::hash`, `ResolvePrimitiveMeshes`, and the prune. Migrate the GPU primitive test to spawn-then-pump. | done |
| 03 | [Migrate consumers + docs](03-consumer-migration.md) | App: drop the post-`SpawnInto` `ResolvePrimitiveMeshes` call and the `m_PrimitiveCache` member. Editor: replace the per-frame scan with `ResolveComponents` on prefab-spawn (now automatic), "Add Component", and inspector edit; drop `m_PrimitiveCache`. Verify the smoke golden is unchanged. Update `CLAUDE.md` (engine + root), the asset docs, and `plans/README.md`. | done |
| 04 | [Expand the primitive shape set](04-primitive-shape-set.md) | Add `Primitives::Cylinder`/`Cone`/`Torus`/`Capsule` (device-free generators), their `*Shape` recipe structs + variant alternatives, and their `BuildShapeMeshData` arms; mint four new shape `TypeId`s. The editor widget and the cooker validate them for free; `CreatePrimitiveMesh` and the resolver are unchanged. | done |
| 05 | [Component naming consistency](05-component-naming.md) | Make every component a bare noun: `PrimitiveComponent` → `Primitive`, `CameraComponent` → `Camera`, and the value-type `Camera` (the render-ready view-projection) → `CameraView`; `TypeId`s unchanged. Update the sample `.prefab.json` key and add the bare-noun rule to CLAUDE.md. A mechanical, behavior-preserving sweep; smoke golden unchanged. | proposed |
| 06 | [Runtime async build factories for `Texture`/`Material`](06-async-build-factories.md) | Add the `Task<Ref<T>>`-returning async build factories for `Texture` (the high-level sibling of its loader-shaped two-phase form) and `Material` (its first async build), feeding `AssetManager::CreateAsync<T>` exactly as `Mesh::CreateAsync` does (plan 07 renames these to `Build`). Standalone GPU tests; no in-tree consumer yet. | done |
| 07 | [`Create` for GPU resources, `Build` for engine assets](07-create-async-default.md) | Split the factory surface: engine-asset builds (`Mesh`/`Texture`/`Material`) move to `Build` (async) / `BuildSync` (blocking); `AssetManager::CreateAsync<T>` folds into an `Adopt(Task<Ref<T>>)` overload; `CreatePrimitiveMesh` → `BuildPrimitiveMesh`. Every low-level `Create(const XInfo&)` GPU-resource factory is unchanged. Sweep ~87 call sites; state the split in CLAUDE.md. Mechanical, behavior-preserving. | proposed |

## Dependency & order

The seam chain at the core; the additive/naming plans branch off and the two rename
sweeps (component names, factory names) come last:

```
01 (seam) → 02 (primitive on the seam, cache deleted) → { 03 (consumers) ‖ 04 (shapes) } → 05 (component naming)
06 (Texture/Material async factories — independent)
{ 02, 04, 06 } → 07 (Create rename — last)
```

- **01** is the reusable mechanism and is independently unit-testable with a
  throwaway resolver — no GPU, no primitive code. It must land first: 02 registers a
  resolver and 03's editor triggers both call `ResolveComponents`.
- **02** depends on 01 (it registers a `SpawnResolve`) and deletes the planset-26
  resolution model in the same pass that replaces it, so the tree never has both.
- **03** depends on 02: the app and editor cannot drop their `ResolvePrimitiveMeshes`
  calls until the resolver makes them redundant.
- **04** depends on 02 (it extends `BuildShapeMeshData` and the variant after the
  resolution rewrite) but is **independent of 03** — the new generators touch no
  consumer code, so 03 and 04 can run in parallel once 02 lands.
- **05** is a mechanical component-name sweep over the final reference set, after 02,
  03, and 04 have settled which code mentions `PrimitiveComponent`/`Camera`.
- **06** is **independent** of the seam — it needs only planset-26's
  `Mesh::CreateAsync`/`AssetManager::CreateAsync` shape, so it parallelizes with
  01–05.
- **07** runs **last** — the `Create`/`Build` sweep covers `Mesh` and
  `CreatePrimitiveMesh` (used by 02), the `Texture`/`Material` factories (06), and
  `AssetManager::CreateAsync` together, so it depends on 02, 04, and 06. It is
  orthogonal to 05 (factory names vs component names); the two sweeps don't touch each
  other's symbols.
- **Worktree base (megaexec).** Isolated worktrees branch from `origin/main`, not the
  session's integration HEAD, so only **01** and **06** start from a clean base.
  {03, 04} must branch from the **02** integration commit, **05** from 02+03+04, and
  **07** from 02+04+06 — a manual worktree off the integrating commit, not a fresh
  `origin/main` worktree. The two wide sweeps (05, 07) run **strictly sequentially**
  on the correct base, never as parallel `origin/main` worktrees.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` and
the editor in the **same** pass as the breaking change (02 deletes the API, 03
migrates the callers) → verify (clean build, `ctest` green, smoke binary writes a
correct-sized PPM, **smoke golden unchanged** — the geometry does not move) → update
this table → one commit per plan, `Plan NN: <summary>` with a `Co-Authored-By`
trailer (`planset-27:` for roadmap-only edits).

- **Only plan 04 mints `TypeId`s** — four, for the new `*Shape` recipe structs, via
  `vengc generate-type-id` (the `TypeId` analogue, not `generate-id`) once the build
  is green (placeholders until then). Plans
  01–03 add no persisted type, and **plan 05's renames preserve every `TypeId`
  literal** (identity is the id, not the C++/registered name) — so cooked archives
  are untouched throughout.
- **Plans 01, 04's generators, 05, and 07 are CPU/logic** — plan 01's *wiring* test
  runs under `ctest -L unit` (a throwaway resolver proving `VE_RESOLVE` makes
  `SpawnResolve` non-null and opt-out stays null, no device), while its *firing*
  assertions sit in the `gpu` band, since `SpawnInto`/`ResolveComponents` take an
  `AssetManager&` that requires a `Context`; plan 04's geometry parity is a
  device-free `BuildShapeMeshData` unit test, and 05 and 07 are behavior-preserving
  rename sweeps. **Plans 02–03, plan 04's spawn test, and plan 06
  create/draw GPU resources** and must pass the `VE_DEBUG` validation gate (`ctest
  --test-dir build-debug -L validation`); every async build reuses planset-6's
  host-visible `Upload`, already validation-clean, so no allowlist widening is
  expected — no plan may widen it. **Plans 03, 05, and 07 each assert the
  `smoke_golden` capture is unchanged** — no rendered geometry moves.
- **`include_hygiene`** must stay green: `TypeRegistry.h` adds only forward
  declarations (`class Scene; class AssetManager; struct Entity;`), no new include.
- **Layering check:** the `Reflection` layer gains no upward *include*. The
  `SpawnResolve` typedef names `Scene`/`AssetManager`/`Entity` only as forward-declared
  references; the concrete resolver functions live in the Scene/Asset layers where
  those types are complete.
- **Delegation.** Good `model: sonnet` subagent work once the seam contract is fixed:
  the primitive resolver body + the GPU test migration (02) after 01 lands, the
  consumer/editor migration + docs (03) after 02, the four shape generators + their
  recipe structs/tests (04) after 02 (03 and 04 parallelize), the `Texture`/`Material`
  factories + tests (06, independent), and the two mechanical rename sweeps (05 after
  03+04, 07 after 02+04+06). Keep on the main thread: the `TypeInfo` thunk +
  `RegisterImpl` wiring + `SpawnInto` resolve pass (01), the `CreatePrimitiveMesh`
  lift and the delete of the cache/scan (02), and verification/commits.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

Loading an asset or spawning a prefab streams in every dependent — cooked or
generated — with no caller-side resolve pass and no caller-owned cache. A component
declares a resolver once and `SpawnInto` runs it automatically; `Primitive` (née
`PrimitiveComponent`) is the first rider on a seam any future resolve-time component
reuses, now over an expanded shape set
(cube/plane/sphere/icosphere/cylinder/cone/torus/capsule). The planset-26
`ResolvePrimitiveMeshes`/`PrimitiveMeshCache` model is gone, and every component is a
bare noun (`Camera`/`Primitive`, the render-ready view-projection now `CameraView`)
under a codified CLAUDE.md rule. The asset-factory surface is complete and split along
its real seam: `Texture` and `Material` gain the `Task<Ref<T>>` build factories `Mesh`
had, low-level GPU resources construct through `Create(const XInfo&)`, engine assets
are produced through async-by-default `Build` / blocking `BuildSync`, and a runtime
resource enters the cache through `Adopt` (resident `Ref` or pending `Task`). Update
[plans/README.md](../README.md) with the planset-27 line and note in `CLAUDE.md` / the
asset docs that a component can carry a recipe whose resources resolve and stream
automatically at spawn, that the per-frame primitive scan and its cache are retired,
that components are named as bare nouns, and that GPU resources `Create` while engine
assets `Build`.
