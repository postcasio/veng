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

- **Area 7 — mark DONE (planset-10).** Rewrite the section's header and body the way
  areas 1/2/3/5/9 are marked: a `> Done — delivered by [planset-10]` blockquote
  summarising what shipped (ECS, reflection layer, transform hierarchy, camera,
  game-defined components) and what stayed future (cooked `.scene` asset; systems;
  the `VengModuleHost` `TypeRegistry&` registration seam; archetype/dirty-flag
  perf).
- **Area 8 (scene renderer) — note its `Scene`/`Camera` prerequisite is now met.**
  Update the ordering diagram and area-8 text: with area 9 (compiled graph) already
  done and area 7 now done, the deferred `SceneRenderer` has **both** rendering and
  scene prerequisites — its remaining gate is just being taken up (its first/hardest
  consumer, the editor, is a consumer not a blocker). It is the natural next planset.
- **Area 6 (editor).** Note the reflection layer it planned to build now exists
  (delivered here for serialization); its inspectors consume it rather than
  introduce it. The scene editor's area-7 gate is cleared.
- **Ordering & dependencies / Status.** Move area 7 into the done list; leave area 4
  (events/input), area 6 (editor), area 8 (scene renderer), and hot-reload as the
  remaining future work, with the cooked `.scene` asset called out as a named
  follow-on within the asset/scene line.

## `CLAUDE.md`

Add a **Scene / ECS** subsection under Core conventions (or its own top-level
section near Application/Assets), in the house factual style — present-tense facts,
no plan citations:

- A `Scene` is a runtime ECS world: generational `Entity` handles, type-erased
  sparse-set component storage, `Add`/`Remove`/`Get`/`Has`, and `View`/`Each`
  queries. Structural changes during iteration are illegal (single-threaded model).
- Component **types** register into the engine-owned `TypeRegistry`
  (`Application::GetTypeRegistry()`, threaded into `Scene::Create`); a game registers
  its own types through the same `Register<T>(name, fields)` call as the builtins —
  there is no special engine path for builtin components.
- The reflection layer (`Veng/Reflection/`): an open `TypeId` field-type vocabulary,
  a closed `FieldClass`, and hand-written `FieldDescriptor`s drive generic
  serialization (and, later, editor inspectors).
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
