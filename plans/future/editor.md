# Editor application — design overview (future)

> **Vision / design sketch, not scheduled.** Detail for the **editor** area
> ([README](README.md)). Direction, architecture, and recommendations — not a firm
> plan; it spans **several plansets** when taken up. Builds on the shipped asset /
> bindless / material foundation (planset-5) and depends on three other future
> pieces: the [game-module build model](game-module.md), the
> [threading/async-load path](threading-task-system.md), and a **scene/entity
> model** veng does not yet have (below). The editor is the "demanding second
> consumer" the roadmap has flagged from the start
> ([README cross-cutting](README.md#cross-cutting-concerns-weigh-when-opening-each-phase)).

## Why now (and why it is the right second consumer)

hello-triangle is one pipeline and one push constant; it will never surface the
friction of many materials, many meshes, a scene graph, or per-asset authoring
settings. The **editor will** — it is the application that actually exercises the
multi-material / scene / asset surface, so building it alongside the engine keeps
that surface honest. It is also the home of the asset features deliberately left
open: the **loaded `.vmat`** path (a node graph's cooked output — planset-5 shipped
only the *constructed* material), live recook, and hot-reload.

## What the editor *is*, architecturally

The editor is a veng `Application` (the **editor host**) that **also links the
cooker** (`libveng_cook`). That last clause is the whole architectural tension:

> **The engine never links importers; the editor is a tool, so it does.** The
> runtime (`libveng`) only ever *loads cooked archives* — no stb, no assimp, no
> Slang (planset-5, decision 1). But authoring **means importing and cooking
> source assets**, live, as the user edits them. So the editor — like `vengc` — is
> a **cooker consumer**. It reads *source* assets (the JSON packs + `.tex.json` /
> `.mesh.json` / `.shader.json` / `.vmat.json` + raw images/meshes/`.slang`),
> **cooks them on demand**, and previews the cooked result through the engine's
> normal `AssetManager` load path.

```
   editor host (exe)
   ├── libveng            the engine — render, load cooked archives, previews
   ├── libveng_cook       COOK-ON-DEMAND — import source → cooked blob, live
   ├── libveng_editor     the editor framework (panels, inspectors, registries)
   ├── libgame            the game's runtime types        (game-module.md)
   └── libgame_editor     the game's editor views/tools    (game-module.md)
```

This reconciles cleanly with the pipeline planset-5 built. The editor's edit→cook→
preview loop is:

```
edit a source (e.g. brick.tex.json)  ──►  cook on demand (libveng_cook, off-thread)
   ──►  mount the cooked blob (in-memory / scratch archive)
      ──►  AssetManager hot-reloads behind the stable AssetHandle  (async, area 2)
         ──►  the preview panel re-renders with the new asset
```

"Cook on demand" was explicitly excluded from the **runtime** (planset-5 decision
2) — and it stays excluded there. It reappears *only* in the editor, which is a
tool, not a shipped runtime. The boundary the project drew (importers never reach
`libveng` or its consumers) is preserved exactly: `libgame` never links the cooker;
the editor does.

## Docking & window management

The ImGui groundwork is already in place. ImGui is pinned to **`v1.92.4-docking`**
and `ImGuiConfigFlags_DockingEnable` is already set in `ImGuiLayer`; **imnodes** is
already vendored (the material node editor's library is in the tree). The editor
host builds a top-level **`DockSpace`** over the main viewport, panels dock into it,
and the layout persists via `imgui.ini` with a shipped default layout.

### Previews reuse the pattern the sample already proves

Each preview surface — scene viewport, material preview, texture view — renders
into its **own offscreen render target**, then is shown inside a dock panel via
`ImGuiLayer::CreateTexture(sampler, view)` → `ImGui::Image(textureId, size)`. This
is **exactly** the path hello-triangle already uses for its scene texture
(`m_SceneImageView` → `CreateTexture` → `ImGui::Image`, then `ImGuiLayer::Render`
records the sampled read, then `CompositeToSwapChain`). The editor generalizes one
preview to N. A panel that is resized recreates its render target (debounced) and
retires the old one through the existing per-frame deferred-destruction queue —
no special teardown.

### The real decision: multi-viewport vs. single-window docking

Multi-viewport (ImGui spawning **real OS windows** for torn-off panels) is
scaffolded behind a `VE_VIEWPORTS` compile flag but **off** — and it conflicts with
veng's current rendering model. Today the ImGui layer renders the whole UI into a
**single offscreen image** that the app composites into the **one** swapchain
(`ImGuiLayer::GetOutputImage`). Multi-viewport instead needs ImGui's platform/
renderer backend to own **a swapchain per OS window** — incompatible with the
single-offscreen-composite indirection. Two routes:

1. **Single-window docking (recommended v1).** Every panel lives inside one OS
   window's dockspace; "torn-off" panels are floating ImGui windows *within* the
   main viewport, not OS windows. This matches today's compositing model exactly,
   stays clean on MoltenVK (the primary dev platform), and is the least code. Ship
   this first.
2. **Full multi-viewport.** Panels become real OS windows. Requires reworking the
   ImGui integration onto the standard ImGui Vulkan backend's multi-viewport
   support (per-window swapchains), giving up the offscreen-composite path — or
   running ImGui's viewport renderer beside it. Heavier and MoltenVK-fiddly. Defer
   until single-window docking is proven limiting.

`Window` is single and GLFW-based today; route 2 also means lifting that
assumption. Keep the decision explicit and start at route 1.

## The editor framework — `libveng_editor`

A new library: the panel / inspector / registry framework plus the built-in
editors. It links `libveng` + imgui/imnodes. It is **not** linked by the engine
runtime or by `libgame` — only by the editor host and by `libgame_editor`. This is
the "editor support library" the brief calls for: the thing a game links (in its
**editor** module only) to define custom views and tools.

### Panels

```cpp
class EditorPanel
{
public:
    virtual ~EditorPanel() = default;
    [[nodiscard]] virtual string_view Title() const = 0;
    virtual void OnImGui() = 0;          // draws the panel's contents each frame
    // open/close state, dock id, and menu wiring owned by the host
};
```

The host owns a panel registry and a Window menu to toggle them. Built-in panels:
asset browser, inspector, console/log, and the per-asset editors below.

### Asset-type → editor registry

A registry from `AssetType` to an asset-editor factory. Double-click a texture →
the texture editor opens; a `.vmat` → the material node editor. The built-ins
(texture / material / scene) register here, and **games register editors for their
own asset types through the same registry** from `libgame_editor`
([game-module.md](game-module.md)) — the extension point that keeps custom views
out of the shipped game.

```cpp
class EditorRegistry            // the EditorRegistry* the module host hands modules
{
public:
    void RegisterAssetEditor(AssetType type, Unique<AssetEditorFactory> factory);
    void RegisterPanel(Unique<EditorPanel> panel);
    void RegisterFieldWidget(FieldType type, FieldWidgetFn fn);   // inspector widgets
};
```

### Reflection-driven inspectors

Selecting an entity or asset draws an inspector built from the type's registered
**field descriptors** (the `TypeRegistry` / `FieldDescriptor` layer designed in
[game-module.md](game-module.md)). A built-in widget per `FieldType` — `f32`→drag,
`vec4`→color, `AssetHandle<Texture>`→asset picker, `bool`→checkbox — and games
register custom widgets via `RegisterFieldWidget`. No reflection in C++ means the
inspector is only as rich as the descriptors the game supplies; auto-generating
those descriptors is the open burden the game-module doc weighs.

## The concrete editors

### 1. Texture viewer / settings — the first slice

The simplest end-to-end editor and the template for "asset settings editor." Shows
the decoded image (zoom / pan / per-channel toggles), edits the `.tex.json`
settings (`srgb`, sampler `min`/`mag`/`wrap`, room for mips/compression later),
and **live re-cooks + re-previews** on change. Round-trips the JSON source on save.
Build this first — it exercises cook-on-demand, the preview-RT pattern, and JSON
source round-tripping with the least surface.

### 2. Node-based material editor — the headline

imnodes is already in the tree. Nodes (shader inputs, texture samples, params, math)
wire into an output node; the graph is the **authoring form** whose cooked output is
the `.vmat`. A preview panel renders a sphere/quad with the live material into a
preview RT. This is the **loaded `.vmat` path** the asset system explicitly left to
the editor ([asset-system.md](asset-system.md) — "the loaded path from a node-based
material editor is the editor's job, still future").

The defining open question — **does the graph generate shader source, or only bind
parameters?**

- **v1 (recommended): parameter/texture binding to an author-provided Slang
  shader.** The graph wires textures and `vec4`/scalar params into a fixed shader's
  inputs; its cooked output is the explicitly-typed **field list** planset-5's
  material already validates against the shader's reflected `MaterialData`. This
  reuses the *constructed* material path wholesale — the graph just authors the
  fields. Smallest leap, and it lights up immediately on the shipped foundation.
- **later: shader-graph codegen.** Nodes generate `.slang` source, cooked +
  reflected like any shader. Far larger (a node→Slang compiler, validation, the
  full material-graph surface). Defer; design the node/field model so codegen can
  layer on without breaking v1 graphs.

### 3. Scene editor + preview — blocked on a missing prerequisite

The most dependency-heavy editor, and the one veng **cannot build yet**: there is
**no scene/entity model**. planset-5 explicitly descoped a scene/level asset type
("a scene/level asset type — meshes + materials + textures only"); area 4
(events/input) is thin and stubbed; there is no transform hierarchy, no component
system, no ECS. The scene editor needs, as a hard prerequisite, a **scene
representation**:

- an entity / transform hierarchy with components,
- component types described via the [reflection layer](game-module.md) (so the
  inspector and serialization work), and
- a **scene asset type** that cooks and loads like the others.

That is its own area / planset — call it out, do not fold it into the editor
framework. Once it exists, the scene editor is: a viewport panel rendering the scene
to a preview RT, a hierarchy panel, a reflection-driven inspector, manipulation
gizmos (ImGuizmo or hand-rolled), and a save path that cooks the scene asset.
Everything *above* the scene model (the panels, the inspector, the preview RT) is
ordinary editor-framework work; the model underneath is the gate.

## Potential problems (the part the brief asks to think hard about)

- **Multi-viewport vs. the offscreen-composite model.** The single biggest
  rendering-integration decision (above). Real OS windows for panels fight the
  one-offscreen-image → one-swapchain path the engine uses today. Start
  single-window; treat multi-viewport as a deliberate, separate rework.
- **No reflection in C++.** Auto-inspectors and serialization need field
  descriptors the game must hand-write (or codegen). This burden is unavoidable and
  shapes the whole inspector/extension API — design the descriptor layer
  ([game-module.md](game-module.md)) before the inspectors that consume it.
- **The scene editor is gated on a scene/entity model that does not exist.** Do not
  start the scene editor as if it were just another panel; sequence the scene-model
  area ahead of it.
- **Cook-on-demand reintroduces importer deps — but only in the editor.** stb /
  assimp / Slang come back *into the editor process* via `libveng_cook`. The
  boundary holds only because the editor is a separate target: importers must never
  leak into `libgame` or `libveng`. Cooking should also run **off the main thread**
  (the [task system](threading-task-system.md)) so a recook doesn't hitch the UI —
  another reason the editor wants the async path.
- **Live preview needs async/hot-reload to not stall.** Recooking + re-uploading
  synchronously (`LoadSync` / `UploadSync` / `WaitIdle`) on every keystroke would
  hitch the editor hard. The editor is *the* client that makes
  [area 2's async `Load` + hot-reload](threading-task-system.md) worth building;
  build the live-preview loop against it rather than the blocking path.
- **The DLL/ABI boundary.** Same-toolchain assumption, `VE_API` exports, ownership
  across the boundary — all carried in [game-module.md](game-module.md). Get it
  wrong and the editor crashes loading a game.
- **Game-code hot-reload is hard; v1 restarts the play session.** Asset hot-reload
  (live content preview) is separate and *does* work — don't conflate them.
- **N preview render targets per frame + resize churn.** The render graph must
  drive several independent offscreen passes (one per visible preview), and panel
  resizes recreate RTs constantly — route every recreation through the deferred-
  destruction queue (it already makes mid-frame drops safe) and debounce on resize.
- **Undo/redo and save round-tripping.** A real editor needs a command stack
  (undo/redo) — a sizeable subsystem in its own right — and must round-trip the
  JSON sources **without destroying hand-authored formatting/comments**. Both are
  easy to under-scope; name them early.
- **Editor state & layout versioning.** `imgui.ini` docking layouts drift as panels
  are added/renamed; ship a default layout and a reset, and version it.

## Touch points (what this area adds / modifies)

- **New libraries / binaries:** `libveng_editor` (framework + built-in editors);
  the editor-host executable; per-game `libgame_editor` modules
  ([game-module.md](game-module.md)).
- **New types:** `EditorPanel`, `EditorRegistry`, `AssetEditorFactory`, the
  inspector + `FieldWidget` layer, the material node-graph model, and — as a
  prerequisite area — the scene/entity model + scene asset type.
- **Editor links `libveng_cook`** (cook-on-demand); the engine and `libgame` never
  do — the planset-5 boundary is preserved.
- **ImGui integration:** the dockspace host; the multi-viewport decision (start
  single-window). The preview path reuses `ImGuiLayer::CreateTexture` /
  `GetOutputImage` unchanged.
- **Depends on:** [game-module.md](game-module.md) (load native types/editors),
  [threading-task-system.md](threading-task-system.md) (off-thread cook + async
  hot-reload preview), a **new scene/entity area** (the scene editor), and the
  shipped [asset/material foundation](asset-system.md) (the loaded `.vmat` path).

## Suggested planset breakdown

The editor is too large for one planset. A first cut, each its own planset:

```
A. game-module build model      (game-module.md) — shared lib + launcher +
                                   C-ABI app registration.  ── DONE (planset-9).
B. editor shell + framework      libveng_editor, dockspace (single-window),
                                   panel/inspector/editor registries,
                                   cook-on-demand, the TEXTURE editor as first slice,
                                   AND the type-reflection layer (TypeRegistry),
                                   pulled out of A to be designed against the inspector.
C. material node editor          imnodes graph → loaded .vmat (param-binding v1),
                                   live preview against the async/hot-reload path.
D. scene/entity model            transform hierarchy, components, scene asset type
                                   (its own area) — THEN the scene editor on top.
```

**Sub-area A is delivered by [planset-9](../planset-9/README.md)** (the shared lib +
launcher + C-ABI `Application` registration, in-tree). Its **type-reflection layer
now belongs to sub-area B** — held out of the prerequisite so it is designed against
the real inspector rather than speculatively (the resolved open-`TypeId` direction is
recorded in [game-module.md](game-module.md)). The [threading](threading-task-system.md)
async work is the other real prerequisite (done); B is the first visibly-an-editor
milestone; C (material editor) is the headline; D carries its own large prerequisite
and lands last. C and area 7 (the scene model under D) are otherwise unchanged.

## Open decisions

- **Multi-viewport** — single-window docking v1 (recommended) vs. real OS windows
  now. Start single-window; revisit if it proves limiting.
- **Material graph: codegen or param-binding** — bind params to an author-provided
  Slang shader v1 (recommended, reuses the constructed material) vs. node→Slang
  codegen (later, larger).
- **Where cook-on-demand lives** — the editor links `libveng_cook` directly
  (recommended — it *is* a tool) vs. shelling out to the `vengc` binary. Direct
  link gives in-process, incremental, off-thread cooking.
- **Scene model shape** — ECS vs. an object/component hierarchy. Decide in the
  scene-model area, not here; it gates the scene editor.
- **Editor host: `Application` subclass or bespoke** — reuse `Application` (it
  already owns Context + AssetManager + TaskSystem + ImGui) vs. a purpose-built
  host. Leaning `Application` subclass for the shared lifecycle.
- **Undo/redo model** — a global command stack vs. per-editor histories. Name it
  early; it pervades every editor.
- **Does hello-triangle migrate to the module model** — carried in
  [game-module.md](game-module.md) (it is also the smoke test).
