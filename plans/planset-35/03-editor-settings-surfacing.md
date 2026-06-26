# Plan 03 — editor: surface the project & build settings

**Goal:** expose the new settings in the editor — a host-level **Project Settings panel** that
lists/edits the build configurations and the active one, and a **compression-role combo** on the
texture editor that writes the `*.tex.json` `role` key and shows the *resolved* format read-only. The
per-field drawing comes from reflection (`DrawFieldWidget`/`PropertyTable` over Plan 00's structs);
this plan adds the editor machinery reflection does not give for free — named combos for the two enums,
an add/remove widget for the configuration array, and the JSON write path. **Depends on Plans 00, 01.**

## Why it is its own plan

This is the *authoring* surface for the data model — distinct from the cook (Plans 01–02) that
*consumes* it and the preview gate (Plan 04) that *protects* the editor from it. Most of it is reuse —
`ProjectSettings`/`BuildConfiguration` are reflected (Plan 00), so each field draws through the
existing inspector, and the texture editor already round-trips `*.tex.json` preserving unknown keys
(planset-14) and writes JSON through its own nlohmann. But it is **not** zero new machinery: the
generic enum widget draws an editable integer, not a named combo (a readable combo needs a registered
`RegisterFieldWidget`, exactly as `LightType` has one), and the new `FieldClass::Array` config list
needs an add/remove array widget. Keeping it separate from the gate (Plan 04) lets the surfacing land
and be used before the host-capability logic is wired.

## What lands

- **A Project Settings panel** (`EditorPanel`, host-level). Opened from the **Window menu** like the
  asset browser; it edits the host-owned `ProjectSettings` — the list of `BuildConfiguration`s and the
  `ActiveConfiguration` selector — through the reflection inspector (`DrawFieldWidget` /
  `PropertyTable`), the same pattern as `LevelRenderSettings` and the component inspector. Two pieces
  of new editor machinery make it usable: (a) **registered combo widgets** for `CompressionRole` and
  `CompressionFormat` (`RegisterFieldWidget(TypeIdOf<…>(), …)` driven by Plan 00's name tables —
  mirroring the existing `LightType` combo, since the generic enum widget is an editable integer); and
  (b) an **array field widget** for the `FieldClass::Array` configuration list (add / remove / select
  a configuration). Each configuration's `RoleToFormat` table then draws a readable format combo per
  role. Saving writes the `project.veng` / `*.buildcfg` JSON through the editor's own nlohmann (the
  same path that round-trips `*.tex.json`), using Plan 00's enum name tables.

- **A compression-role combo on the texture editor.** `TextureEditorPanel` (planset-14) gains a
  **role** combo beside its sRGB / sampler controls, defaulting to the same `srgb` guess Plan 01 uses
  (sRGB → `Color`, non-sRGB → `Mask`). Selecting a role writes the `role` key into the `*.tex.json`
  over the existing **preserve-unknown-keys** round-trip; clearing it removes the key (reverting to the
  guess). The raw `compression` escape-hatch key, if present, is shown but not authored here (it stays
  the hand-edited override).

- **The resolved-format read-out.** Beside the role combo, a **read-only** line shows the format the
  texture *resolves to* under the active configuration — `"→ ASTC4x4Srgb for active config 'macos'"`
  — computed from the role + the active `BuildConfiguration.RoleToFormat`. The author picks intent and
  the line shows the concrete format the active configuration maps it to. (Whether that format is
  *previewable on this GPU* is Plan 04's concern; this plan shows the resolution, not the gate.)

- **The editor host owns a `ProjectSettings`.** The editor loads `project.veng` at startup (or a
  default empty one), holds it on the host, and threads the active `BuildConfiguration` into the
  cook-on-demand `CookBackend` so an editor recook resolves roles the same way the build does. (The
  *host-safe clamping* of that configuration for live preview is Plan 04 — here the active
  configuration flows through directly.)

## Decisions

1. **Per-field drawing is reflection-driven; the panel adds the combos, the array widget, and save.**
   `ProjectSettings` / `BuildConfiguration` fields draw through `DrawFieldWidget`/`PropertyTable`, so
   the panel is mostly a host view — but the two enum combos (registered like `LightType`) and the
   configuration-array add/remove widget are real new editor code, and the JSON save goes through the
   editor's nlohmann. Reflection carries the rows; it does not carry the named combos or the list UI.
2. **The texture editor authors a role, never a codec.** The combo writes the `role` key; the raw
   `compression` escape hatch stays hand-edited. The author declares intent; the configuration owns the
   format.
3. **The role key rides the existing round-trip.** Writing/clearing `role` reuses planset-14's
   unknown-key-preserving `*.tex.json` save, so a hand-added key (a raw `compression` override, a
   future field) is never clobbered.
4. **The resolved-format read-out is computed, not stored.** It is `RoleToFormat[role]` under the
   active configuration, recomputed on display — no second source of truth to drift from the cook.
5. **The active configuration flows through the editor cook unclamped here.** Host-capability
   clamping for live preview is Plan 04; this plan establishes the path and the surfacing.

## Files

| File | Change |
|---|---|
| `editor/src/panels/ProjectSettingsPanel.{h,cpp}` (new) | The host-level panel over `ProjectSettings` via the reflection inspector + the config-array add/remove widget; Window-menu entry; JSON save via the editor's nlohmann. |
| `editor/src/panels/InspectorPanel.cpp` | Register `CompressionRole` / `CompressionFormat` combo widgets (driven by Plan 00's name tables), mirroring the existing `LightType` registration. |
| `editor/src/FieldWidget.{h,cpp}` | An array field widget for `FieldClass::Array` (add / remove / reorder rows). |
| `editor/src/panels/TextureEditorPanel.cpp` | The role combo (default-guessed), the `role`-key round-trip write/clear, the resolved-format read-out. |
| `editor/src/EditorHost.{h,cpp}` | Own a `ProjectSettings`; load `project.veng` (nlohmann); thread the active `BuildConfiguration` into the `CookBackend`. |
| `editor/src/…` (menu) | The Window-menu entry opening the Project Settings panel. |

## Verification

- Clean build; `ctest` green — editor targets build; the `gpu`/editor smoke paths are unaffected.
- Manual editor check (recorded in the plan's notes, not a golden): the Project Settings panel lists
  hello-triangle's macOS/Windows configurations and edits the active one; the texture editor's role
  combo writes the `role` key and the resolved-format line tracks the active configuration.
- `include_hygiene` unaffected — the panels are editor-internal (`libveng_editor` / the editor exe);
  no public engine header change.
- `smoke_golden` does **not** move — no runtime render-path change.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
