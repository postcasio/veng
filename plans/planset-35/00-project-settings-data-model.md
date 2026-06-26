# Plan 00 — the project-settings data model

**Goal:** stand up the **reflected data model** for build configurations — `ProjectSettings` and
`BuildConfiguration` under a new `Veng/Project/` home, the two closed enums (`CompressionRole`,
`CompressionFormat`) they are built from, the **`FieldClass::Array`** reflection support the
configuration list needs, and a device-free reflection round-trip test. This is pure data: no cook
integration (Plan 01), no CMake/CLI (Plan 02), no editor (Plans 03–04). Getting the **schema and the
role taxonomy** right here is what keeps every later plan small. **Foundational; nothing precedes it.**

## Why it is its own plan

The whole planset hangs off two structs and two closed enums. They are consumed by the cooker (Plan
01 reads `BuildConfiguration` in the cook), the CMake layer (Plan 02 selects a configuration file),
and the editor (Plans 03–04 inspect them through reflection) — so the schema must be settled and
tested in isolation before any consumer binds to it. They are reflected `VE_REFLECT` structs so the
editor inspects each field through `DrawFieldWidget`; proving the reflection round-trip device-free
here is the same foundation-first move as planset-20's `AABB` and planset-21's `Frustum`. The one
genuinely new reflection capability — an **array field** for the configuration list — also lands and
is proven here, since both the editor panel (Plan 03) and the binary round-trip depend on it.

## What lands

- **`CompressionRole` — the closed intent taxonomy.** A `VE_LEAF(FieldClass::Enum)` enum naming a
  texture's *intent*, never a codec: `Color`, `Normal`, `Mask`, `HDR`, `UI`. The set is closed and
  durable — a role's *format* fills in as codecs arrive (today every role resolves to a shipped
  `BC7`/`ASTC4x4` format; `Normal → BC5` lands when BC5 does), but the role itself is the stable
  authoring surface. `Color` ⇒ the sRGB-aware albedo codec; `Normal`/`Mask`/`UI` ⇒ the unorm codec;
  `HDR` ⇒ uncompressed `RGBA16Sfloat` (the LDR block codecs cannot carry HDR — settled, not deferred).

- **`CompressionFormat` — the closed codec-output taxonomy.** A `VE_LEAF(FieldClass::Enum)` enum
  holding *only the formats a role table may resolve to* — `RGBA8Unorm`, `RGBA8Srgb`, `BC7Unorm`,
  `BC7Srgb`, `ASTC4x4Unorm`, `ASTC4x4Srgb` (plus `RGBA16Sfloat` for `HDR`). This is **not**
  `Renderer::Format`: the full vocabulary enum carries depth/stencil/swapchain/index formats that are
  nonsensical (and dangerous) as a texture codec, so a `RoleToFormat` combo over it would let an author
  pick `D32Sfloat` for `Color`. `CompressionFormat` is the small closed set the table can legally hold;
  a free `CompressionFormat → Renderer::Format` mapping (one exhaustive switch, Vulkan-free) lowers it
  to the engine format at cook time. Both this and `CompressionRole` ship a canonical **enum⇄name
  table** (`string_view ToString(...)` / `optional<...> Parse(...)`) — pure, no JSON dependency — that
  the cooker's parser (Plan 01), the editor's JSON writer (Plan 03), and the editor combos (Plan 03)
  all share.

