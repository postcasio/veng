# Plan 02 — the cooked `InputMappingContext` asset

**Goal:** make bindings **data**. Add the `AssetType::InputMap` cooked asset, its `*.inputmap.json`
source, the cooker importer (validating every binding against the context's declared actions), and
the runtime loader, so an `InputMappingContext` loads by `AssetId` and hot-reloads through the
existing `MountMemory` path. hello-triangle's in-C++ context from Plan 01 moves into a cooked asset
the game references; the level names the default context to push onto a seat's stack. Depends on Plan
01.

## The starting point

- An asset is cooked offline from a per-type JSON source into a `.vengpack` blob and loaded by
  `AssetId` through `AssetManager::Load`/`LoadSync` (engine/CLAUDE.md, cooker/CLAUDE.md,
  assetpack/CLAUDE.md). Every asset type has a `*.json` source the manifest points at; a new type
  adds an `AssetType` enum arm, a cooker importer, and a runtime loader.
- The cooked blob format is the reflection serializer's `WriteFields`/`ReadFields` record encoding for
  reflected data (as prefabs and levels use), so a reflected type needs **no bespoke binary format**.
- Hot-reload rides `AssetManager::MountMemory(bytes, name) → MountHandle`: the editor cooks a source
  into an in-memory archive, mounts it over the on-disk pack, and reloads the handle behind it.
- Plan 00's `InputAction`/`Binding`/`InputSource` are reflected structs; `ResolvedContext` is their
  in-memory, validated form the resolver consumes.

## What lands

### 1. The asset type

```cpp
/// @brief A named, remappable set of input-action bindings — the authored input scheme.
///
/// Declares the actions it defines (id + name + kind) and the raw-source → action bindings.
/// A seat's InputContextStack references one or more of these; InputMappingSystem resolves
/// the active set against the raw snapshot. Cooked from a *.inputmap.json source, loaded by
/// AssetId like any asset.
class InputMappingContext      // AssetType::InputMap
{
public:
    std::span<const InputAction> GetActions() const;
    std::span<const Binding>     GetBindings() const;
    const ResolvedContext&       Resolved() const;   // the resolver-ready form, built at load
private:
    vector<InputAction> m_Actions;
    vector<Binding>     m_Bindings;
    ResolvedContext     m_Resolved;
};
```

- `AssetType::InputMap` added to the closed `AssetType` set; `CookedInputMapHeader { Version; }` for
  a loud stale-blob reject, matching the other cooked headers.
- The blob **is** the `WriteFields` encoding of the `{ vector<InputAction>, vector<Binding> }` — no
  new format. The loader `ReadFields` them and builds `m_Resolved` (indexing bindings by action,
  computing the positional action schema order Plan 01's wire form depends on).

### 2. The source and the importer

A `*.inputmap.json` source:

```json
{
  "actions": [
    { "id": 12345678901234567890, "name": "Move", "kind": "Axis2D" },
    { "id":  9876543210987654321, "name": "Jump", "kind": "Button" }
  ],
  "bindings": [
    { "source": { "device": "Keyboard", "control": 87 }, "action": 12345678901234567890, "axis": "Y", "scale":  1.0 },
    { "source": { "device": "Keyboard", "control": 83 }, "action": 12345678901234567890, "axis": "Y", "scale": -1.0 },
    { "source": { "device": "Keyboard", "control": 68 }, "action": 12345678901234567890, "axis": "X", "scale":  1.0 },
    { "source": { "device": "Keyboard", "control": 65 }, "action": 12345678901234567890, "axis": "X", "scale": -1.0 },
    { "source": { "device": "GamepadAxis", "control": 0 }, "action": 12345678901234567890, "axis": "Whole" },
    { "source": { "device": "Keyboard", "control": 32 }, "action":  9876543210987654321, "axis": "Whole" }
  ]
}
```

The **`InputMapImporter`** (cooker) parses it and **validates**, the way the material importer
validates `.vmat` fields against shader reflection and the prefab importer validates components
against reflected descriptors:

- every `binding.action` names an action **declared in this context's `actions`** (an unknown action
  id is a located cook error — the typo-catch the absence of a global registry would otherwise miss);
- `axis` is consistent with the action's `kind` (a `Whole` binding onto an `Axis2D` action wants a
  native 2D source; an `X`/`Y` binding onto a `Button` action is an error);
