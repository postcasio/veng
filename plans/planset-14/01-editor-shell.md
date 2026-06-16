# Plan 01 — `libveng_editor` shell + host

**Goal:** create `libveng_editor` (the editor framework library), the `EditorHost`
`Application` subclass with a top-level ImGui dockspace, stub built-in panels (asset browser,
inspector, console/log), the `veng_add_editor` CMake macro, and a `hello_triangle-editor`
executable that compiles, launches, and shows a docked UI with the hello-triangle scene
rendered into a viewport panel via `SceneRenderer`.

## New library: `libveng_editor`

Create `editor/` at the repo root, parallel to `engine/`, `cooker/`, `examples/`. It is
structured as:

```
editor/
  CMakeLists.txt
  include/VengEditor/
    EditorPanel.h
    EditorRegistry.h
    EditorHost.h
  src/
    EditorHost.cpp
    panels/
      AssetBrowserPanel.cpp
      InspectorPanel.cpp
      ConsolePanel.cpp
    ...
```

`libveng_editor` is a `SHARED` library. It links:
- `veng::veng` PRIVATE (the engine runtime).
- ImGui PRIVATE (already fetched by the engine's `FetchContent`; editor links the same
  target).

`libveng_cook` is **not** linked here — cook-on-demand plumbing lands in plan 03. This plan
delivers the shell only.

Add `add_subdirectory(editor)` to the top-level `CMakeLists.txt`, gated behind
`PROJECT_IS_TOP_LEVEL` (same gate as tests/examples/tools).

### `EditorPanel`

```cpp
// editor/include/VengEditor/EditorPanel.h
class EditorPanel
{
public:
    virtual ~EditorPanel() = default;
    [[nodiscard]] virtual string_view Title() const = 0;
    virtual void OnImGui() = 0;
};
```

No other state — open/close toggle, dock id, and Window menu wiring are owned by
`EditorHost`.

### `EditorRegistry`

This planset fully defines `EditorRegistry`, filling in the forward declaration already
present in `engine/include/Veng/Module/Module.h`. The definition lives in `libveng_editor`
and is never seen by `libveng`. It holds:

- An `AssetType`→`Unique<AssetEditorFactory>` map — double-click an asset opens its editor
  panel. `AssetEditorFactory` is a pure-virtual factory: `OpenEditor(AssetId) →
  Unique<EditorPanel>`.
- A `vector<Unique<EditorPanel>>` for game-contributed custom panels (registered via
  `RegisterPanel`).
- A `FieldClass`/`TypeId`-keyed map for custom inspector widgets (`RegisterFieldWidget`) —
  consumed by the inspector in plan 02.

```cpp
// editor/include/VengEditor/EditorRegistry.h
class EditorRegistry
{
public:
    void RegisterAssetEditor(AssetType type, Unique<AssetEditorFactory> factory);
    void RegisterPanel(Unique<EditorPanel> panel);
    void RegisterFieldWidget(TypeId type, FieldWidgetFn widget);
};
```

`FieldWidgetFn` is defined here as `function<void(void* fieldPtr, const FieldDescriptor&)>`.
It is declared but unused until plan 02.

### `EditorHost`

`EditorHost` is an `Application` subclass. It owns:

- The `TypeRegistry` and `ApplicationRegistry` (same host-side pattern as the launcher).
- The `EditorRegistry` instance.
- A `LoadedModule` from `ModuleLoader::Load(modulePath)`.
- A panel registry (`vector<Unique<EditorPanel>>`) and open-state map.
- The `SceneRenderer` for the scene viewport.

```cpp
struct EditorHostInfo
{
    path GameModulePath;          // path to libgame.so / libhello_triangle.dylib
    optional<path> EditorModulePath; // optional libgame_editor (nullptr → skip)
    ApplicationInfo App;
};

class EditorHost : public Application
{
public:
    static Unique<EditorHost> Create(const EditorHostInfo& info);
    ...
};
```

`EditorHost::OnInitialize` sequence:
1. Load the game module via `ModuleLoader::Load(info.GameModulePath)`.
2. Build `VengModuleHost { App: m_AppRegistry, Types: GetTypeRegistry(), Editor: &m_EditorRegistry }`.
3. Call `LoadedModule::Register(host)` — the module registers its Application factory +
   type descriptors + (if the editor module path is given) editor panels and asset editors.
