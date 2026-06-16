# planset-14 — editor shell + framework (area 6, sub-area B)

**Phase goal:** deliver the first tangible authoring environment — `libveng_editor` (the
editor framework library), an editor-host executable that loads a game module, a
docking-based UI with reflection-driven inspector and asset browser panels, cook-on-demand
plumbing off the render thread, and the **texture editor** as the first end-to-end slice.

This is [future area 6](../future/README.md#6-editor-application) sub-area B, following the
delivered sub-area A (game-module build model, planset-9) and the three prerequisites that
cleared between planset-9 and now: the `TypeRegistry` + module-reflection seam its inspectors
consume (planset-10/11), the `SceneRenderer` its scene viewport will eventually drive
(planset-12/13), and the async load path the live-preview loop needs (planset-6).

## Prerequisites

All in place:

- **Game-module build model** (planset-9): `libgame` + launcher, `VengModuleHost`, ABI
  version handshake, `veng_add_game` CMake macro. The `EditorRegistry*` slot in
  `VengModuleHost` exists and is currently always null — this planset fills it.
- **Module reflection** (planset-10/11): `TypeRegistry`, `FieldDescriptor`/`TypeInfo`,
  `VE_REFLECT`/`VE_FIELD`, the `TypeRegistry& Types` seam in `VengModuleHost`. The inspector
  consumes this directly.
- **Async load path** (planset-6): `TaskSystem`, off-thread `Upload`/`Load`. Cook-on-demand
  runs on a task worker; the preview loop never stalls the render thread.
- **SceneRenderer** (planset-12/13): `SceneRenderer`, `Execute`, `GetOutput`. The scene
  viewport panel is out of scope this planset but will consume `SceneRenderer` unchanged.

## Shape of the editor

```
editor-host executable
├── libveng              engine — render, load cooked archives, previews
├── libveng_editor       this planset: panels, inspector, EditorRegistry, dockspace host
├── libveng_cook         cook-on-demand (importers stay out of libveng)
└── libhello_triangle    game module — loaded at runtime via ModuleLoader
```

The editor host is an `Application` subclass. It dlopen's the game module and passes a
non-null `EditorRegistry*` so the module can register custom views; hello-triangle's editor
module is added this planset so the full path is exercised end-to-end.

**Single-window docking (v1)** — `ImGuiConfigFlags_DockingEnable` is already set in
`ImGuiLayer`. The editor host builds a top-level `DockSpace`, panels dock into it. Real
OS windows (multi-viewport) conflict with the current single-offscreen-composite model and
are explicitly deferred.

## Decisions

1. **`EditorPanel` is the base class.** `Title()` + `OnImGui()` virtual interface; the host
   owns a panel registry (open/close state, Window menu toggle, dock id). Built-in panels:
   asset browser (read-only), inspector, console/log.

2. **`EditorRegistry` is defined in `libveng_editor`.** The forward declaration already in
   `engine/include/Veng/Module/Module.h` is filled here. `EditorRegistry` holds:
   - An `AssetType`→`AssetEditorFactory` map (double-click an asset → open its editor).
   - A `RegisterPanel` entry for game-defined custom panels from `libgame_editor`.
   - A `RegisterFieldWidget` entry for custom inspector widgets per `FieldClass`.
   The host passes `&m_EditorRegistry` in `VengModuleHost::Editor` when loading a game module.

3. **Reflection-driven inspector.** Selecting an entity populates the inspector by walking
   the entity's components' `FieldDescriptor` lists. A built-in widget per `FieldClass`:
   `Scalar`(f32/int/bool) → drag/checkbox, `Vector` → drag float N, `Quaternion` → drag
   float 3 (Euler), `String` → InputText, `AssetHandle` → asset picker (name + click to
   select), `Enum` → Combo, `Struct` → collapsing nested sub-inspector. The `Hidden`
   descriptor attribute is respected (the field is not shown). Custom widgets registered via
   `RegisterFieldWidget` override the built-in for a given `FieldClass`/`TypeId`.

4. **Cook-on-demand: `libveng_cook` linked directly into the editor host.** In-process,
   off-thread via `TaskSystem`. A thin `CookRequest { path SourcePath; AssetType Type; }`
   is submitted; the worker calls the appropriate importer, packs the cooked blob into an
   in-memory scratch archive, and returns it to the main thread via the task continuation.
   The main thread hot-reloads via `AssetManager`'s existing async path (mount the scratch
   archive, reload the handle).

5. **Texture editor is the first real slice.** It exercises the full edit→cook→preview loop:
   show the decoded image in a preview RT (using `ImGuiLayer::CreateTexture`), edit
   `.tex.json` settings (sRGB, sampler min/mag/wrap/mip), trigger a live recook on change
   (debounced), and round-trip the JSON source on save. It is the template for all later
   asset editors.

