# Plan 05 — Docs + roadmap re-cut

**Goal:** one documentation pass closing area 7 and re-cutting the roadmap around
it. No code; markdown only. Commit prefix `planset-10:` (roadmap-only change).

## `plans/README.md`

Add the planset-10 line after planset-9, in the established style: scene/entity
model (area 7) — a hand-rolled sparse-set ECS with game-defined components, the
reflection layer pulled forward for serialization, a transform hierarchy, and a
`Camera`; the cooked `.scene` asset, a systems framework, and the module-ABI
registration seam held back. Mark it ✅ done with the plan count.

## `plans/future/README.md`

- **Area 7 — mark its RUNTIME half DONE (planset-10).** Rewrite the section the way
  areas 1/2/3/5/9 are marked: a `> Done (runtime) — delivered by [planset-10]`
  blockquote summarising what shipped (ECS, reflection layer, transform hierarchy,
  camera, game-defined components) and what stayed future. Note that the cooked
  `.scene` asset **and** the `VengModuleHost` `TypeRegistry&` registration seam are
  **area 10** (the prioritized next planset — cooker-side module reflection — already
  drafted in this file's pass), not loose "future." What else stayed future: systems;
  archetype/dirty-flag perf; inline `[[=…]]` annotation reflection once AppleClang
  gains P2996/P3394; the named follow-on re-expressing `ShaderInterface`/
  `MaterialField` on the reflection layer.
- **Area 10 — confirm it reads coherently.** Area 10 (cooker-side module reflection +
  cooked `.scene`) was added to the roadmap during planset-10's design; verify it is
  still the prioritized-next entry and that area 7 / area 6 cross-references to it are
  intact.
- **Area 8 (scene renderer) — note its `Scene`/`Camera` prerequisite is now met.**
  Update the ordering diagram and area-8 text: with area 9 (compiled graph) already
  done and area 7's runtime model done, the deferred `SceneRenderer` has **both**
  rendering and scene prerequisites met — though area 10 and the editor (area 6) are
  sequenced ahead of it.
- **Area 6 (editor).** Note the reflection layer it planned to build now exists
  (delivered here for serialization); its inspectors consume it rather than introduce
  it, and its native-type introspection reuses **area 10's** module reflection. The
  scene editor's area-7 gate is cleared (its area-10 gate — the cooked scene — lands
  next).
- **Ordering & dependencies / Status.** Move area 7's runtime half into the done list;
  keep **area 10 as the prioritized next planset**, then area 6 (editor), then area 8
  (scene renderer); leave area 4 (events/input) and hot-reload as the remaining future
  work.

## `CLAUDE.md`

Add a **Scene / ECS** subsection under Core conventions (or its own top-level
section near Application/Assets), in the house factual style — present-tense facts,
no plan citations:

- A `Scene` is a runtime ECS world: generational `Entity` handles, `TypeId`-keyed
  type-erased sparse-set component storage, `Add`/`Remove`/`Get`/`Has`, and
  `View`/`Each` queries. Structural changes during iteration are illegal
  (single-threaded model). A **component is just a reflected type a `Scene` pools** —
  there is no separate component-id space.
- **Types** register into the engine-owned `TypeRegistry`
  (`Application::GetTypeRegistry()`, threaded into `Scene::Create`), which is generic
  over any reflected type (leaves, structs, components share one `TypeId` space). Each
  type carries a **stable `u64` `TypeId` authored exactly like an `AssetId`** —
  hardcoded `0x…ULL` for engine types, `vengc generate-id` for game types, hex in C++
  and decimal in JSON — so it is a compile-time constant, persisted directly, and
  identical across the future module boundary (a colliding id is a fatal assert). A
  game registers its own types through the same path as the builtins: the `VE_REFLECT`
  describe-block next to the struct, read back by `Register<T>()`.
- The reflection layer (`Veng/Reflection/`): an open `TypeId` space, a closed
  `FieldClass` (`Scalar`/`Vector`/…/`AssetHandle`/`Reference`/`Struct`/`Enum`), and
  `FieldDescriptor`s (authored via `VE_REFLECT`/`VE_FIELD`, carrying optional editor
  metadata — `DisplayName`/`Tooltip`/range/etc. — that the serializer ignores) drive a
  tolerant, name-keyed, recursive generic serialization (and, later, editor
  inspectors). The serialization key (`Name`) is kept distinct from the UI label
  (`DisplayName`); component type identity on disk is the `TypeId`, field identity is
  the name.
- `Transform` is local TRS; `Parent` links the hierarchy; `WorldMatrix` walks it.
  `Camera` builds view/projection; `MeshRenderer` holds the `AssetHandle<Mesh>` a
  draw queries.
- The ownership note: a `Scene` is `Unique`, app-owned, dropped in `OnDispose`.

If plan 04 regenerated the smoke golden, the `HT_SMOKE` capture command in
`CLAUDE.md` is unchanged (still the same binary) — only the golden PNG moved, which
is already documented.

## Acceptance

The three docs read consistently; the planset-10 status table shows all plans
`done`. Commit: `planset-10: scene/entity model — docs + roadmap re-cut`.