4. Construct the `SceneRenderer` (for the scene viewport panel).
5. Register built-in panels: `AssetBrowserPanel`, `InspectorPanel`, `ConsolePanel`.
6. Any game-registered panels from `m_EditorRegistry`.

`EditorHost::OnRender`:
1. Begin the ImGui dockspace: `ImGui::DockSpaceOverViewport()`.
2. Draw the menu bar: File | Window (panel toggles by title).
3. For each open panel: `ImGui::Begin(panel->Title()); panel->OnImGui(); ImGui::End()`.

#### Scene viewport panel

This plan delivers a **`SceneViewportPanel`** that owns a `SceneRenderer`, drives it with a
hardcoded `Scene` + `Camera` (the hello-triangle prefab scene, loaded from the pack), and
shows `GetOutput()` in an `ImGui::Image`. The panel updates the renderer on resize (debounced
— don't recreate the renderer every frame the user drags the resize handle; collect the new
size and only Resize on the next frame if it changed).

The hello-triangle `Scene` construction moves from `main.cpp` into `EditorHost`/
`SceneViewportPanel` so the editor runs real geometry. The windowed `hello_triangle-launcher`
retains its own scene construction — no shared state.

#### Built-in stub panels

- **`AssetBrowserPanel`**: lists the mounted archive's asset table (id, type, name if
  available). Read-only this plan. Selecting an asset sets the inspector's selected asset
  id; double-clicking calls the registered `AssetEditorFactory` for that type (no-op for
  unregistered types this plan).
- **`InspectorPanel`**: stub — shows a "Nothing selected" message. Wired properly in plan 02.
- **`ConsolePanel`**: mirrors `Log::` output to a scrollable ImGui text area. Wire a
  `Log::SetSink` callback that appends to a ring buffer; auto-scroll on new entries.

## `veng_add_editor` CMake macro

Add to `cmake/Editor.cmake` and include it from the top-level `CMakeLists.txt`:

```cmake
# veng_add_editor(<name>
#     GAME_MODULE    <game target>        # libgame to dlopen at runtime
#     [EDITOR_MODULE <editor target>]     # optional libgame_editor
# )
#
# Produces lib<name>_editor (SHARED, links veng_editor::veng_editor) and
# <name>-editor (exe, links veng::veng, veng_editor::veng_editor, veng_cook::veng_cook).
# The editor exe is placed beside the game launcher binary; both resolve the game
# module via @loader_path / $ORIGIN.
```

The macro sets `VENG_EDITOR_GAME_MODULE` and `VENG_EDITOR_EDITOR_MODULE` compile
definitions on the editor exe so it knows the module file names at compile time (same
pattern as `veng_add_game`'s `VENG_GAME_MODULE`).

## `hello_triangle-editor` executable

`examples/hello-triangle/CMakeLists.txt` gains:

```cmake
veng_add_editor(hello_triangle
    GAME_MODULE   hello_triangle
    EDITOR_MODULE hello_triangle_editor)   # added in this plan
```

`libhello_triangle_editor` is a new SHARED target in the hello-triangle example:
`hello_triangle_editor.cpp`. It implements `VengModuleRegister` (with ABI export) and
registers:
- The `Spinner` component inspector (just shows its `Speed` field — used as a smoke test for
  the inspector in plan 02).
- No custom asset editors this plan.

The editor exe is placed beside `hello_triangle-launcher` so they share the game module
resolution path.

## Verification

- `cmake --build build -j 2` — clean build, no new warnings in `libveng_editor` or the
  editor exe.
- `./build/examples/hello-triangle/hello_triangle-editor` — launches, shows the dockspace,
  scene viewport renders the hello-triangle scene, panels are toggleable from the Window menu.
  *(Manual; no headless path for the editor this planset.)*
- `ctest --test-dir build --output-on-failure` — existing tests unchanged (the smoke launcher
  is `hello_triangle-launcher`, not the editor exe; `smoke_golden` unchanged).
- `include_hygiene` green — no new Vulkan/GLFW includes in `libveng`'s public headers.

## Acceptance

Clean build; `hello_triangle-editor` launches and shows a dockspace with scene viewport,
asset browser, inspector (stub), and console panels; `hello_triangle-launcher` smoke writes
a correct-sized PPM; default `ctest` green. Commit: `Plan 01: libveng_editor shell,
EditorHost dockspace, SceneViewportPanel, veng_add_editor macro`.