6. **`veng_add_editor` CMake macro.** Parallels `veng_add_game`. Produces `lib<name>_editor`
   (SHARED, links `libveng_editor`) + `<name>-editor` (exe, links `libveng`, `libveng_editor`,
   `libveng_cook`). The editor exe dlopen's the game module (`libgame`) and optionally the
   game's editor module (`libgame_editor`) if the target exists.

7. **No undo/redo this planset.** The command stack is acknowledged as a future subsystem;
   the texture editor applies changes live with a revert-via-reload as the escape hatch.

8. **No scene editor this planset.** The scene editor (viewport + hierarchy + gizmos + save
   round-trip) is a substantial follow-on planset of its own (editor sub-area D). The
   inspector panel built here will be its foundation.

## Scope

| In scope | Out of scope |
|---|---|
| `libveng_editor`: `EditorPanel`, `EditorRegistry`, `EditorHost` (Application subclass), dockspace | Scene editor (sub-area D — its own planset) |
| `veng_add_editor` CMake macro | Material node editor (sub-area C — its own planset) |
| Built-in panels: asset browser (read-only), inspector (reflection-driven), console/log | Multi-viewport / real OS windows for torn-off panels |
| Cook-on-demand plumbing (off-thread, `TaskSystem`) | Undo/redo command stack |
| Texture editor: preview RT, `.tex.json` editing, live recook + preview, JSON round-trip | Custom vertex layouts in the asset browser |
| `libhello_triangle_editor`: registers the Spinner inspector + asset editor for the hello-triangle texture | Full prefab/scene browser |
| Update `plans/README.md`, `plans/future/README.md`, `CLAUDE.md` | — |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [`libveng_editor` shell + host](01-editor-shell.md) | New library (`EditorPanel`, `EditorRegistry`, `EditorHost`), dockspace host, built-in stub panels, `veng_add_editor` macro, editor exe compiles and launches with hello-triangle. | done |
| 02 | [Reflection-driven inspector](02-reflection-inspector.md) | Wire `TypeRegistry`/`FieldDescriptor` into the inspector panel. Widget per `FieldClass`. Selecting an ECS entity populates the inspector with its components. | done |
| 03 | [Cook-on-demand plumbing](03-cook-on-demand.md) | Link `libveng_cook` into the editor host. Off-thread `CookRequest` via `TaskSystem`, in-memory scratch archive, hot-reload via `AssetManager` on the main thread. | done |
| 04 | [Texture editor](04-texture-editor.md) | End-to-end texture editor panel: preview RT via `CreateTexture`/`ImGui::Image`, `.tex.json` settings editing, live recook on change, JSON round-trip on save. | proposed |
| 05 | [Docs + roadmap re-cut](05-docs-roadmap.md) | `future/editor.md` sub-area B → delivered; `future/README.md` area 6 updated; `CLAUDE.md` editor architecture documented; `plans/README.md` planset-14 line. No code. | proposed |

Plans 01 → 02 → 03 → 04 must run in sequence (each builds on the prior). Plan 05 runs last.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the same
pass → verify (clean build, `ctest` green, smoke PPM correct size + exit 0) → update this
table → one commit per plan.

- **`libveng_editor` links `libveng` PRIVATE and `libveng_cook` PRIVATE.** The editor host
  links both. Neither leaks into `libgame` or `libveng` — the planset-5 boundary holds.
- **`include_hygiene` stays green.** New `libveng_editor` public headers may pull in ImGui;
  they must not pull in Vulkan or GLFW. `libveng`'s public headers are unchanged.
- **The smoke golden is unchanged.** The editor is a separate executable; `hello_triangle-
  launcher` still writes the same PPM.
- **`EditorRegistry` in `Module.h` is fully defined.** The forward declaration is already
  in `engine/include/Veng/Module/Module.h`. This planset fills the definition in
  `libveng_editor`, keeping `libveng` clean.
- **Contract comments are present-tense facts.** No plan citations, no "future work"
  phrasing (CLAUDE.md).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

veng has a running editor: launch `hello_triangle-editor`, see the hello-triangle scene in a
docked viewport driven by `SceneRenderer`, click an entity to inspect its components via the
reflection-driven inspector, double-click the brick texture to open the texture editor, tweak
`.srgb` or a sampler setting, and see the preview update live without touching code. The
cook-on-demand loop is proven end-to-end. The foundation for the material node editor
(sub-area C) and the scene editor (sub-area D) is laid: `EditorRegistry`, `EditorPanel`, and
the inspector panel are all extension points those plansets build on.
