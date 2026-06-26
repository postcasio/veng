# Plan 03 — editor: surface the project & build settings

**Goal:** expose the new settings in the editor — a host-level **Project Settings panel** that
lists/edits the build configurations and the active one, and a **compression-role combo** on the
texture editor that writes the `*.tex.json` `role` key and shows the *resolved* format read-only. Both
ride existing machinery: the reflected structs draw through `DrawFieldWidget`/`PropertyTable` for
free, and the role key rides the texture editor's unknown-key-preserving round-trip. **Depends on
Plans 00, 01.**

## Why it is its own plan

This is the *authoring* surface for the data model — distinct from the cook (Plans 01–02) that
*consumes* it and the preview gate (Plan 04) that *protects* the editor from it. It is almost entirely
reuse: `ProjectSettings`/`BuildConfiguration` are reflected (Plan 00), so the panel is a thin host
view over the existing reflection inspector, and the texture editor already round-trips `*.tex.json`
preserving unknown keys (planset-14). Keeping it separate from the gate (Plan 04) lets the surfacing
land and be used before the host-capability logic is wired.

## What lands

- **A Project Settings panel** (`EditorPanel`, host-level). Opened from the **Window menu** like the
  asset browser; it edits the host-owned `ProjectSettings` — the list of `BuildConfiguration`s and the
  `ActiveConfiguration` selector — through the reflection inspector (`DrawFieldWidget` /
  `PropertyTable`), the same pattern as `LevelRenderSettings` and the component inspector. Each
  configuration's `RoleToFormat` table draws a **format combo per role** (the `VE_LEAF(FieldClass::Enum)`
  fields from Plan 00), so no new inspector machinery is added. Saving writes the `project.veng` /
  `*.buildcfg` JSON through Plan 00's serializer.

- **A compression-role combo on the texture editor.** `TextureEditorPanel` (planset-14) gains a
  **role** combo beside its sRGB / sampler controls, defaulting to a guess from `srgb`/usage (the same
  heuristic Plan 01 uses). Selecting a role writes the `role` key into the `*.tex.json` over the
  existing **preserve-unknown-keys** round-trip; clearing it removes the key (reverting to the guess).
  A raw `codec` override, if present, is shown but not authored here (it stays the hand-edited escape
  hatch).

- **The resolved-format read-out.** Beside the role combo, a **read-only** line shows the format the
  texture *resolves to* under the active configuration — `"→ ASTC4x4 (Srgb) for active config 'macos'"`
  — computed from the role + the active `BuildConfiguration.RoleToFormat`. This is the author's
  WYSIWYM feedback: they pick intent, the line shows the concrete format the active configuration maps
  it to. (Whether that format is *previewable on this GPU* is Plan 04's concern; this plan shows the
  resolution, not the gate.)

- **The editor host owns a `ProjectSettings`.** The editor loads `project.veng` at startup (or a
  default empty one), holds it on the host, and threads the active `BuildConfiguration` into the
  cook-on-demand `CookBackend` so an editor recook resolves roles the same way the build does. (The
  *host-safe clamping* of that configuration for live preview is Plan 04 — here the active
  configuration flows through directly.)

## Decisions

1. **The settings panels are reflection-driven, not bespoke.** `ProjectSettings` /
   `BuildConfiguration` draw through `DrawFieldWidget`/`PropertyTable`; the panel is a thin host view.
   This is the whole reason Plan 00 made them reflected structs.
2. **The texture editor authors a role, never a codec.** The combo writes the `role` key; the raw
   `codec` escape hatch stays hand-edited. The author declares intent; the configuration owns the
   format.
3. **The role key rides the existing round-trip.** Writing/clearing `role` reuses planset-14's
   unknown-key-preserving `*.tex.json` save, so a hand-added key (a raw `codec`, a future field) is
   never clobbered.
4. **The resolved-format read-out is computed, not stored.** It is `RoleToFormat[role]` under the
   active configuration, recomputed on display — no second source of truth to drift from the cook.
5. **The active configuration flows through the editor cook unclamped here.** Host-capability
   clamping for live preview is Plan 04; this plan establishes the path and the surfacing.

## Files

| File | Change |
|---|---|
| `editor/src/Panels/ProjectSettingsPanel.{h,cpp}` (new) | The host-level panel over `ProjectSettings` via the reflection inspector; Window-menu entry; save through Plan 00's serializer. |
| `editor/src/Panels/TextureEditorPanel.cpp` | The role combo (default-guessed), the `role`-key round-trip write/clear, the resolved-format read-out. |
| `editor/src/EditorHost.{h,cpp}` | Own a `ProjectSettings`; load `project.veng`; thread the active `BuildConfiguration` into the `CookBackend`. |
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
