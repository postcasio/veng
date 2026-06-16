# Plan 05 — docs + roadmap re-cut

**Goal:** update the roadmap and CLAUDE.md to reflect planset-14's deliverables. No code
changes.

## `plans/README.md`

Add the planset-14 entry after planset-13:

> **planset-14** — editor shell + framework (✅ done, 5 plans). Delivers [future area 6](future/README.md#6-editor-application)
> sub-area B: `libveng_editor` (the editor framework library with `EditorPanel`,
> `EditorRegistry`, `EditorHost`), a `veng_add_editor` CMake macro, a dockspace host with
> built-in panels (scene viewport, asset browser, inspector, console/log), a
> reflection-driven inspector wired to `TypeRegistry`/`FieldDescriptor` (widgets per
> `FieldClass`, live entity component editing), cook-on-demand plumbing (`libveng_cook`
> linked into the editor host, off-thread `CookSession` via `TaskSystem`,
> `AssetManager::MountMemory` hot-reload), and the **texture editor** as the first
> end-to-end asset editor (preview RT, `.tex.json` settings editing, live recook on change,
> JSON round-trip on save). `hello_triangle-editor` launches with `libhello_triangle`,
> shows the scene in a docked viewport, and opens the brick texture in the texture editor.
> Held back as named future plansets: the material node editor (sub-area C), the scene
> editor (sub-area D), undo/redo command stack, multi-viewport OS windows.

## `plans/future/README.md`

### Area 6 — update

Mark sub-area B delivered. The remaining open sub-areas:

- **Sub-area C — material node editor.** imnodes graph → loaded `.vmat` (param-binding v1),
  live preview against the cook-on-demand + hot-reload path. Both prerequisites met:
  cook-on-demand (planset-14) and the inspector foundation (planset-14). A planset of its own.
- **Sub-area D — scene editor.** Viewport panel (reuses `SceneRenderer`), hierarchy panel,
  gizmos (ImGuizmo or hand-rolled), save round-trip to `.prefab.json`. Its scene model and
  cooked prefab prerequisites are met (planset-10/11); the inspector and cook-on-demand
  foundation (planset-14) are now also in place. A planset of its own.

Update the ordering section:
```
remaining:
  6  editor (material editor sub-C → scene editor sub-D)   sub-B delivered (planset-14)
  4  events/input — independent, gameplay-driven (any time)
```

Update the `Status` paragraph to name planset-14 as delivering sub-area B and identify the
material node editor (sub-area C) as the next editor planset.

### Cross-cutting concerns

Move "The editor is the demanding second consumer" from **Open** to **Resolved** — the
editor is a real, running consumer. The note: the editor and engine API co-evolved through
planset-14, validating the multi-viewport `SceneRenderer` consumer model and the inspector
against real component types.

## `CLAUDE.md`

Add a **Editor** section covering:

- The editor host is an `Application` subclass in `libveng_editor`, linked by the
  `<name>-editor` executable (produced by `veng_add_editor`). It dlopen's the game module
  with `Editor = &m_EditorRegistry` (non-null, activating editor-side registrations).
- `EditorPanel` — `Title()` / `OnImGui()` virtual interface; the host owns open/close state
  and the dockspace.
- `EditorRegistry` — holds the `AssetType`→editor-factory map, game-contributed panels, and
  custom inspector widgets. Fully defined in `libveng_editor`; forward-declared in
  `engine/include/Veng/Module/Module.h` (unchanged).
- Cook-on-demand: `CookSession::Cook` submits to `TaskSystem`; the result is a
  `MountHandle` over an in-memory archive mounted via `AssetManager::MountMemory`. The
  `libveng` / `libgame` boundary is preserved — importers live only in the editor host.
- Texture editor as the template: preview RT via `ImGuiLayer::CreateTexture` → `ImGui::
  Image`, live recook on settings change (debounced 300ms), JSON round-trip on save (patch
  existing keys, preserve unknown keys).

Add the `veng_add_editor` macro to the **Game modules** section alongside `veng_add_game`,
noting it produces `lib<name>_editor` + `<name>-editor`.

Document `AssetManager::MountMemory` / `MountHandle` in the **Assets** section alongside
`Load`/`LoadSync`.

Document `Scene::ForEachComponent` in the **Scene & ECS** section.

## Acceptance

`plans/README.md` has the planset-14 entry; `plans/future/README.md` area 6 marks sub-area
B delivered and identifies sub-area C as next; `CLAUDE.md` documents the editor
architecture, `veng_add_editor`, `MountMemory`/`MountHandle`, and `ForEachComponent`. No
code changes. Commit: `planset-14: docs + roadmap re-cut — editor shell + framework
delivered (sub-area B), sub-area C next`.
