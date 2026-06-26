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

- **`vengc cook … --config <file>`.** A new flag passing the active `*.buildcfg` / `project.veng` to
  the cook; the cooker parses it (Plan 00's deserializer), populates `CookContext.Config` (Plan 01),
  and the configuration file lands in the depfile as a central input. Absent the flag, the cook is the
  zero-config ASTC default — unchanged.

- **`add_asset_pack(... CONFIG <file> SUFFIX <s>)`.** [`add_asset_pack`](../../cmake/AssetPack.cmake)
  grows an optional `CONFIG` argument: it appends `--config <file>` to the cook command, adds the
  config file to the command's `DEPENDS`, and names the output pack with the configuration's suffix
  (`sample` → `sample-macos.vengpack`). One `add_asset_pack` invocation = one (pack × configuration)
  output, exactly the design's "one output pack per configuration." Packs with no `CONFIG` cook the
  zero-config default into the un-suffixed name (the existing behavior).

- **`VENG_BUILD_CONFIG` — host-defaulted selection.** A new cache variable whose **default is derived
  from the host triple** (`CMAKE_HOST_SYSTEM_NAME` + `CMAKE_HOST_SYSTEM_PROCESSOR`): `Darwin`+`arm64`
  → the project's macOS configuration, `Windows` → the Windows configuration, `Linux` → the Linux one.
  A project maps host → configuration file in its own CMake (a small lookup the project owns, since
  the configuration *set* is the project's); `add_asset_pack` reads `VENG_BUILD_CONFIG` to pick the
  `CONFIG` file + suffix when the caller defers to it. `cmake -B build-win -S . -DVENG_BUILD_CONFIG=windows`
  overrides to cook the BC7 pack — always allowed (the encoder is CPU; building a foreign config on
  any host is normal).

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
2. **One `add_asset_pack` invocation is one (pack × config) output.** Per-config invalidation falls
   out: each configuration is its own output pack with its own depfile, so editing `windows.buildcfg`
   re-cooks only the Windows pack — the macOS pack's depfile does not name that file.
3. **The configuration set is the project's, the selection default is the engine's.** The engine
   provides the `VENG_BUILD_CONFIG` variable and the host-triple default mechanism; the project
   supplies the actual configuration files and the host → config map (it knows its ship targets). The
   engine does not hardcode a platform list.
4. **The default build stays incremental and host-only.** Foreign-platform packs cook only under an
   explicit override or `cook-all-packs`, so a normal edit-build-run loop never encodes a config the
   developer is not running.
5. **The relocatable trio is unchanged.** `veng_add_game` copies the host-default configuration's
   pack beside the launcher under the name the app mounts (`ExecutableDirectory()`-relative); the
   suffix is a build-tree detail, resolved to the mounted name at copy.

## Files

| File | Change |
|---|---|
| `cooker/src/…` (CLI) | The `--config <file>` flag → parse → `CookContext.Config`. |
| `cmake/AssetPack.cmake` | `CONFIG` / `SUFFIX` args → `--config` + output suffix + config in `DEPENDS`; default to `VENG_BUILD_CONFIG`. |
| `cmake/…` (top-level or a `BuildConfig.cmake`) | Declare `VENG_BUILD_CONFIG` with a host-triple default; the `cook-all-packs` aggregate target scaffolding. |
| `cmake/Game.cmake` | Copy the host-default config's pack beside the launcher under the mounted name (suffix resolved at copy). |
| `examples/hello-triangle/configs/macos.buildcfg`, `windows.buildcfg`, `project.veng` (new) | The configuration pair + project settings (macOS active). |
| `examples/hello-triangle/CMakeLists.txt` | Map host → config; thread the host-default `CONFIG` into `add_asset_pack`. |

## Verification

- Clean build on the dev Mac; `ctest` green — `VENG_BUILD_CONFIG` defaults to macOS, the cook produces
  the ASTC pack, `hello_triangle_launcher_smoke` runs the relocatable trio and exits 0.
- `smoke_golden` does **not** move — the macOS configuration resolves every role to the same `ASTC4x4`
  formats planset-33's hardcoded default produced; the cooked bytes are identical.
- A configured `-DVENG_BUILD_CONFIG=windows` build cooks `sample-windows.vengpack` with `BC7` formats
  (a build-only check; the BC7 pack is not sampled on the non-BC dev path); `cook-all-packs` builds
  both packs.
- `include_hygiene` / `validation_gate` unaffected — no engine-runtime or public-header change.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
