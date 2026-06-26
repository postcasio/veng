# planset-35 — build configurations & the minimal game template

**Phase goal:** give the engine a **home for per-platform build policy** — the developer-control
layer planset-33 shipped the codec *plumbing* for and deliberately deferred — and, on top of it,
add the **minimal game template** that new developers copy to start. Two threads:

1. **Build configurations** (future [area 15](../future/build-configurations.md)) — a
   **project-settings** concept owning a set of **build configurations** (one per ship target:
   macOS / Windows / Linux / mobile), each holding the texture codec policy as a **role → format**
   table. A texture declares a compression **role** (its *intent* — Color / Normal / Mask / HDR / UI),
   never a raw codec; the build configuration resolves role → concrete format per platform. The cook reads the
   active configuration, emits **one output pack per configuration**, and a bare `cmake --build` on a
   host picks the **host-matching configuration by default** (build on a Mac → cook the macOS/ASTC
   pack, with no ceremony). The editor surfaces the settings through the reflection inspector and
   **gates live preview to host GPU capability**, so "ASTC on Windows" is structurally impossible.

2. **The minimal game template** — a new `examples/template/` member: the **absolute minimum** code
   to open a window and render a rotating cube. It is the canonical **starting point a new developer
   copies**, and — like `hello-triangle` — it is **migrated in the same pass as every breaking
   change** going forward. Where `hello-triangle` is the engine's *maximal* sample (every battery,
   the full debug UI, multiple build configurations), the template is its *minimal* conformance
   surface: no debug UI, no tuning panels, the smallest correct app.

## The build-configuration model

A texture's codec is **not** a per-asset decision the way `srgb` or `max_size` is — it is a
**platform** decision (`BC7` needs `textureCompressionBC`, broadly Apple-Silicon/desktop; `ASTC`
needs `textureCompressionASTC_LDR`, broad on Apple GPUs). One project ships several targets wanting
different codecs for the *same* source art, and veng has **no home** for that policy today: the pack
manifest is a pure `{ id, type, source }` table by rule, and per-asset `*.tex.json` is the wrong
scope. planset-33 papered over this by hardcoding ASTC as the cook default; this planset replaces the
hardcode with the standard cross-engine factoring — **role on the asset, format on the platform**
(Unreal `TextureCompressionSettings`, Unity per-platform overrides, Godot import/export presets):

```
  Project settings (one)            Veng/Project/ProjectSettings — reflected, JSON
    └─ the list of build configs + the editor's active/default one
       Build configuration (N)      Veng/Project/BuildConfiguration — reflected, JSON
         └─ role → concrete-format table, compression level, output pack name
            Per-asset *.tex.json     declares a role (Color/Normal/Mask/HDR/UI), not a codec
              └─ the existing raw "compression" key is the escape hatch, not the norm
```

The **gate is met by planset-33**: the `BC7`/`ASTC4x4` formats, the `FormatInfo::BytesForLevel`
block math, the `Context::IsBlockCompressionSupported()` / `IsAstcSupported()` queries, and a cooker
that already selects a codec per texture all exist. This planset adds only the *authoring surface*
that chooses the codec and the project/build-settings concept it lives on — it ships **no new
codec**. BC5/BC4 channel specialization, wider ASTC footprints (6×6, 8×8), HDR ASTC, and an
uncompressed fallback pack are **new formats + encoders**, a separate concern from the settings
tiers, and stay [named follow-ons](../future/build-configurations.md) behind this delivered surface.

## The CMake integration — host-default config selection

