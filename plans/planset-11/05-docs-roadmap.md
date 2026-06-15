# Plan 05 — Docs + roadmap re-cut

**Goal:** record planset-11 as landed and re-cut the roadmap around it. No code —
docs only. Same shape as every planset's closing doc plan.

## `plans/README.md`

Add the **planset-11** line after planset-10, in the established style: cooker-side
module reflection (the cooker `dlopen`s `libgame` to reflect its native component
types, realizing the `VengModuleHost` `TypeRegistry&` seam) + the cooked **prefab**
asset (validated against the reflected descriptors, the way materials are validated
against shader reflection). Note it supersedes **planset-10 decision 4** (the
`TypeRegistry` is now host-owned, not an `Application` member). Update the closing
`future` paragraph: area 10 is no longer "prioritized next" — it is **done**; the
remaining areas are the editor (6), the scene renderer (8), and events/input (4).

## `plans/future/README.md`

- Mark **area 10 DONE** (planset-11), with a one-paragraph recap and the still-future
  remainder (cross-compiled cooking stays out; the type-manifest is delivered).
- **Area 7:** note the cooked prefab asset (split off into area 10) is now
  delivered, so area 7's deferred "scene asset type that cooks and loads like the
  others" is closed — **delivered as the prefab asset** (a `Scene` is an engine
  primitive, never an asset; the cooked thing is a prefab spawned into one); the
  **systems framework** and **`ShaderInterface`/`MaterialField` unification** remain
  its named follow-ons.
- **Area 6 (editor):** the **`TypeRegistry` host-wiring seam** (game-module.md seam 2)
  is now delivered; the editor's native-type inspectors **reuse** this reflected-
  descriptor mechanism rather than reintroducing it. The **scene editor**'s two gates
  (area 7 runtime scene model + area 10 cooked prefab) are both met.
- **Ordering & dependencies / Status:** move area 10 from "in flight / next" to
  "done"; the editor (6) becomes the prioritized next area, with the scene renderer
  (8) and events/input (4) after. Keep the two named deferrals (hot-reload;
  `ShaderInterface`/`MaterialField` unification).

## `plans/future/game-module.md`

**Seam 2 (type reflection / descriptors)** is no longer "the editor-shell planset's
first task" — it is **delivered by planset-11**: `VengModuleHost` carries
`TypeRegistry& Types`, the module registers its component descriptors through
`VengModuleRegister`, and the cooker reflects them. Update the seam-2 section and
the resolved-decisions list (the ABI is now v2; "reflection deferred to the editor"
becomes "reflection delivered by planset-11; the editor reuses it"). Note the
launcher's host-owned registry supersedes planset-10's Application-owned one.

## `CLAUDE.md`

Add to the asset/scene-facing sections:

- **Cooked prefabs.** A `*.prefab.json` (entities + components + field values) cooks
  into an `AssetType::Prefab` blob and loads like **every other asset** — a cached
  `AssetHandle<Prefab>` via `AssetManager::Load`/`LoadSync` — its embedded
  asset references resolved as ordinary load-time dependencies. A `Scene` is an
  engine primitive, never loaded; you **spawn** a prefab's entities into one with
  `Prefab::SpawnInto(Scene&, AssetManager&) → vector<Entity>`, which remaps `Entity`
  references and rehydrates `AssetHandle` fields. The cooked blob is planset-10's
  name-keyed `WriteFields` record encoding.
- **The cooker loads the game module.** `vengc cook --module <lib>` `dlopen`s the
  game module and reflects its component types into a `TypeRegistry` (reusing
  `ModuleLoader`), so the `PrefabImporter` validates a prefab's components against the
  real descriptors (unknown component / wrong field type caught at cook time). The
  prefab-cooking path links `libveng`; this is the one place the Vulkan-free cooker
  relaxes its separation, scoped to the load path and recorded.
- **The GPU-free registration contract.** `RegisterBuiltinTypes` + `Register<T>` +
  the module's `VengModuleRegister` (factory + type registration) touch **no**
  `Context`/device — the headless cooker depends on it. `VengModuleHost` carries
  `TypeRegistry& Types`; the host (launcher or cooker) owns the registry and threads
  it in.
- **Build-order edge.** `add_asset_pack(... MODULE <lib>)` makes a prefab-bearing pack
  cook after its game lib (`lib → cook → bundle`); packs without prefabs stay
  module-independent.
- Add **`AssetType::Prefab`** wherever the asset types are enumerated, and the
  `*.prefab.json` per-asset source to the "every asset type has its own JSON source"
  list.
- **Update the two sections the seam change makes stale:** the **Game modules**
  section describes `VengModuleHost` as `{ ApplicationRegistry& App; EditorRegistry*
  Editor; }` at ABI v1 — add `TypeRegistry& Types` and note the ABI is now v2 and that
  type registration routes through `VengModuleRegister`; the **Scene & ECS** /
  `Application` text says `Application` owns the `TypeRegistry`
  (`Application::GetTypeRegistry()`, threaded into `Scene::Create`) — correct it to
  host-owned-and-borrowed (the launcher/cooker constructs it, `Application` borrows a
  `TypeRegistry&` and threads it into the `AssetManager`).

## Acceptance

Docs only; no build/test impact (re-run `ctest` to confirm nothing regressed from
the planset, but this plan changes no code). Update the planset-11 README status
column to `done` across the board. Commit: `planset-11: docs + roadmap re-cut —
area 10 done, seam 2 delivered, cooked-prefab + cooker-loads-module in CLAUDE.md`.
