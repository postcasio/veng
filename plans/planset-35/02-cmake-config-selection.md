# Plan 02 — CMake configuration selection & per-config output packs

**Goal:** wire configuration selection into the build — the `vengc cook --config` flag, an
`add_asset_pack` `CONFIG` dimension (one output pack per configuration), and a `VENG_BUILD_CONFIG`
cache variable that **defaults from the host triple**, so a bare `cmake --build` on a Mac cooks the
macOS/ASTC pack with no flag. hello-triangle ships a macOS/Windows configuration pair and cooks the
host-default one. **Depends on Plan 01.**

## Why it is its own plan

Plan 01 taught the cooker to consume a `BuildConfiguration`; this plan decides *which* configuration a
build selects, and makes the selection ergonomic — the part of the design doc that was open and the
piece explicitly asked for ("cmake build on a Mac uses the Mac configuration for asset cooking"). It
is a CMake + CLI surface with no engine-runtime change, cleanly separable from the cooker resolution
below it and the editor above it.

## What lands

- **`vengc cook … --config <file>`.** A new flag (parsed in [`cooker/tool/main.cpp`](../../cooker/tool/main.cpp)'s
  `cook` subcommand) passing the active configuration's `*.buildcfg` to the cook; the cooker hand-parses
  it (Plan 01), populates `CookContext.Config` (`cooker/src/Cooker.cpp`), and records it as a central
  depfile input. Absent the flag, the cook is the zero-config ASTC default — unchanged. (`project.veng`
  is the editor/project index of configurations; the cook is handed the one resolved `*.buildcfg`, not
  the index.)

- **`add_asset_pack(... CONFIG <file>)`.** [`add_asset_pack`](../../cmake/AssetPack.cmake) grows an
  optional `CONFIG` argument naming a `*.buildcfg`. It reads the pack suffix from that file
  (`file(READ)` + `string(JSON …)` on `OutputSuffix` — the struct field stays the single source of
  truth, no redundant `SUFFIX` argument), derives a **per-config** output path (`sample-macos.vengpack`),
  a **per-config** depfile (`<output>.d`), and a **per-config** custom-target name
  (`${TARGET_NAME}-<configname>`), appends `--config <file>` to the cook, and adds the config file to
  that command's `DEPENDS`. Each `(pack × configuration)` is thus its own output/target/depfile —
  exactly the design's "one output pack per configuration," with no collision between configs. A pack
  with no `CONFIG` cooks the zero-config default into the un-suffixed name and target (the existing
  behavior, byte-for-byte). The target exposes two properties: `VENG_ASSET_PACK_OUTPUT` (the suffixed
  build-tree path) and `VENG_ASSET_PACK_MOUNT_NAME` (the un-suffixed name the app mounts), so the
  launcher copy (Decision 5) can rename.