- ids are non-null and unique within the context;
- enum fields (`device`, `kind`, `axis`) parse by name through the shared enum tables.

An omitted optional (`scale`, `invert`) defaults, the same schema tolerance every importer gives.

### 3. The runtime loader

An `InputMapLoader` (`engine/src/Asset/…`, beside the other loaders): resolves the blob, `ReadFields`
the actions + bindings, builds the `ResolvedContext`, returns a cached `AssetHandle<InputMappingContext>`
through the ordinary async `Load` / blocking `LoadSync` path. It is CPU-only (no GPU resource), like
`Skeleton`/`Animation`/`Level`.

### 4. `InputContextStack` references the asset; the sample + level wire it

- `InputContextStack.Active` becomes `vector<AssetHandle<InputMappingContext>>` (Plan 01's placeholder
  resolved). `InputMappingSystem` skips a not-yet-resident handle (a seat with an unloaded context
  resolves to empty actions until it streams in — the ordinary async-load contract).
- hello-triangle's in-C++ context (Plan 01) becomes a cooked `gameplay.inputmap.json` in its asset
  pack; the game's action-id constants stay in C++ and match the cooked ids.
- **The seat gets its context from the level.** The world prefab's seat entity carries an
  `InputContextStack` whose `Active` lists the `gameplay` context `AssetId` — authored data, resolved
  as an ordinary prefab `AssetHandle` dependency. So "which scheme is active" is level/prefab data,
  not code — a gameplay system still push/pops for transient contexts (vehicle, menu), but the base
  context is authored.

### 5. Hot-reload

The editor's cook-on-demand (Plan 03) recooks a `*.inputmap.json` into an in-memory archive and
`MountMemory`s it; the `AssetHandle<InputMappingContext>` reloads behind the stable handle, the
seat's next resolve uses the new bindings — no restart. This reuses the existing `MountHandle`
machinery with no new mechanism.

## Notes & constraints

- **No new dependency.** The importer is cooker-side JSON (nlohmann, already a cooker dep); the
  runtime loader is `assetpack` + reflection, no new library in `libveng`.
- **Build-order edge.** A pack containing an `.inputmap` needs no module (unlike a prefab pack) — the
  context references no game types, only action ids — so it stays module-independent in
  `veng_add_asset_pack`.
- **Mint the ids.** Replace the placeholder `AssetId`/`ActionId` literals with `vengc generate-id`
  values once green, in both the JSON (decimal) and the C++ constants (hex), per the working norms.

## Files (sketch)

- `engine/include/Veng/Asset/InputMappingContext.h` + `engine/src/Asset/InputMappingContext.cpp` —
  the asset + loader.
- `assetpack/…` — `AssetType::InputMap` + `CookedInputMapHeader`.
- `cooker/…` — `InputMapImporter` + its validation, wired into the cook dispatch.
- `examples/hello-triangle/assets/…` — `gameplay.inputmap.json`, referenced by the world prefab's
  seat `InputContextStack`; `main.cpp` drops the in-C++ context, keeps the action-id constants.

## Verification

- **A `cooker` suite test** cooks a valid `.inputmap` and asserts the loaded `ResolvedContext`
  matches; a negative test asserts an unknown-action binding and a kind/axis mismatch each produce a
  located cook error.
- **`hello_triangle-launcher` under `HT_SMOKE`** still writes a correct-sized PPM (input is neutral in
  smoke; `smoke_golden` unmoved).
- **Windowed run:** WASD/Space drive the pawn through the *cooked* context; editing
  `gameplay.inputmap.json` and recooking (Plan 03) hot-reloads the bindings live.
- Clean build, full `ctest` green, validation gate in `build-debug`.

## Dependencies

Plan 01 (the component + system consuming a context). Plan 03's editor consumes this loader.
