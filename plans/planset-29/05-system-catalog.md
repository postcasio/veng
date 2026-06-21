# Plan 05 — system catalog

**Goal:** make the system registry a **catalog** and let a `SceneSimulation` be built from a
**named, ordered `SystemId` set** instead of "all registered." Registered systems gain a stable
`SystemId` + display name (so the registry can enumerate what is available and resolve an id to its
factory), and `SceneSimulation` gains a constructor taking an ordered id set. Engine-only — no
asset, no cooker, no on-disk format — the foundation Plan 06's `Level` loader selects systems
through.

## Why it is its own plan

It is the engine-side seam the level wiring needs, and it is independently verifiable in isolation:
the catalog (enumerate + resolve), duplicate-`SystemId` detection, and `SceneSimulation`-from-id-list
all have pure tests and depend on **no asset format**. Crucially it does **not** depend on Plan 04 —
so the registry rework lands and is reviewed on its own, de-risked, *before* the `Level` keystone
(Plan 06) builds the loader, the cooked format, and the sample migration on top of it.

## What lands

- **Systems become a catalog.** A `SceneSystem` subclass declares a stable **`SystemId`** (a
  `u64`, authored exactly like a `TypeId`/`AssetId`) and a display name via a trait/macro
  (`VE_SYSTEM(Type, 0x…ULL, "Name")`). This is a real change to `SystemRegistry`, scoped to its
  internals: `Register<T>()` today type-erases `T` into a bare `function<Unique<SceneSystem>()>`
  that retains no id or name; it now also reads `SystemIdOf<T>()` + name off the trait and stores
  `{ SystemId, Name, factory }`, so the catalog can enumerate **without** instantiating. The
  registry gains a read side — enumerate `[{ SystemId, Name }]` and resolve a `SystemId` to its
  factory — and **detects duplicate `SystemId`s with a fatal assert**, mirroring `TypeId`
  registration. The *call site* stays `Register<T>()` and the **host ABI is untouched** (it is a
  template, `VengModuleHost` is unchanged) — but "no registration-signature change" means the call,
  not the registry's stored shape, which does change. `SystemId` is a new id space alongside
  `AssetId`/`TypeId`, minted by `vengc generate-id` the same way.

- **`SceneSimulation` is built from a system-id list, not "all registered."** Its construction
  takes the registry **plus an active ordered `SystemId` set**, so a caller runs exactly the systems
  it names, in its order, honoring the Sim/View phases (Plan 03). A convenience "all registered"
  path stays for tests and the no-level case (the sample still uses it until Plan 06 loads a
  `Level`).

## Decisions

1. **`SystemId` rides a trait, like `TypeId`.** The `Register<T>()` call site and the host ABI are
   unchanged; the registry's *internals* gain id/name storage + a read side + duplicate-id
   detection (it is not a zero-change "just reads a trait"). `SystemId` is a new id space minted
   with `vengc generate-id` (placeholder during implementation), hex in C++ / decimal in JSON.

2. **Selection/order is a list of ids; system *config* is components.** The active set is just
   `[SystemId]` in order. A system's parameters are authored as **components** (a settings entity
   the system reads, defaulting if absent) — reusing the entire reflection inspector and keeping
   systems pure logic, with no reflected-system-config infra. (Plan 06's `Level` is what *stores* a
   selected set; this plan only makes a set *runnable*.)

3. **No module-ABI bump.** The catalog rides traits; nothing touches `VengModuleHost` /
   `VENG_MODULE_ABI_VERSION`.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/SceneSystem.h` | `SystemId`/name trait (`VE_SYSTEM`). |
| `engine/include/Veng/Scene/SystemRegistry.{h,cpp}` | `Register<T>()` stores `{ SystemId, Name, factory }` and fatally rejects a duplicate `SystemId`; catalog read side (enumerate + resolve by id). |
| `engine/include/Veng/Scene/SceneSimulation.{h}` / `engine/src/Scene/SceneSimulation.cpp` | Construct from the registry + an ordered `SystemId` set (phase-honoring); keep an "all registered" convenience. |
| `tests/…` | Catalog + id-list construction tests (see below). |

## Tests

- **Catalog (unit):** register systems, enumerate `[{ SystemId, Name }]`, resolve each `SystemId`
  back to a system; an unknown id resolves to nothing.
- **Duplicate id (death):** registering two systems with the same `SystemId` trips the fatal assert
  (mirrors the `TypeId` collision test).
- **Id-list construction (unit):** a `SceneSimulation` built from an ordered `SystemId` subset runs
  exactly those systems, in that order, Sim before View (Plan 03); the "all registered" convenience
  still builds every registered system.

## Verification

- Clean build; `ctest` green across the unit/death bands, including the new catalog + id-list tests.
- The **no-device cooker test stays green** — engine-only, GPU-free.
- `include_hygiene` stays green — no backend include added.
- `smoke_golden` does **not** move — no runtime path changes here; the sample still hand-registers
  its simulation until Plan 06 loads a `Level`.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