- **`VENG_BUILD_CONFIG` — host-defaulted selection.** A new cache variable whose **default is derived
  from the host triple** (`CMAKE_HOST_SYSTEM_NAME` + `CMAKE_HOST_SYSTEM_PROCESSOR`): `Darwin`+`arm64`
  → the project's macOS configuration, `Windows` → the Windows configuration, `Linux` → the Linux one.
  A project maps host → configuration file in its own CMake (a small lookup the project owns, since
  the configuration *set* is the project's); `add_asset_pack` resolves the `CONFIG` file for
  `VENG_BUILD_CONFIG` when the caller defers to it. The resolved configuration is **printed at
  configure time** (`message(STATUS …)`) so a foreign selection is never silent. `cmake -B build-win
  -S . -DVENG_BUILD_CONFIG=windows` overrides to cook the BC7 pack — always allowed (the encoder is
  CPU; building a foreign config on any host is normal). Note the cache variable is **sticky**: once
  set in a build tree it persists across plain `cmake --build` runs until re-set or the cache is
  cleared — a configured-once foreign build keeps cooking foreign until overridden (Decision 6).

- **A `cook-all-packs` aggregate target.** A custom target depending on **every** configuration's
  output pack, for CI / ship that wants all platforms' packs in one build. The default build still
  cooks only the host-default configuration's pack (the relocatable trio beside the launcher), so a
  developer's incremental build never pays for foreign-platform encodes it does not need.

- **hello-triangle ships a configuration pair.** A `examples/hello-triangle/configs/` directory with a
  macOS configuration (every role → the `ASTC4x4` family, matching the shipped golden) and a Windows
  configuration (→ the `BC7` family); the project's CMake maps host → config and threads the
  host-default into [its `add_asset_pack`](../../examples/hello-triangle/CMakeLists.txt). On the dev
  Mac the default resolves to macOS/ASTC, so the **smoke golden is unaffected** (the cooked formats are
  identical to planset-33's). The `project.veng` lists both configurations with macOS active.

## Decisions

1. **A bare `cmake --build` cooks the host-matching configuration.** `VENG_BUILD_CONFIG` defaults
   from the host triple — the zero-ceremony path the request named. An explicit override cooks any
   configuration (CPU-only, unrestricted), and `cook-all-packs` cooks them all.
2. **One `add_asset_pack` invocation is one (pack × config) output, with its own target and depfile.**
   Per-config invalidation falls out: each configuration is its own output path, custom-target name,
   and depfile, so editing `windows.buildcfg` re-cooks only the Windows pack — the macOS pack's depfile
   and `DEPENDS` do not name that file. (A regression test asserts exactly this: touching
   `windows.buildcfg` leaves the macOS pack's timestamp untouched.)
3. **The configuration set is the project's, the selection default is the engine's.** The engine
   provides the `VENG_BUILD_CONFIG` variable and the host-triple default mechanism; the project
   supplies the actual configuration files and the host → config map (it knows its ship targets). The
   engine does not hardcode a platform list.
4. **The default build stays incremental and host-only.** Foreign-platform packs cook only under an
   explicit override or `cook-all-packs`, so a normal edit-build-run loop never encodes a config the
   developer is not running.
5. **The relocatable trio is unchanged.** `veng_add_game` copies the host-default configuration's
   pack beside the launcher under the name the app mounts (`ExecutableDirectory()`-relative). The copy
   **renames**: source is `VENG_ASSET_PACK_OUTPUT` (the suffixed build path, `sample-macos.vengpack`),
   destination is `VENG_ASSET_PACK_MOUNT_NAME` (the un-suffixed `sample.vengpack` the launcher mounts).
   The suffix never leaks past the build tree, so the launcher mounts the same name regardless of which
   configuration cooked it.
6. **`VENG_BUILD_CONFIG` is sticky, and the host-default cook is host-samplable by construction.** A
   plain `cmake --build` cooks whatever `VENG_BUILD_CONFIG` last resolved to; the host-triple default
   makes that host-samplable, and the configure-time `message(STATUS …)` surfaces a foreign override.
   The smoke/golden tests cook (and mount) the host-default pack, so a stale foreign selection would
   show up as a configure-line mismatch, not a silently-passing test — and the launcher copy mounts the
   un-suffixed name either way.

## Files

| File | Change |
|---|---|
| `cooker/tool/main.cpp` (CLI) | The `--config <file>` flag on the `cook` subcommand → resolved `*.buildcfg` path. |
| `cooker/src/Cooker.cpp` | Parse the config → `CookContext.Config`; record it as a depfile input. |
| `cmake/AssetPack.cmake` | `CONFIG` arg → read `OutputSuffix` from the buildcfg via `string(JSON …)`; per-config output / target-name / depfile; `--config` + config in `DEPENDS`; expose `VENG_ASSET_PACK_OUTPUT` + `VENG_ASSET_PACK_MOUNT_NAME`; default to `VENG_BUILD_CONFIG`. |
| `cmake/BuildConfig.cmake` (new) | Declare `VENG_BUILD_CONFIG` with a host-triple default; the configure-time `message(STATUS …)`; the `cook-all-packs` aggregate target scaffolding. |
| `cmake/Game.cmake` | Copy `VENG_ASSET_PACK_OUTPUT` → `VENG_ASSET_PACK_MOUNT_NAME` beside the launcher (rename at copy). |
| `examples/hello-triangle/configs/macos.buildcfg`, `windows.buildcfg`, `project.veng` (new) | The configuration pair + project index (macOS active). |
| `examples/hello-triangle/CMakeLists.txt` | Map host → config; thread the host-default `CONFIG` into `add_asset_pack`. |
| `tests/…` | A per-config invalidation check: touching `windows.buildcfg` does not re-cook the macOS pack. |

## Verification

- Clean build on the dev Mac; `ctest` green — `VENG_BUILD_CONFIG` defaults to macOS (printed at
  configure), the cook produces the ASTC pack, `hello_triangle_launcher_smoke` runs the relocatable
  trio (mounting the renamed un-suffixed pack) and exits 0.
- `smoke_golden` does **not** move — the macOS configuration resolves every role to the same `ASTC4x4`
  formats planset-33's hardcoded default produced; the cooked bytes are identical.
- A configured `-DVENG_BUILD_CONFIG=windows` build cooks `sample-windows.vengpack` with `BC7` formats
  (a build-only check; the BC7 pack is not sampled on the non-BC dev path); `cook-all-packs` builds
  both packs; touching `windows.buildcfg` re-cooks only the Windows pack.
- `include_hygiene` / `validation_gate` unaffected — no engine-runtime or public-header change.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