The cook-time model is **whole-pack and coarse, by design** (the design doc's Decision 3): the
cooker records the active configuration file as **one central depfile input** (exactly as it already
records the pack JSON), and emits **one output pack per configuration**, so editing
`windows.buildcfg` re-cooks only `sample-windows.vengpack` — per-config invalidation falls out for
free, no fine-grained per-asset edges. On top of [`add_asset_pack`](../../cmake/AssetPack.cmake) the
new `--config` flag is a one-argument extension of the existing custom command.

The piece the design doc left open — and the one explicitly asked for — is **which configuration a
bare `cmake --build` selects**. A `VENG_BUILD_CONFIG` cache variable **defaults from the host triple**
(`CMAKE_HOST_SYSTEM_NAME` + `CMAKE_HOST_SYSTEM_PROCESSOR`), so a fresh `cmake -B build && cmake
--build build` on an Apple-Silicon Mac picks the macOS/ASTC configuration with zero ceremony. A
developer overrides with `-DVENG_BUILD_CONFIG=windows` to cook the BC7 pack, and a `cook-all-packs`
aggregate target builds every configuration's pack at once for CI / ship. This keeps the
**zero-config default** intact (no project settings → planset-33's hardcoded ASTC) while making the
host-matched configuration the natural one.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Project-settings data model | `Veng/Project/`: reflected `ProjectSettings` (the config list + active/default) and `BuildConfiguration` (a `CompressionRole → CompressionFormat` table + target + output suffix), the two closed `VE_LEAF(FieldClass::Enum)` enums + shared name tables, new **`FieldClass::Array`** reflection for the config list, and a device-free reflection round-trip test. JSON lives in the consumers (cooker hand-parse / editor nlohmann), not `libveng`. The closed `CompressionRole`/`CompressionFormat` taxonomies are settled here. No cook/editor wiring yet. | proposed |
| 01 | Compression roles → cook resolution | `*.tex.json` gains a `role` key (intent) layered over the **existing** raw `compression` escape hatch (no new key, no fixture migration); `CookContext` gains `const BuildConfiguration* Config`; `TextureImporter` resolves `role → format` through it (raw override on top, planset-33's hardcoded ASTC the zero-config fallback) and applies the config's `CompressionLevel`. Records the config file as a central depfile input. Depends on 00. | proposed |
| 02 | CMake config selection | `vengc cook … --config <file>`; `add_asset_pack` grows a `CONFIG` dimension (one output pack per config); a `VENG_BUILD_CONFIG` cache var **defaulting from the host triple**; a `cook-all-packs` aggregate target. hello-triangle ships a `configs/` pair (macOS/ASTC + Windows/BC7) and its cook threads the host-default config. Depends on 01. | proposed |
| 03 | Editor — surface the settings | A host-level **Project Settings panel** (Window menu) listing/editing the configs + active one through `DrawFieldWidget`/`PropertyTable` — reflection draws the rows, this plan adds the two enum combos (registered like `LightType`), the config-array add/remove widget, and the nlohmann JSON save; the **texture editor** gains a **compression-role combo** over the unknown-key-preserving round-trip, showing the *resolved* format read-only. Depends on 00, 01. | proposed |
| 04 | Editor — gate preview to host GPU | The editor's default live-cook target is **host-safe**, independent of the selected ship config; "preview as ship config" is opt-in and **disables host-incompatible configs with a reason** via `IsBlockCompressionSupported()`/`IsAstcSupported()`; a fallback banner when no config is host-previewable. Building any config stays unrestricted. Depends on 03. | proposed |
| 05 | The minimal game template | A new `examples/template/` `veng_add_game` member: the smallest app that opens a window and renders a **rotating cube** (a cube primitive recipe + one trivial material + a camera, the cube rotated inline in `OnUpdate` — no custom component, no system, no debug UI). Relies on the **zero-config cook default** to stay minimal. A build-only launcher smoke (exit 0, correct-sized capture); **no pixel golden** to keep it low-maintenance. Independent. | done |
| 06 | Docs + roadmap | Document build configurations across the `CLAUDE.md` set + the root, document the template + its co-migration rule, mark future area 15 delivered (footprint items stay future), run the full verification band. The closer. Depends on 00–05. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is foundational (the reflected structs + JSON schema), pure and device-free.
- **01** depends on 00 (it consumes `BuildConfiguration` in the cook); **02** depends on 01 (the
  CMake/CLI layer threads the config the cooker now reads). **00 → 01 → 02** is the cooker/CMake
  chain — merge in number order; 01 and 02 both touch the **cook entry** (`CookContext` threading in
  [`cooker/src/Cooker.cpp`](../../cooker/src/Cooker.cpp) and the `--config` flag in
  [`cooker/tool/main.cpp`](../../cooker/tool/main.cpp)), sequential by construction. Only 01 touches
  [`cooker/src/Importers/TextureImporter.cpp`](../../cooker/src/Importers/TextureImporter.cpp).
- **03** depends on 00 (structs to inspect) + 01 (the `role` key it writes); **04** depends on 03.
  **03 → 04** is the editor chain, merging cleanly beside the cooker chain.
- **05** is **independent** of the build-config work — it relies on the zero-config cook default and
  builds on planset-34's mesh-source unification (a cube is a recipe) and the current engine. It can
  land in parallel.
- **06** is the closer, depending on 00–05.

Dependent plans must build on the **prior plan's integration commit**, not `origin/main`. Per
[[project_megaexec_worktree_base]], `isolation: "worktree"` branches from `origin/main` and will not
see a locally-committed-but-unpushed base: dispatch **01** against a manual worktree cut from **00**'s
integration commit, **02** against the 00→01 chain, **03** against **01**'s (it depends on 00 + 01),
and **04** against **03**'s. Only the genuinely independent plans — **00** (foundational) and **05**
(zero-config template) — can use `isolation: "worktree"` directly.

## The decisions this planset settles

- **Codec is a build-configuration property, not a project default or a per-asset knob.** Per-platform
  by nature; the project owns the *list* of configurations, the configuration owns the codec policy.
- **A per-asset texture declares a role, not a raw codec.** The build configuration maps role → format
  per platform; the existing raw `"compression"` key stays as the escape hatch for the rare case.
- **The build-time config dependency is implicit and coarse.** A central depfile input over a
  whole-pack cook, one output pack per configuration. No fine-grained per-asset edges; a
  content-addressed per-asset cook cache stays a separate, orthogonal cooker optimization.
- **A bare `cmake --build` cooks the host-matching configuration.** `VENG_BUILD_CONFIG` defaults from
  the host triple; building on a Mac cooks the macOS/ASTC pack with no flag, an explicit override
  cooks a foreign config (CPU-only, always allowed), and `cook-all-packs` builds them all.
- **The editor previews host-safe by default and gates preview-through-config on device features.**
  Building any configuration is unrestricted (the encoder is CPU); previewing one is bounded by what
  the host GPU can sample. The failure is made structurally impossible, not merely warned against.
- **The engine ships two co-migrated examples: a maximal one and a minimal one.** `hello-triangle`
  exercises every capability; `examples/template/` is the smallest correct app a new developer copies.
  A breaking change migrates **both** in the same pass — the template is the minimal conformance check.
- **The template stays minimal by relying on the zero-config default.** It demonstrates the smallest
  app, not the build-config surface; `hello-triangle` is the multi-configuration demonstration. A
  template `README` points a new developer at the Project Settings panel and `hello-triangle` for the
  build-config workflow.

## What remains future

The **still-open footprint work** rides future [area 15](../future/build-configurations.md)'s open
questions, since the right home for each is the role → format table a build configuration owns —
they are **new formats + encoders**, not settings-tier work, so they stay out of this planset:
**BC5/BC4 channel specialization** (`Normal → BC5`, `Mask → BC4` rather than full BC7/ASTC); **wider
ASTC footprints** (6×6, 8×8 — a per-role quality/size choice); **HDR ASTC** (the cooked codec is
LDR-only; HDR sources keep their `RGBA16Sfloat` panorama path); and an **uncompressed fallback pack**
for a device supporting neither cooked codec (today such a device gets `AssetError::Unsupported`).
Also future: **editor active-config persistence** (per-project vs per-user state) and the **Windows
cross-compile constraint** (a foreign-platform pack is CPU-only and fine, but `--module` prefab
reflection still loads a *host* lib — area 10's recorded cross-compiled-cooking limit).
