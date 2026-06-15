# planset-11 — cooker-side module reflection + the cooked prefab asset

**Phase goal:** let the cooker **`dlopen` the game module and reflect its native
component types**, and on that capability deliver the **cooked prefab asset** —
a `*.prefab.json` source (entities + components + values) that cooks into a binary
blob, loads as a normal asset, and **spawns its entities into a `Scene`**. This is
**future area 10**
([future/README.md](../future/README.md#10-cooker-side-module-reflection--the-cooker-loads-the-game-module)),
the prioritized next planset after area 7's runtime scene model
([planset-10](../planset-10/README.md)).

Its two prerequisites are in place: the **game-module build model**
([planset-9](../planset-9/README.md)) — `libgame` + launcher + the C-ABI
`VengModuleRegister(VengModuleHost*)` entry — and the **reflection layer + runtime
`Scene`/`TypeRegistry`** ([planset-10](../planset-10/README.md)). planset-11 joins
them: it realizes the **`TypeRegistry&`-on-`VengModuleHost` seam** planset-10 named
as additive, and uses the loaded module's reflected descriptors to validate and
cook prefabs the way the material importer validates `*.vmat.json` against a
shader's reflected `ShaderInterface` today.

It is **one coherent stream** grown in small per-plan increments: first the
registration seam (CPU-only), then the cooker's load-the-module capability, then
the prefab format + importer, then the loader + spawn + sample. Only the last
plan touches the GPU.

## What this delivers, and what it deliberately holds back

The cooker gains the ability to load `libgame`, call `VengModuleRegister` with a
host carrying a `TypeRegistry`, and read back every component type the module
registers — engine builtins and game-defined alike. On that, a **`*.prefab.json`**
source (entities + components + field values) cooks into a binary prefab blob in the
pack, **validated against the reflected component descriptors** (unknown component,
wrong field type, missing field caught at cook time). At runtime it loads through
the **same `AssetManager::Load` path as every other asset** (a cached
`AssetHandle<Prefab>`); the app **spawns** its entities into a mutable `Scene`. The
`Scene` is an engine primitive, never loaded — it is created empty and populated by
spawning prefabs. hello-triangle stops building its **entity/component world** in code
(planset-10 plan 04) and instead **loads + spawns a cooked prefab**; its geometry stays
a runtime-generated primitive adopted into an `AssetHandle` (a runtime resource carries
the invalid `AssetId`, so the prefab serializes its mesh field as "no asset" and the app
assigns the adopted handle to the spawned renderer).

What it **holds back** (named, not silently dropped):

- **A systems/scheduler framework.** Still planset-10's stance: the app writes its
  own update loops over queries. Unchanged here.
- **Scene *editing* / a scene-editor UI** (area 6). This planset cooks and loads a
  prefab; authoring it through an inspector is the editor planset. The reflected-
  descriptor seam the cooker uses here is the **same one the editor's native-type
  inspectors reuse** — built once, here.
- **A `SceneRenderer`** (area 8). The sample keeps planset-10's forward
  `RenderGraph` draw, now sourced from a spawned prefab rather than a code-built world.
- **Installed-package `find_package(veng)` wiring** for `veng_add_game` and the new
  prefab-cook path. In-tree only, as in planset-9.
- **Cross-compiled cooking.** `dlopen` reflection loads a **host == target** lib;
  cooking a cross-compiled target lib on the build host is out (a latent constraint
  for the anticipated Windows port — recorded, not solved).
- **Game-code hot-reload** and **asset hot-reload** — both remain future
  (planset-9 / area 1).
- **Unifying `ShaderInterface`/`MaterialField` onto the reflection layer** — the
  named area-7 follow-on, untouched.

## Relationship to planset-9 and planset-10

planset-9 shipped the module entry with an **inert** host —
`VengModuleHost { ApplicationRegistry& App; EditorRegistry* Editor; }` — and
deliberately deferred a reflection registry, "a change to a boundary nothing ships
against yet" ([game-module.md](../future/game-module.md), seam 2). planset-10 built
the reflection layer and the runtime `TypeRegistry`, but had the **`Application`
own it** (`m_TypeRegistry`, `GetTypeRegistry()`) and the single-exe sample register
its components at startup — explicitly leaving "the module-ABI component-
registration seam (additive on planset-9's `VengModuleHost`)" to a later planset.

**This is that planset.** The seam is the additive `TypeRegistry& Types` field
planset-9 reserved decision-room for and planset-10 named. Realizing it forces one
reconciliation (decision 1): the launcher already calls `VengModuleRegister`
**before** it constructs the `Application`, so a registry the module fills at that
call cannot be an `Application` member — ownership moves **host-side**.

## Decisions

1. **`TypeRegistry` ownership moves from `Application` to the host; component
   registration routes through `VengModuleRegister`.** The single, uniform
   registration path — *the module registers its types in `VengModuleRegister`* — is
   what lets the cooker reflect the same types the runtime does, with no second code
   path. But that call runs **right after `dlopen`, before the `Application` exists**
   (the app's factory is *obtained from* that very call), so a registry the module
   fills then cannot be an `Application` member yet. The owner moves up exactly one
   frame: the launcher constructs a `TypeRegistry` **local**, pre-registers the engine
   builtins (a GPU-free engine call, decision 3), puts it in the host as `Types`,
   calls `module->Register(host)` — at which point the module registers its component
   types — *then* constructs the `Application`, threading the registry in.
   `Application` **borrows** it (a `TypeRegistry&`, no longer a value member);
   `GetTypeRegistry()` and every caller are unchanged. This mirrors `ApplicationRegistry`,
   which is *already* a launcher-owned local that outlives the app — not a new
   ownership concept. It supersedes **planset-10 decision 4** ("Application owns the
   registry"), which only held the registry inside `Application` because planset-10
   had no module-registration story yet; the owner is now whoever loaded the module
   (launcher *or* cooker), as `Context`/`AssetManager`/`TaskSystem` are already
   threaded. (Keeping `Application` the *value* owner was considered and costs more —
   it forces a `TypeRegistry&&` into the factory signature, rippling through
   `ApplicationRegistry` and every module's `RegisterApplication`, or a post-`Create`
   adopt step; the borrow is the lighter touch. A second C-ABI entry was rejected:
   game-module.md resolved "single `VengModuleRegister`, not multiple named exports.")

2. **`VengModuleHost` gains `TypeRegistry& Types`; the ABI version bumps to 2.**

   ```cpp
   struct VengModuleHost
   {
       ApplicationRegistry& App;     // the module's Application factory
       TypeRegistry&        Types;   // the module's component/type descriptors  ← new
       EditorRegistry*      Editor;  // non-null only under the editor host
   };
   ```

   `VENG_MODULE_ABI_VERSION` goes `1u → 2u` (the host layout changed), so a module
   built against the old header fails **loudly at load** through the existing
   `VengModuleAbiVersion` handshake — no silent layout mismatch.

3. **A GPU-free builtin-registration entry, made a guaranteed, tested contract.**
   Registering reflected types touches no `Context`/device — but today that is an
   incidental property of `Register<T>`. This planset makes it a **contract**: a
   public, GPU-free engine function (e.g. `RegisterBuiltinTypes(TypeRegistry&)`)
   pre-registers `Name`/`Transform`/`Parent`/`CameraComponent`/`MeshRenderer` and the
   leaf vocabulary, callable by the launcher *and* the headless cooker with no ICD; and
   the **`Application` factory lambda + `VengModuleRegister` are GPU-free by design**
   (the factory captures `ApplicationInfo`, it does not construct GPU objects). A
   cooker test loads the module and reflects its types with **no device present**,
   pinning the contract.

4. **The cooker links `libveng` and reuses `ModuleLoader` — a deliberate, scoped
   dependency inversion.** `libveng_cook`/`vengc` is Vulkan-free and never links
   `libveng` today. Reflecting a module's types means loading `libgame`, which pulls
   `libveng` (and its graphics stack) into the cooker's process, and the cooker must
   call `TypeRegistry`/serializer symbols that live in `libveng`. So the **prefab-
   cooking path links `veng::veng`** and reuses the existing
   `Veng::ModuleLoader::Load` (ABI-version check included) rather than reinventing a
   `dlopen` wrapper. The graphics stack is **linked but never initialized** — no
   device, no GLFW window, consistent with decision 3. The clean cooker/runtime
   separation is relaxed **only on this load path**; ordinary importers
   (texture/mesh/shader/material) stay exactly as they are.

5. **The cooked prefab blob *is* the reflection serializer's name-keyed record
   format.** planset-10's `WriteFields`/`ReadFields` already define a **name-keyed,
   schema-tolerant** field-record encoding (decision 11 there). The cooked prefab is
   that encoding, per component, wrapped in an entity/component table — *not* a new
   format. The cooker produces it by **reusing `WriteFields`**: it default-constructs
   a type-erased component instance through the registry's lifecycle thunk, binds
   the `*.prefab.json` field values into it (the validation step — decision 6), and
   serializes it; the runtime `PrefabLoader` reconstructs with `ReadFields`. One
   encoder/decoder, defined once in `libveng`, exercised from both sides —
   `WriteFields`/`ReadFields` are promoted to a public header
   (`engine/include/Veng/Reflection/Serialize.h`, added to `include_hygiene`) so the
   cooker and the runtime loader both call the same symbols. The split
   is clean: the **JSON→instance binding is cooker-only** (JSON never enters
   `libveng`, and validation belongs there), while the **instance↔bytes
   serialization is shared `libveng` code** — the cooker owns the JSON half, not the
   wire format. (The alternative — the cooker hand-writing the record bytes from JSON
   — would define the wire format in two places that must stay byte-identical
   forever, inviting silent prefab corruption on drift; rejected, especially since
   plan 02 already links `libveng` into the cooker, so `WriteFields` is right there.)

6. **The importer validates against descriptors, not blind passthrough.** Because
   the module is loaded, the cooker has every component's `TypeInfo`. The
   `PrefabImporter` rejects, at cook time, an **unknown component** (no registered
   `TypeId`), a **wrong field type** (JSON value incompatible with the descriptor's
   `FieldClass`/leaf `TypeId`), and surfaces a located error
   (`"prefab importer: entity[3] component 'Spinner': field 'Speed' …"`) — mirroring
   the material importer's validation of `*.vmat.json` against reflected
   `MaterialData`. A field **absent** from the source keeps its default-constructed
   value (planset-10's schema tolerance), so omission is allowed, type-mismatch is
   not.

7. **`AssetType::Prefab` is appended; a `CookedPrefabHeader` lives in `assetformat`.**
   The enum gains `Prefab` (appended, never renumbered). `assetformat` gets a
   `CookedPrefabHeader` describing the entity/component table — stored with the same
   cycle-avoidance discipline as the other cooked headers (`assetformat` gains **no**
   reflection or engine dependency; it carries opaque bytes the engine's prefab loader
   interprets through the `TypeRegistry`).

8. **A prefab loads as a normal cached asset; a `Scene` is what you *spawn it into*.**
   The cooked blob — the entity/component value tree + its embedded mesh/material
   references — is as immutable and shareable as a `Mesh`'s vertex data, so it loads
   through the **identical path as every other asset**: a registered `AssetLoader`
   for `AssetType::Prefab` produces a refcounted, cached **`Prefab`**, returned from
   the **same** `AssetManager::Load<Prefab>` / `LoadSync<Prefab>` →
   `AssetHandle<Prefab>` as textures/meshes/materials. Its embedded `AssetHandle`
   fields (a `MeshRenderer`'s mesh, etc.) are ordinary `LoadJob` **dependencies**,
   resolved by the *same* mechanism a `Material` uses for its textures and shaders —
   nothing prefab-specific in the load path.

   The runtime **`Scene`** stays exactly as planset-10 defined it — an engine
   primitive, `Unique`, single-owner, mutable (decision 6, untouched) — and is
   **never loaded**. You create one and **spawn** the prefab's entities into it:
   `Prefab::SpawnInto(Scene&, AssetManager&) → vector<Entity>` (the spawned roots)
   creates the entities, `ReadFields` each component, remaps `Entity` `Reference`
   fields (a cooked reference stores the **prefab-local entity index**, which spawn
   resolves to the fresh handle; the invalid-index sentinel `Entity::Null.Index`
   stays `Entity::Null`), and rehydrates the already-resident `AssetHandle` fields. This mirrors the engine's grain: a `Mesh`
   asset loads uniformly and you *use* it for a draw; a `Prefab` loads uniformly and
   you *spawn* it into a world. There is **no** bespoke `LoadScene` and no cache
   bypass — spawning the same prefab twice spawns two independent copies, and a fresh
   world is just spawning into a newly-created empty `Scene`. (`SpawnInto` lives on
   `Prefab`, so the dependency points asset → primitive; the `Scene` primitive gains
   no asset-system dependency.)

9. **Build-order edge: `add_asset_pack(... MODULE <lib>)` + `vengc cook --module`.**
   A pack containing prefabs names the game module; the cook gains a `--module <path>`
   flag that `dlopen`s it before cooking. The build graph grows a
   `lib → cook → bundle` edge **only** for packs that declare a module; ordinary
   packs stay independent of any lib (decision 4's separation, in the build graph).
   `veng_add_game` wires the example's prefab pack to depend on `libhello_triangle`.

10. **Secondary payoff — a reflected type manifest / `vengc generate-type-id`.**
    The loaded type table is a real source of truth: `vengc` can emit a **type
    manifest** (names → `TypeId`s) and mint deduplicated `TypeId`s with a
    `generate-type-id` subcommand (the `TypeId` analogue of `generate-id` for
    assets), so a game authoring a new component gets a collision-free id from the
    same tool. Small and additive; folded into the cooker-capability plan.

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| `TypeRegistry& Types` on `VengModuleHost`; ABI bump `1u→2u`; component registration routed through `VengModuleRegister`; registry ownership moved host-side (launcher constructs + threads into `Application`, which borrows, and on into the `AssetManager` so the prefab loader reflects through it) | Live `Context`/`AssetManager` on the host (still inert — registration is GPU-free); custom asset-type **loader** registration (needs a live `AssetManager`, returns when one is driven) |
| A GPU-free `RegisterBuiltinTypes(TypeRegistry&)` contract; a headless cooker test reflecting the module's types with no device | A systems/scheduler framework; archetype storage |
| `vengc` links `veng::veng` and reuses `ModuleLoader` for `cook --module <path>`; the reflected type table; `vengc generate-type-id` + a type manifest | A standalone cooker `dlopen` wrapper (reuse `ModuleLoader`); cross-compiled / host≠target cooking |
| `AssetType::Prefab`; `CookedPrefabHeader` in `assetformat`; a `PrefabImporter` that parses `*.prefab.json`, **validates** components against reflected descriptors, and emits the `WriteFields` record blob | A new on-disk serialization format (reuse planset-10's name-keyed records); editor authoring UI |
| A registered `Prefab` `AssetLoader` (cached `AssetHandle<Prefab>` via the standard `Load`/`LoadSync`, embedded refs as `LoadJob` dependencies); `Prefab::SpawnInto(Scene&, AssetManager&) → vector<Entity>` (`ReadFields` per component, `Entity` reference remap, `AssetHandle` rehydration) | A bespoke `LoadScene` / cache bypass (a prefab loads like any asset); making the runtime `Scene` itself a cached/shared resource (it stays `Unique`); prefab nesting / composition |
| Sample migration: a **data-driven** hello-triangle — a `*.prefab.json` (`Transform` + `MeshRenderer` + game `Spinner`) cooked via the `MODULE` build edge, **loaded + spawned** at runtime; the mesh stays a runtime-adopted primitive the app assigns to the spawned renderer (no entity/component built in code) | The deferred `SceneRenderer`; multi-camera / N-renderer editor wiring; cooking procedural/primitive geometry (the sample's mesh is runtime-adopted, not cooked) |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [The `TypeRegistry&` module seam + GPU-free registration](01-module-typeregistry-seam.md) | Add `TypeRegistry& Types` to `VengModuleHost` (ABI `1u→2u`); route component registration through `VengModuleRegister`; move registry ownership host-side (launcher constructs, pre-registers builtins via a GPU-free `RegisterBuiltinTypes`, threads into `Application`); migrate hello-triangle's `Spinner` registration into its module entry. CPU-only; module/unit tests, incl. a headless no-device reflection test. | done |
| 02 | [`vengc` loads the module — reflected type table + manifest](02-cooker-loads-module.md) | Link `veng::veng` into the prefab-cooking path; `vengc cook --module <path>` reuses `ModuleLoader` to populate a `TypeRegistry` (builtins + game types); `vengc generate-type-id` + a type manifest. A cooker test reflects the hello-triangle module's `Spinner` with no device. No prefab cook yet. | done |
| 03 | [The cooked prefab asset — format + importer + validation](03-prefab-cook.md) | `AssetType::Prefab`; `CookedPrefabHeader` in `assetformat`; the `*.prefab.json` schema; a `PrefabImporter` that validates entities/components against the reflected descriptors and emits the `WriteFields` record blob (reusing the serializer via type-erased instances). Cooker tests for the happy path + each validation failure. | done |
| 04 | [`Prefab` asset loader + `SpawnInto` + sample](04-prefab-spawn-sample.md) | A registered `Prefab` `AssetLoader` (cached `AssetHandle`, embedded refs as dependencies — uniform with every asset); `Prefab::SpawnInto(Scene&, AssetManager&) → vector<Entity>` (`ReadFields`, `Entity` reference remap, `AssetHandle` rehydration). The `add_asset_pack MODULE` / `veng_add_game` build edge. Migrate hello-triangle to author + cook + load + spawn a prefab. The one GPU-touching plan; validation gate + smoke. | proposed |
| 05 | [Docs + roadmap re-cut](05-docs-roadmap.md) | `plans/README.md` (planset-11 line); `future/README.md` (area 10 **DONE**; remaining: editor, scene renderer, events/input; hot-reload still future); `game-module.md` (seam 2 delivered); `CLAUDE.md` (cooked-prefab + cooker-loads-module + GPU-free registration contract). | proposed |

## Dependencies & dispatching

One ordered chain — each plan builds on the last:

```
01 (module TypeRegistry seam) ──► 02 (cooker loads module)
   ──► 03 (prefab format + importer) ──► 04 (prefab loader + spawn + sample) ──► 05 (docs)
```

- **Recommended single-threaded order:** `01 → 02 → 03 → 04 → 05`, the house
  "one plan per session" cadence.
- **Keep on the main thread:** the contract-setting plans — **01** (the ABI seam +
  the ownership reconciliation + the GPU-free registration contract), **02** (the
  cooker→`libveng` dependency inversion + the build-linkage decision), **03** (the
  prefab format + the validation contract), and **04** (the `Prefab`
  loader + `SpawnInto` API + the build-order edge) — plus **05** (docs).
- **Good `model: sonnet` delegation** once those contracts are fixed: **03**'s
  per-`FieldClass` JSON→field binder + the located-error cases + the cooker
  round-trip tests (given the format 03 sets), and **02**'s `generate-type-id` /
  manifest CLI plumbing (given the load path). Keep the ABI seam, the ownership
  move, the dependency inversion, and the `Prefab`/`SpawnInto` API on the
  main thread.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in
the same pass (plan 04 migrates the sample; plan 01 migrates only its module's
registration call) → verify (clean build, `ctest` green, smoke binary writes a
correct-sized 1280×720 RGB PPM ≈ 2,764,816 bytes) → update this table → one commit
per plan, `Plan NN: <summary>` with a `Co-Authored-By` trailer (`planset-11:` for
the docs plan).

- **Public headers stay backend-free.** Any new public headers (the `Prefab` /
  spawn API,
  `AssetType::Prefab` is already in `assetformat`) are pure CPU — no `vk::`/VMA/GLFW —
  and are **hand-added to `tests/include_hygiene.cpp`**. Every plan keeps
  `include_hygiene` green.
- **`assetformat` gains no reflection/engine dependency.** `CookedPrefabHeader` is
  opaque bytes; the `TypeRegistry`-driven interpretation lives entirely in the
  engine's `PrefabLoader` (decision 7).
- **The cooker→`libveng` link is scoped and recorded.** It is the deliberate cost of
  area 10 (decision 4); `vengc` already requires the Vulkan SDK (for Slang), so the
  added graphics-stack link is a build-weight cost, not a new toolchain requirement.
- **Plans 01–03 add no GPU work** — their tests are driver-free (`-L unit`,
  `-L death`, and the cooker suite). The **no-device reflection test** (plan 01/02)
  is the new contract guard. **Plan 04 draws** (the migrated sample, now loading +
  spawning a cooked prefab) and must pass the `VE_DEBUG` validation gate
  (`ctest --test-dir build-debug -L validation`); it does not change the render path
  materially, so **it may not widen the allowlist** (it is empty).
- **The smoke PPM is non-deterministic** — verify size + exit 0. The smoke pose is
  fixed, so `smoke_golden` still applies; plan 04 keeps the same runtime-adopted
  primitive geometry (only its *source* — the entity/components carrying it — moves
  into a spawned prefab), so the capture is **unchanged** and the golden is **not**
  regenerated. A green `smoke_golden` is the proof the data-driven path renders
  identically.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

The cooker can load a game module and reflect its native component types; a
`*.prefab.json` cooks — **validated against those descriptors** — into the pack and
loads at runtime through the **same `AssetManager::Load` path as every other asset**
(a cached `AssetHandle<Prefab>`, embedded references resolved as ordinary
dependencies), from which the app **spawns** its entities into a mutable `Scene`
with entity references remapped. hello-triangle ships a cooked prefab it loads +
spawns instead of building its world in code. Mark **area 10
delivered** in [future/README.md](../future/README.md) and update
[game-module.md](../future/game-module.md) (seam 2 — the `TypeRegistry` host wiring
— delivered), noting that the **editor** (area 6, whose native-type inspectors reuse
this reflected-descriptor seam), the **scene renderer** (area 8), and **events/input**
(area 4) remain the natural next plansets, and that **hot-reload** (asset and
game-code) and the **`ShaderInterface`/`MaterialField` unification** stay future.
