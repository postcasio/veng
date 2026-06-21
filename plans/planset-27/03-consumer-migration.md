# Plan 03 — Migrate consumers + docs

**Goal:** remove the last callers of the deleted resolution model. The app drops its
post-`SpawnInto` resolve call and its cache member; the editor replaces its per-frame
scene scan with event-driven `ResolveComponents` triggers and drops its cache member.
Verify the smoke golden is unchanged, then update the docs. Depends on plan 02.

## Why this is its own plan

Plan 02 deletes the API; this plan migrates everything that called it and lands the
documentation. Keeping it separate keeps 02 focused on the engine seam and lets the
consumer/editor migration — which spans the app, two editor panels, and the docs — be
a clean, reviewable commit (good `model: sonnet` delegation once 02 is fixed).

## App — `examples/hello-triangle/main.cpp`

The app spawns its prefab once at startup and never edits a shape at runtime, so its
migration is pure deletion:

- Drop the explicit `ResolvePrimitiveMeshes(*m_Scene, GetAssetManager(),
  m_PrimitiveCache)` after `SpawnInto` ([main.cpp:115](../../examples/hello-triangle/main.cpp:115)).
  `SpawnInto` now resolves every `PrimitiveComponent` as it spawns ([line 109](../../examples/hello-triangle/main.cpp:109)).
- Drop the `m_PrimitiveCache` member ([main.cpp:449](../../examples/hello-triangle/main.cpp:449))
  and its `OnDispose` clear ([main.cpp:220](../../examples/hello-triangle/main.cpp:220)).

The geometry that appears is identical — the same shapes, built by the same
`CreatePrimitiveMesh`/`Mesh::CreateAsync` path, just kicked off from inside
`SpawnInto` instead of one line later. The smoke golden must not move.

## Editor — `editor/src/panels/`

The planset-26 editor re-scanned the whole scene **every frame**
([SceneViewportPanel.cpp:99](../../editor/src/panels/SceneViewportPanel.cpp:99)),
which only stays idempotent because of the now-deleted cache. Replace the single
catch-all scan with the explicit triggers that cover the four ways a
`PrimitiveComponent` enters or changes in the editor:

1. **Prefab spawned into the editor scene** — `PrefabEditorPanel`'s `SpawnInto`
   ([PrefabEditorPanel.cpp:68](../../editor/src/panels/PrefabEditorPanel.cpp:68)) now
   resolves automatically (plan 01's spawn pass). No editor call needed.
2. **"Add Component" adds a `PrimitiveComponent`** — after the add, call
   `ResolveComponents(scene, entity, assets)` for that entity so the new recipe
   streams its mesh in.
3. **An inspector edit changes a shape/parameter** — fire `ResolveComponents(scene,
   entity, assets)` on a changed edit to the selected entity. This needs **real
   plumbing**: `DrawFieldWidget` currently returns `void`
   ([FieldWidget.h:56](../../editor/src/FieldWidget.h:56)), swallowing the per-widget
   `changed` bool, so this plan threads a `bool changed` return up through
   `DrawFieldWidget` and its nested/variant recursion
   ([FieldWidget.cpp:191](../../editor/src/FieldWidget.cpp:191)), accumulates it per
   entity in `InspectorPanel::DrawComponents`, and resolves when true. `InspectorPanel`
   already holds the `AssetManager&` and the selected entity.
4. **A primitive entity is duplicated** — `PrefabExplorerPanel::DuplicateSubtree`
   ([PrefabExplorerPanel.cpp:406](../../editor/src/panels/PrefabExplorerPanel.cpp:406))
   round-trips each component (including a `PrimitiveComponent`) into the copy but
   builds **no mesh** — today it relied on the deleted per-frame scan. Call
   `ResolveComponents` on each entity the duplicate produces (the new subtree). Without
   this, a duplicated primitive is invisible — a regression the smoke golden does not
   catch (it never duplicates).

Note that the trigger code **relocates across a panel boundary**: the deleted scan
lived in `SceneViewportPanel`, while triggers 2–3 land in `InspectorPanel` and trigger
4 in `PrefabExplorerPanel`.

These four triggers are exhaustive for *today's* editor (no undo/redo or paste exists).
Because correctness now depends on every mutation path firing `ResolveComponents` —
where the deleted scan was blanket-idempotent regardless of how a component arrived —
funnel the call through one shared helper where practical (the add/duplicate/edit
paths), and state the rule in `editor/CLAUDE.md`: **any editor path that adds or edits
a resolver-bearing component must call `ResolveComponents` on the touched entity.** A
future structural op (paste, undo) must add its own trigger.

Then delete the per-frame `ResolvePrimitiveMeshes` call and the
`m_PrimitiveCache` member from `SceneViewportPanel`
([.h:70](../../editor/src/panels/SceneViewportPanel.h:70)), and the same member from
any prefab-editor document holding one.

The churn while dragging a parameter slider is unchanged from planset-26 — each
distinct value rebuilds the mesh and retires the prior one — but the steady state no
longer scans the scene every frame.

## Verify

- Clean build; `ctest` green; the smoke binary writes a correct-sized PPM.
- **`smoke_golden` unchanged** — the rendered geometry does not move
  (`ctest --test-dir build -R smoke_golden`). No golden regeneration; if the capture
  moves, the migration changed behavior and is wrong.
- `VE_DEBUG` validation gate clean.
- The editor: spawning a primitive prefab, adding a `PrimitiveComponent`, editing a
  shape parameter, and **duplicating** a primitive entity each stream the mesh in, with
  no per-frame scan.

## Docs

- **`engine/CLAUDE.md`** — rewrite the `PrimitiveComponent` bullet in the assets
  section: a component carries a recipe whose mesh is generated and streamed
  **automatically at spawn** via its registered spawn-resolve thunk
  (`CreatePrimitiveMesh` + the resolver), not a caller-driven `ResolvePrimitiveMeshes`
  pass; the `PrimitiveMeshCache` is gone. Add a short note on the generic
  `TypeInfo::SpawnResolve` seam in the Scene/reflection section (a component can
  declare a resolver fired by `SpawnInto`/`ResolveComponents`).
- **`editor/CLAUDE.md`** — state the resolve rule: any editor path that adds or edits
  a resolver-bearing component must call `ResolveComponents` on the touched entity
  (the per-frame scan that blanket-covered this is gone).
- **Root `CLAUDE.md`** — only if it references the planset-26 resolution model;
  align it.
- **`plans/README.md`** — add the planset-27 line.
- **`plans/planset-27/README.md`** — flip the status column to `done`.

## Acceptance

- No caller of `ResolvePrimitiveMeshes` remains; the app and editor own no
  `PrimitiveMeshCache`.
- The app resolves at `SpawnInto`; the editor resolves on spawn (automatic),
  add-component, edit, and duplicate — no per-frame scan.
- Smoke golden unchanged; `ctest` and the validation gate green.
- `CLAUDE.md` (engine + root as needed), the asset docs, and `plans/README.md`
  reflect the spawn-resolve model and the retirement of the per-frame scan + cache.
