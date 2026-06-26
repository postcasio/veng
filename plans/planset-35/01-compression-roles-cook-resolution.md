# Plan 01 — compression roles → cook-time format resolution

**Goal:** make the cooker resolve a texture's cooked format from its **role** through the active
**build configuration**, replacing planset-33's hardcoded ASTC default with the role → format table.
A `*.tex.json` declares a `role` (intent), `CookContext` carries the active `BuildConfiguration`, and
`TextureImporter` resolves `role → format` — with the raw `codec` escape hatch on top and the
planset-33 hardcoded ASTC as the **zero-config fallback** when no configuration is supplied.
**Depends on Plan 00.**

## Why it is its own plan

This is the cook-side consumption of the data model — the seam where the authoring intent becomes a
concrete cooked format. It is distinct from the CMake/CLI layer (Plan 02, *which* configuration the
build selects) and from the editor (Plans 03–04, *authoring* the role and the configuration). Landing
the resolution rule alone, exercised through a fixture, keeps every commit green and lets Plan 02
wire selection onto a cooker that already knows what to do with a configuration.

## What lands

- **The `*.tex.json` `role` key.** A texture source declares `"role": "Color"` (or `Normal` / `Mask`
  / `HDR` / `UI`) — its *intent*, parsed to the `CompressionRole` from Plan 00. The existing internal
  raw-`codec` seam (planset-33, BC7-vs-ASTC selection) becomes the **escape hatch**: a `"codec"` key
  pins a `Renderer::Format` directly and wins over the role. Absent both, the role defaults from the
  texture's `srgb`/usage (an sRGB source guesses `Color`, otherwise `Mask`/`Normal` heuristics —
  documented, overridable).

- **`CookContext.Config`.** `CookContext` gains `const BuildConfiguration* Config = nullptr`
  (alongside the existing `Types` / `Systems`). The `TextureImporter` resolves a texture's cooked
  `Renderer::Format` as: raw `codec` override, else `Config->RoleToFormat[role]`, else — when `Config`
  is null (no project settings) — **planset-33's hardcoded ASTC default** (the zero-config behavior,
  preserved exactly). The resolution is one helper so the fallback chain is in one place.

- **sRGB-correctness through the role.** The role resolves to the sRGB-aware format for color data
  (`Color → ASTC4x4_Srgb` / `BC7Srgb` per configuration) and the unorm format for data textures
  (`Normal`/`Mask`/`UI` → the `*Unorm` variant), preserving planset-33's sRGB/unorm split — the role
  carries the sRGB intent so the importer never has to re-derive it from a flag the configuration
  cannot see.

- **The configuration as a central depfile input.** When a `BuildConfiguration` drives the cook, its
  source file is recorded as **one central cook input** (exactly as the pack JSON is recorded
  centrally), so it lands in the cooker's emitted depfile and a configuration edit re-cooks the pack.
  No per-importer `RecordDependency(configFile)` — a config change re-cooks every texture anyway, so
  a per-asset edge buys nothing (the design doc's Decision 3).

- **A cooker fixture proving resolution.** A `cooker`-suite test cooks a fixture texture under (a) no
  configuration → ASTC (the zero-config default holds), (b) a BC7 configuration → the `BC7*` format,
  and (c) a raw `codec` override → the pinned format, asserting the written `CookedTextureHeader.Format`
  ordinal each time. hello-triangle is **not** migrated here (Plan 02 ships its configurations); the
  smoke golden does not move.

## Decisions

1. **Role wins over the default, raw codec wins over role.** The precedence is `codec` (escape hatch)
   > `RoleToFormat[role]` (the configuration) > hardcoded ASTC (zero-config). One helper owns the
   chain so the fallback is auditable.
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
| `cooker/src/Importers/TextureImporter.cpp` | Parse the `role` key; resolve role → format through `CookContext.Config` (raw `codec` override > role > hardcoded ASTC); the sRGB-aware mapping. |
| `cooker/include/…/CookContext.h` | Add `const BuildConfiguration* Config = nullptr` beside `Types` / `Systems`. |
| `cooker/src/…` (cook entry) | Thread an optional `BuildConfiguration` into `CookContext`; record its source file as a central depfile input. |
| `tests/cooker/…` | A fixture cooking under no-config / BC7-config / raw-override, asserting the written `Format` ordinal each way; a default-role-from-srgb guess test. |

## Verification

- Clean build; `ctest` green — the new cooker resolution test passes; the `gpu` band is unaffected.
- `smoke_golden` does **not** move — hello-triangle is unmigrated here (it still cooks via the
  zero-config ASTC default until Plan 02 ships its configurations); resolution is exercised by the
  fixture only.
- `include_hygiene` unaffected — `CookContext` is cooker-internal; no public engine header changes.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
