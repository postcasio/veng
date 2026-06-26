# Plan 01 — compression roles → cook-time format resolution

**Goal:** make the cooker resolve a texture's cooked format from its **role** through the active
**build configuration**, replacing planset-33's hardcoded ASTC default with the role → format table.
A `*.tex.json` declares a `role` (intent), `CookContext` carries the active `BuildConfiguration`, and
`TextureImporter` resolves `role → format` — with the existing raw `compression` escape hatch on top
and the planset-33 hardcoded ASTC as the **zero-config fallback** when no configuration is supplied.
**Depends on Plan 00.**

## Why it is its own plan

This is the cook-side consumption of the data model — the seam where the authoring intent becomes a
concrete cooked format. It is distinct from the CMake/CLI layer (Plan 02, *which* configuration the
build selects) and from the editor (Plans 03–04, *authoring* the role and the configuration). Landing
the resolution rule alone, exercised through a fixture, keeps every commit green and lets Plan 02
wire selection onto a cooker that already knows what to do with a configuration.

## What lands

- **The `*.tex.json` `role` key.** A texture source declares `"role": "Color"` (or `Normal` / `Mask`
  / `HDR` / `UI`) — its *intent*, parsed to the `CompressionRole` from Plan 00. The **existing**
  `"compression"` key (planset-33: `"astc"`/`"bc7"`/`"none"`, a raw codec name) stays unchanged and
  becomes the **escape hatch**: when present it pins the codec directly and wins over the role. No new
  `"codec"` key is introduced and no existing fixture changes — `"compression"` keeps its current name
  and value space, `role` layers above it. Absent both, the role defaults from the texture's `srgb`
  flag: an sRGB source guesses `Color`, a non-sRGB source guesses `Mask` (the safe unorm default).
  `Normal`/`HDR`/`UI` are **not** auto-guessed — the `srgb` bool cannot distinguish them, so they are
  authored explicitly (the texture editor's role combo, Plan 03).

- **`CookContext.Config`.** `CookContext` gains `const BuildConfiguration* Config = nullptr`
  (alongside the existing `Types` / `Systems`). The `TextureImporter` resolves a texture's cooked
  format as: raw `compression` override, else `Config->RoleToFormat[role]` lowered to
  `Renderer::Format` (Plan 00's `CompressionFormat → Renderer::Format` switch), else — when `Config`
  is null (no project settings) — **planset-33's hardcoded ASTC default** (the zero-config behavior,
  preserved exactly). The resolution is one helper so the fallback chain is in one place.

- **sRGB-correctness through the role.** The role resolves to the sRGB-aware format for color data
  (`Color → ASTC4x4Srgb` / `BC7Srgb` per configuration) and the unorm format for data textures
  (`Normal`/`Mask`/`UI` → the `*Unorm` variant), preserving planset-33's sRGB/unorm split — the role
  carries the sRGB intent so the importer never has to re-derive it from a flag the configuration
  cannot see.

- **The configuration drives the archive compression level.** When a `BuildConfiguration` drives the
  cook, its `CompressionLevel` (Plan 00) becomes the zstd level the pack archive is written at; absent
  a configuration, the cook uses planset-33's existing default level. This is the one place the level
  field is consumed, so it is not a dead schema field.

- **The configuration as a central depfile input.** When a `BuildConfiguration` drives the cook, its
  source file is recorded as **one central cook input** (the cook entry's `RecordDependency`, exactly
  as the pack JSON is recorded centrally), so it lands in the cooker's emitted depfile and a
  configuration edit re-cooks the pack. No per-importer `RecordDependency(configFile)` — a config
  change re-cooks every texture anyway, so a per-asset edge buys nothing (the design doc's Decision 3).
  The CMake-level `DEPENDS` edge that catches the *first* build (before a depfile exists) is Plan 02's.

- **A cooker fixture proving resolution.** A `cooker`-suite test cooks a fixture texture under (a) no
  configuration → ASTC (the zero-config default holds), (b) a BC7 configuration → the `BC7*` format,
  and (c) a raw `compression` override → the pinned codec, asserting the written
  `CookedTextureHeader.Format` ordinal each time. A companion test parses a `*.buildcfg` JSON fixture
  through the cooker's hand-parser and asserts the `RoleToFormat` table and the name-keyed enums
  (the JSON name mapping Plan 00 deferred to the consumer). hello-triangle is **not** migrated here
  (Plan 02 ships its configurations); the smoke golden does not move.

## Decisions

1. **Role wins over the default, raw codec wins over role.** The precedence is `compression` (the
   existing escape-hatch key) > `RoleToFormat[role]` (the configuration) > hardcoded ASTC
   (zero-config). One helper owns the chain so the fallback is auditable.
2. **The zero-config default is preserved bit-for-bit.** A pack with no project settings cooks exactly
   as planset-33 left it (mipped ASTC). Adding build configurations is purely additive; an
   un-migrated project is unaffected.
3. **The role carries sRGB intent.** `Color` is the only sRGB role; the importer reads sRGB-ness from
   the role, not a separate flag the configuration table cannot express — so a configuration's
   `Color → BC7Srgb` is unambiguous.
4. **The config dependency is one central depfile input, coarse by design.** A configuration edit
   re-cooks the whole pack; the texture encode is the expensive, actually-changed part, and the rest
   of a pack re-cook is fast. No per-asset edges.
5. **HDR stays uncompressed.** A `HDR`-role texture resolves to `RGBA16Sfloat` regardless of
   configuration (no HDR block codec exists) — the honest mapping settled in Plan 00, enforced here.

## Files

| File | Change |
|---|---|
| `cooker/src/Importers/TextureImporter.cpp` | Parse the `role` key; resolve role → format through `CookContext.Config` (`compression` override > role > hardcoded ASTC); the sRGB-aware mapping; the `srgb`→`Color`/`Mask` default guess. |
| `cooker/include/…/CookContext.h` | Add `const BuildConfiguration* Config = nullptr` beside `Types` / `Systems`. |
| `cooker/src/Cooker.cpp` (cook entry) | Thread an optional `BuildConfiguration` into `CookContext`; record its source file via `RecordDependency`; apply `Config->CompressionLevel` to the archive write. |
| `cooker/src/…` (config parse) | The hand-parser reading `project.veng`/`*.buildcfg` JSON into the structs via Plan 00's name tables. |
| `tests/cooker/…` | A fixture cooking under no-config / BC7-config / raw-override, asserting the written `Format` ordinal each way; the default-role-from-`srgb` guess; the `*.buildcfg` JSON parse round-trip. |

## Verification

- Clean build; `ctest` green — the new cooker resolution test passes; the `gpu` band is unaffected.
- `smoke_golden` does **not** move — hello-triangle is unmigrated here (it still cooks via the
  zero-config ASTC default until Plan 02 ships its configurations); resolution is exercised by the
  fixture only.
- `include_hygiene` unaffected — `CookContext` is cooker-internal; no public engine header changes.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
