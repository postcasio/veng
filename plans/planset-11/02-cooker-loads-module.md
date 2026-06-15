# Plan 02 — `vengc` loads the module: reflected type table + manifest

**Goal:** give the cooker the ability to **`dlopen` the game module and reflect its
native types**. `vengc cook --module <path>` reuses `Veng::ModuleLoader` to load
`libgame`, calls `VengModuleRegister` with a cooker-owned host carrying a
`TypeRegistry`, and reads back every component descriptor — builtins (via
`RegisterBuiltinTypes`) and game-defined alike. Adds the **scoped cooker→`libveng`
dependency inversion** (decision 4) and a secondary **`vengc generate-type-id` +
type manifest**. No prefab cook yet — this plan delivers and proves the *capability*.

## Why this is its own plan, and on the main thread

It inverts the cooker's dependency graph (`libveng_cook` starts linking
`veng::veng`) and reuses the engine's `ModuleLoader` instead of a bespoke `dlopen`.
That linkage decision, and the headless GPU-free guarantee it rides on (plan 01's
contract), are the reviewable surface. The prefab importer (plan 03) is built on
top, so isolating "the cooker can now see a module's types" keeps the cost
(decision 4) visible and self-contained.

## The dependency inversion — `cooker/CMakeLists.txt`

`libveng_cook` is Vulkan-free and links only `veng::assetformat` today. Reflecting
a module's types means the cooker process loads `libgame` (which pulls `libveng`)
and the cooker code calls `TypeRegistry`/`ModuleLoader`/serializer symbols from
`libveng`. So:

- **`libveng_cook` links `veng::veng`** (PUBLIC for the headers it now uses:
  `Veng/Module/ModuleLoader.h`, `Veng/Reflection/TypeRegistry.h`). The graphics
  stack is linked but **never initialized** — no `Context`, no GLFW — riding plan
  01's GPU-free registration contract.
- `vengc` already requires the Vulkan SDK (Slang); the added link is build weight,
  not a new toolchain requirement. Record it in the CMake comment beside the
  existing "cooker-only deps" note.
- The existing importers (texture/mesh/shader/material) are untouched — the
  separation is relaxed **only** on the module-load path.

> **No bespoke loader.** `Veng::ModuleLoader::Load` already does the `dlopen` +
> `VengModuleAbiVersion` handshake + RAII `LoadedModule`. The cooker reuses it
> verbatim; a version-mismatched module is a located cook error.

## The cooker-side module reflection — `cooker/src/`

A small helper turns a module path into a populated registry:

```cpp
// Loads the game module, runs its VengModuleRegister against a cooker-owned host,
// and returns a TypeRegistry holding builtins + every type the module registered.
// The Application factory the module also registers is captured into a throwaway
// ApplicationRegistry and never invoked (the cooker constructs no app). Editor is
// null. GPU-free — no Context is created. Located Result error on load/ABI failure.
Result<???> LoadModuleTypes(const path& modulePath);
```

The cooker builds `VengModuleHost{ .App = throwaway, .Types = registry, .Editor =
nullptr }`, calls `RegisterBuiltinTypes(registry)` then `module->Register(host)`,
and holds the `LoadedModule` alive alongside the registry (the descriptors'
strings/thunks live in the module image — same lifetime discipline as the launcher,
plan 01). The registry + the live module handle travel together to the importer
(plan 03).

## The `cook --module` flag — `cooker/tool/main.cpp`, `Cooker`

- **CLI:** `vengc cook <pack.json> [-o <out>] [--reference <pack>]... [--module <lib>]`.
  `--module` is optional: a pack with no prefabs needs none (decision 9). When
  present, the tool calls `LoadModuleTypes` and threads the resulting `TypeRegistry&`
  into the cook.
- **`Cooker::CookPack`** gains an optional `const TypeRegistry*` (null when no
  module). It is handed to importers through the `CookContext` (the prefab importer
  in plan 03 is the only consumer); existing importers ignore it. A prefab entry
  with a null registry is a located cook error ("prefab cooking requires --module").

## Secondary — `vengc generate-type-id` + type manifest

The loaded type table is a source of truth, mirroring `generate-id` for assets:

- **`vengc generate-type-id [--module <lib>]`** mints a fresh `u64` `TypeId`,
  collision-checked against the builtins (and, with `--module`, the game's already-
  registered ids), printing **both** forms — hex `0x…ULL` for C++ `VE_REFLECT`
  literals and decimal for JSON (matching `generate-id`'s output and the house id
  convention).
- **Type manifest (optional emit):** `vengc cook --module … ` can dump a
  `names → TypeId` manifest for tooling/debugging. Keep this minimal — a printable
  table is enough for v1; a persisted manifest artifact is only worth it if a
  consumer appears.

## Tests — `cooker` suite (`-L cooker`) + a headless reflection test

- **Reflect the real module:** a cooker test points `LoadModuleTypes` at the built
  `libhello_triangle` and asserts the registry contains the builtins **and** the
  game's `Spinner`, with `Spinner`'s reflected fields/`TypeId` as authored — **with
  no Vulkan device**. This is plan 01's contract exercised from the cooker. Labelled
  to skip cleanly if the module artifact isn't built (or build-depend on it).
- **ABI mismatch:** pointing `--module` at an ABI-1 module is a located error
  (reuses the `bad_version_module` fixture).
- **`generate-type-id`:** the minted id is non-zero, distinct across invocations,
  and prints both representations; with `--module` it does not collide with a
  registered id.
- **`--module` plumbing:** `cook` without `--module` is unchanged (existing pack
  fixtures still cook); a (placeholder) prefab entry with no `--module` is a located
  "requires --module" error.

## Acceptance

Clean build (cooker now links `veng::veng`); `ctest -L cooker` green; the existing
asset packs still cook bit-identically (no behaviour change for non-prefab packs);
the headless reflection test passes with no ICD. Commit: `Plan 02: vengc loads the
game module — ModuleLoader reuse, cook --module, reflected type table, generate-type-id`.