- **`BuildConfiguration` — a named ship target (reflected).** Fields: a `Name` (`string`, the
  configuration's id, e.g. `"macos"`), a `Target` triple/label (`string`, e.g. `"macos-arm64"`), the
  **`RoleToFormat` table** (a `CompressionRole → CompressionFormat` mapping), a `CompressionLevel`
  (the zstd level for the archive, an `i32`), and an `OutputSuffix` (`string`, the **single source of
  truth** for the per-config pack suffix — `sample` + `-macos` → `sample-macos.vengpack`; CMake reads
  it from the config file rather than re-declaring it, Plan 02). The `RoleToFormat` mapping is a fixed
  small record (one `CompressionFormat` field per `CompressionRole` enumerator), so it reflects as
  plain leaf-enum fields and the editor draws a format combo per role from the shared name table — no
  array support is needed *for the table itself* (only the configuration list below needs it).

- **`ProjectSettings` — one per project (reflected).** Fields: `Configurations` (the set of build
  configurations) and `ActiveConfiguration` (`string`, the configuration the editor previews through
  / the default). Project-wide invariants only — **no codec here**; the codec lives on the
  configuration. `Configurations` is a genuine reflected **`FieldClass::Array`** of
  `BuildConfiguration` (see below), so the editor lists/adds/removes configurations through reflection
  rather than a fixed-capacity hack.

- **`FieldClass::Array` reflection support.** The configuration list is the one unavoidable container,
  and the reflection layer does not have an array field today. This plan adds it: a new
  `FieldClass::Array` variant carrying an element `TypeId` + element-access shims (size / element-ptr /
  resize) in the `FieldDescriptor`, a `VE_FIELD`-style describe spelling for a `vector<T>` member, and
  the `WriteFields`/`ReadFields` length-prefixed array case so a reflected array round-trips. This is
  the single new reflection capability; the editor's array *widget* (add/remove rows) is Plan 03's job
  over this data layer.

- **JSON authoring format, parsed by consumers — not by `libveng`.** `ProjectSettings` lives in
  `project.veng`; each `BuildConfiguration` it lists lives in a sibling `*.buildcfg`. These are
  **JSON authoring files**, handled exactly like `*.tex.json`/`*.vmat.json`: the **cooker hand-parses**
  them into the struct (Plan 01) and the **editor writes** them through its own nlohmann (Plan 03).
  `libveng` ships **no** JSON serializer — its reflection serializer is a binary in-memory seam, not an
  on-disk format, and `libveng` links no JSON library by design. The role/format enums serialize by
  **name** (`"Color"`, `"ASTC4x4Srgb"`) via the shared name table, the established string-keyed
  convention, never by ordinal.

- **A device-free reflection round-trip test.** A unit test (the `unit` suite, no ICD, no nlohmann)
  builds a `ProjectSettings` with two configurations (a macOS/ASTC and a Windows/BC7 one), runs it
  through `WriteFields`→`ReadFields`, and asserts equality — guarding the describe-blocks, the new
  `FieldClass::Array` round-trip, and the enum reflection. A second device-free test round-trips the
  enum⇄name tables (`ToString`→`Parse`). The JSON name-keyed parse is tested in the cooker suite where
  the hand-parser lands (Plan 01).

## Decisions

1. **The role taxonomy is closed and settled here:** `Color / Normal / Mask / HDR / UI`. Adding a
   role is a deliberate reflection-enum change, not an open-ended string. A role names *intent*; its
   per-codec format is the configuration's to decide.
2. **`HDR` resolves to uncompressed `RGBA16Sfloat`, not a block codec.** The cooked block codecs
   (`BC7`/`ASTC4x4`) are LDR; there is no HDR compressed path this planset, so the `HDR` role is the
   honest uncompressed mapping, not a deferred TODO. HDR ASTC stays a future footprint item.
3. **The role table holds `CompressionFormat`, not `Renderer::Format`.** The table can legally name
   only a codec output, so it is typed to the small closed `CompressionFormat` enum and lowered to
   `Renderer::Format` by one exhaustive switch at cook time. This makes the editor combo short and
   makes an impossible mapping (a depth or swapchain format as a codec) unrepresentable rather than
   merely discouraged.
4. **The structs live in `libveng` (`Veng/Project/`); JSON lives in the consumers.** The reflected
   struct is the editor's inspection schema and the canonical type. `libveng` ships no JSON serializer
   (it links no JSON library, and its reflection serializer is a binary in-memory seam); the cooker
   hand-parses the authoring JSON (as it does every source) and the editor writes it through its own
   nlohmann (as it round-trips `*.tex.json`). The shared enum⇄name tables (pure, in `libveng`) are the
   one thing both sides reuse.
5. **Enums serialize by name, never by ordinal.** Unlike the cooked-blob integer-stability rule
   (which pins `Renderer::Format` ordinals in the binary header), these JSON authoring files are
   human-edited and use the enum *name*, so a reordering never silently repoints a configuration.
6. **The configuration list is a real reflected array.** This plan adds `FieldClass::Array` to the
   reflection layer so `ProjectSettings.Configurations` is a genuine reflected field — no
   fixed-capacity cap, and Plan 03's panel adds/removes configurations through reflection. This is the
   single new reflection capability the planset introduces, proven device-free here.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Project/CompressionRole.h` (new) | The closed `CompressionRole` enum + `VE_LEAF` + its `ToString`/`Parse` name table. |
| `engine/include/Veng/Project/CompressionFormat.h` (new) | The closed `CompressionFormat` enum + `VE_LEAF` + name table + the exhaustive `CompressionFormat → Renderer::Format` lowering. |
| `engine/include/Veng/Project/BuildConfiguration.h` (new) | The reflected `BuildConfiguration` struct + `VE_REFLECT`/`VE_FIELD` describe-block (`RoleToFormat` is `CompressionFormat` per role). |
| `engine/include/Veng/Project/ProjectSettings.h` (new) | The reflected `ProjectSettings` struct (`Configurations` an array field) + describe-block. |
| `engine/include/Veng/Reflection/{FieldDescriptor,ReflectionTypes,Reflect}.h` | Add the `FieldClass::Array` variant + element shims + the `VE_FIELD` array describe spelling. |
| `engine/src/Reflection/Serialize.cpp` | The length-prefixed `FieldClass::Array` case in `WriteFields`/`ReadFields`. |
| `engine/src/Project/…` (name tables) | The `CompressionRole`/`CompressionFormat` name tables + the format-lowering switch (no JSON). |
| `engine/CMakeLists.txt` | Add the `Project/` sources. |
| `tests/unit/…` | A device-free reflection round-trip (`WriteFields`→`ReadFields`, two configurations, exercising the array case) + an enum name-table round-trip. |

## Verification

- Clean build; `ctest` green — the new round-trip tests pass with no ICD (`unit` band), and the
  existing reflection/prefab tests still pass with the added `FieldClass::Array` case.
- `include_hygiene` unaffected — the new public headers are over `Veng.h` / `Types.h` / the
  reflection headers only, no backend include (`CompressionFormat` lowers to the Vulkan-free
  `Renderer::Format` vocabulary enum), and `libveng` still links no JSON library.
- `smoke_golden` does **not** move — no cook or render path changes in this plan.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
