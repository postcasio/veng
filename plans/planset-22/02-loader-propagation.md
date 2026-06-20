# Plan 02 — Propagate to AssetError::Corrupt

**Goal:** thread Plan 01's `ReadFields` `Result` through the one consumer that parses
untrusted-first bytes — the prefab loader — so a corrupt cooked blob surfaces as
`AssetError::Corrupt` through the existing `AssetResult` channel instead of aborting. The
spawn-time read, on bytes already validated at load, keeps its assert. End-to-end pinned by a
`gpu`-band test feeding a truncated cooked prefab.

## What lands

### Load-time propagation ([engine/src/Asset/Loaders/PrefabLoader.cpp](../../engine/src/Asset/Loaders/PrefabLoader.cpp))

The loader already collects embedded handle deps by deserializing each component record into a
type-erased instance (≈ line 206) and already raises `std::unexpected(Corrupt(id, "..."))` for
its own structural range checks (≈ lines 177, 187). The `ReadFields` call becomes one more
checked step:

```cpp
const VoidResult read = ReadFields(component.Record, instance.data(), typeInfo, types);
if (!read)
{
    typeInfo.Destruct(instance.data());
    return std::unexpected(Corrupt(id, read.error()));
}
```

This is the untrusted-first parse: the load path is where a corrupt cooked blob first meets the
serializer, so it is where the recoverable error is raised. The `Corrupt` helper and the
`AssetResult` return type are already in place — no new error channel.

### Spawn-time assert ([engine/src/Asset/Prefab.cpp](../../engine/src/Asset/Prefab.cpp))

`Prefab::SpawnInto` reads each component record again (≈ line 139) to populate the freshly
spawned entity. By spawn time the record has already parsed clean at load, so a failure here
would be an engine bug, not bad data — the call `.value()`s the `Result`:

```cpp
ReadFields(component.Record, slot, typeInfo, registry).value();  // validated at load
```

`SpawnInto` keeps its `vector<Entity>` return — no signature change rides this plan. A one-line
comment states why the assert is correct here (the load path validated the bytes).

## Decisions

1. **The load path raises `Corrupt`; the spawn path asserts.** A cooked prefab's records are
   parsed at load to gather handle deps, then again at spawn to populate components. The load
   parse is the untrusted-first one and owns the recoverable error; the spawn parse runs only on
   records that already passed load, so `.value()` there asserts on what would be an engine
   invariant violation, not on user data. This keeps `SpawnInto`'s signature and the whole spawn
   call graph free of `Result` plumbing.

2. **`AssetError::Corrupt`, not a new kind.** A malformed component record is exactly the
   "cooked blob failed to parse" the `Corrupt` kind already names
   ([AssetError.h:19](../../engine/include/Veng/Asset/AssetError.h)); the loader already returns
   it for adjacent range checks. Reusing it keeps the consumer-facing error surface unchanged —
   `LoadSync` callers branch on `AssetError::Kind` exactly as before.

3. **The unregistered-type path is unchanged.** A component whose type the module does not
   register is already skipped at load (its handle deps can't exist) and asserts at spawn — a
   registry mismatch, distinct from a truncated record. Plan 02 touches only the truncation
   propagation; the registration-mismatch behavior stays as documented in the loader comment.

## Files

| File | Change |
|---|---|
| `engine/src/Asset/Loaders/PrefabLoader.cpp` | Check the load-time `ReadFields` result; on failure, `Destruct` the instance and `return std::unexpected(Corrupt(id, read.error()))`. |
| `engine/src/Asset/Prefab.cpp` | Spawn-time `ReadFields(...).value()` with a one-line comment that the bytes were validated at load. |
| `tests/gpu/prefab_spawn.cpp` + the gpu suite source list | Add a truncated-cooked-prefab case (and wire the file into the `gpu` CMake band — it is currently untracked and unlisted). |

## Verification

- Clean build; all existing prefab tests green.
- **`tests/gpu/prefab_spawn.cpp`** (gpu band, skips with no ICD — it needs an `AssetManager`,
  hence a `Context`):
  - **Corrupt blob → `Corrupt`** — mount an in-memory prefab archive whose component record is
    truncated (a valid header/table pointing at a record shorter than its declared field claims),
    `LoadSync<Prefab>` it, and assert the result is `std::unexpected` with
    `error().Kind == AssetError::Corrupt` and `error().Id` the requested id — **not** an abort.
  - **Valid blob still loads + spawns** — the existing hand-authored prefab cases (entity
    creation, Reference remapping, double-spawn independence) stay green, proving the happy path
    and `SpawnInto`'s `.value()` are unaffected.
- `smoke_golden` is **byte-identical**.
- **Validation gate** under `VE_DEBUG` (`ctest --test-dir build-debug -L validation`) — no render
  path change; a no-regression check.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present; smoke
  PPM correct size + exit 0.
