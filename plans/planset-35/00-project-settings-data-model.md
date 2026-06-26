# Plan 00 — the project-settings data model

**Goal:** stand up the **reflected data model** for build configurations — `ProjectSettings` and
`BuildConfiguration` under a new `Veng/Project/` home, with JSON (de)serialization and a device-free
round-trip test. This is pure data: no cook integration (Plan 01), no CMake/CLI (Plan 02), no editor
(Plans 03–04). Getting the **schema and the role taxonomy** right here is what keeps every later plan
small. **Foundational; nothing precedes it.**

## Why it is its own plan

The whole planset hangs off two structs and one closed enum. They are consumed by the cooker (Plan
01 reads `BuildConfiguration` in the cook), the CMake layer (Plan 02 selects a configuration file),
and the editor (Plans 03–04 inspect them through reflection) — so the schema must be settled and
tested in isolation before any consumer binds to it. They are reflected `VE_REFLECT` structs so the
editor draws them for free; proving the reflection round-trip device-free here is the same
foundation-first move as planset-20's `AABB` and planset-21's `Frustum`.

## What lands

- **`CompressionRole` — the closed intent taxonomy.** A `VE_LEAF(FieldClass::Enum)` enum naming a
  texture's *intent*, never a codec: `Color`, `Normal`, `Mask`, `HDR`, `UI`. The set is closed and
  durable — a role's *format* fills in as codecs arrive (today every role resolves to a shipped
  `BC7`/`ASTC4x4` format; `Normal → BC5` lands when BC5 does), but the role itself is the stable
  authoring surface. `Color` ⇒ the sRGB-aware albedo codec; `Normal`/`Mask`/`UI` ⇒ the unorm codec;
  `HDR` ⇒ uncompressed `RGBA16Sfloat` (the LDR block codecs cannot carry HDR — settled, not deferred).

- **`BuildConfiguration` — a named ship target (reflected).** Fields: a `Name` (`string`, the
  configuration's id, e.g. `"macos"`), a `Target` triple/label (`string`, e.g. `"macos-arm64"`), the
  **`RoleToFormat` table** (a `CompressionRole → Renderer::Format` mapping), a `CompressionLevel`
  (the zstd level for the archive, an `i32`), and an `OutputSuffix` (`string`, appended to the pack
  name — `sample` + `-macos` → `sample-macos.vengpack`). `Renderer::Format` is the existing
  vocabulary enum (`Veng/Renderer/Types.h`, Vulkan-free), so the table is over engine types with no
  backend leak. The `RoleToFormat` mapping is represented as a fixed small record per role (one
  `Renderer::Format` field per `CompressionRole` enumerator), so it reflects as plain leaf fields and
  the inspector draws a format combo per role — no container-field support (which the reflection layer
  lacks) is needed.

- **`ProjectSettings` — one per project (reflected).** Fields: `Configurations` (the set of build
  configurations) and `ActiveConfiguration` (`string`, the configuration the editor previews through
  / the default). Project-wide invariants only — **no codec here**; the codec lives on the
  configuration. (The configuration *set* is the one place a container is unavoidable; until the
  reflection layer grows array fields it is stored as a small fixed-capacity reflected record or a
  bespoke-parsed list — Plan 03 only needs to inspect each configuration, not reflect the list as a
  `FieldClass::Array`. Settle the representation here.)

- **JSON (de)serialization.** Both structs are **JSON cook inputs** the cooker hand-parses (like
  every other source) — `project.veng` (the project settings) and the per-configuration
  `*.buildcfg` files it references, or one inlined document. A `Veng::Project` (de)serializer reads/
  writes them; the editor (Plan 03) writes through the same path. The role/format enums serialize by
  **name** (`"Color"`, `"ASTC4x4_Srgb"`), the established string-keyed convention, never by ordinal.

- **A device-free round-trip test.** A unit test (the `unit` suite, no ICD) builds a `ProjectSettings`
  with two configurations (a macOS/ASTC and a Windows/BC7 one), serializes to JSON, re-parses, and
  asserts equality — guarding the schema and the name-keyed enum mapping before any consumer binds.

## Decisions

1. **The role taxonomy is closed and settled here:** `Color / Normal / Mask / HDR / UI`. Adding a
   role is a deliberate reflection-enum change, not an open-ended string. A role names *intent*; its
   per-codec format is the configuration's to decide.
2. **`HDR` resolves to uncompressed `RGBA16Sfloat`, not a block codec.** The cooked block codecs
   (`BC7`/`ASTC4x4`) are LDR; there is no HDR compressed path this planset, so the `HDR` role is the
   honest uncompressed mapping, not a deferred TODO. HDR ASTC stays a future footprint item.
3. **The structs live in `libveng` (`Veng/Project/`), the cooker hand-parses.** The reflected struct
   is the editor's inspection schema and the canonical type; the cooker reads the same JSON without
   linking the reflection runtime (consistent with how it already hand-parses every other source).
4. **Enums serialize by name, never by ordinal.** Unlike the cooked-blob integer-stability rule
   (which pins `Renderer::Format` ordinals in the binary header), these JSON authoring files are
   human-edited and use the enum *name*, so a reordering never silently repoints a configuration.
5. **No container-field dependency.** The role table is one format field per role (fixed), and the
   configuration list is stored without needing `FieldClass::Array` (which the reflection layer does
   not have). This keeps Plan 00 inside the existing reflection surface.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Project/CompressionRole.h` (new) | The closed `CompressionRole` enum + `VE_LEAF`. |
| `engine/include/Veng/Project/BuildConfiguration.h` (new) | The reflected `BuildConfiguration` struct + `VE_REFLECT`/`VE_FIELD` describe-block. |
| `engine/include/Veng/Project/ProjectSettings.h` (new) | The reflected `ProjectSettings` struct + describe-block. |
| `engine/src/Project/ProjectSettings.cpp` (new) | JSON (de)serialization (name-keyed enums); the configuration-list representation. |
| `engine/CMakeLists.txt` | Add the `Project/` sources. |
| `tests/unit/…` | A device-free round-trip test: build → serialize → parse → assert equal, two configurations. |

## Verification

- Clean build; `ctest` green — the new round-trip test passes with no ICD (`unit` band).
- `include_hygiene` unaffected — the new public headers are over `Veng.h` / `Types.h` / the
  reflection headers only, no backend include (`Renderer::Format` is the Vulkan-free vocabulary enum).
- `smoke_golden` does **not** move — no cook or render path changes in this plan.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
