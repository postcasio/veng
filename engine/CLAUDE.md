# libveng — the runtime

The engine: the public API under `engine/include/Veng/` and the Vulkan backend hidden behind it.
This file covers the runtime's spine — `Application`, the game-module/launcher model, and the
project data model — and indexes the per-system docs below. The project-wide conventions every
system is written against (error policy, house vocabulary, naming, comments, resource ownership,
the Native idiom) live in the [root CLAUDE.md](../CLAUDE.md).

## The per-system docs

Each major system's architecture lives in a `CLAUDE.md` inside its source directory:

- **[src/Renderer/CLAUDE.md](src/Renderer/CLAUDE.md)** — `RenderGraph`, the `SceneRenderer`
  deferred über-pipeline (g-buffer, lighting, shadows, SSAO, bloom, TAA, IBL/sky, culling, point
  fields), `Viewport` + the gather/composite tail, the pipeline cache, and bindless set 0.
- **[src/Scene/CLAUDE.md](src/Scene/CLAUDE.md)** — the ECS world (`Scene`, `Entity`, queries,
  `Hierarchy`, the spatial version), and the gameplay layer: seats and cameras, the
  input → actions → `PlayerInput` → `Intent` control flow, the Sim/View tick split, the
  `SystemRegistry` catalog, game modes, and `Level`s.
- **[src/Asset/CLAUDE.md](src/Asset/CLAUDE.md)** — runtime asset loading (`AssetManager`,
  `AssetHandle`, async/sync `Load`, `MountMemory`), meshes/textures/skinning, prefabs, and the
  shader/material model (`Material` / `MaterialInstance`, `MaterialDomain`).
- **[src/Reflection/CLAUDE.md](src/Reflection/CLAUDE.md)** — the `TypeRegistry`, `TypeId`s, the
  `FieldClass` field model, and the shared binary + JSON serializers every consumer binds
  through.
- **[src/UI/CLAUDE.md](src/UI/CLAUDE.md)** — `Veng::UI`, the immediate-mode vocabulary fronting
  ImGui (debug panels and the editor), including the engine-tier reflection inspector.
- **[src/Gui/CLAUDE.md](src/Gui/CLAUDE.md)** — `Veng::Gui`, the retained, data-driven game UI
  (cooked `*.vui.xml`/`*.vuss` documents, Yoga layout, binding, per-seat input, the
  `GuiOverlay`/`GuiSurface`/`CaptureSurface` component family).
- **[src/Net/CLAUDE.md](src/Net/CLAUDE.md)** — `Veng/Net/`, the server-authoritative
  client/server layer (transport, replication, prediction/reconciliation, interest management).

The consumption exemplars are documented in [examples/CLAUDE.md](../examples/CLAUDE.md); the
offline cook in [cooker/CLAUDE.md](../cooker/CLAUDE.md); the archive format in
[assetpack/CLAUDE.md](../assetpack/CLAUDE.md); the editor framework in
[editor/CLAUDE.md](../editor/CLAUDE.md); the node-graph/material-codegen library in
[graph/CLAUDE.md](../graph/CLAUDE.md); the optional MCP server library in
[mcp/CLAUDE.md](../mcp/CLAUDE.md).

## Application

Subclass `Application`, override `OnInitialize` / `OnUpdate(delta)` / `OnRender` / `OnDispose`,
and `Run(args)`. ImGui is opt-in (on by default; `nullopt` to skip), and a `Headless` flag runs
windowless to `RequestExit()` instead of a window close — that's the CI/smoke path.
`Application` owns the `AssetManager` (`GetAssetManager()`), the render `Context`, and the
`TaskSystem` (`GetTaskSystem()`), and threads them explicitly into each other (per-worker
transfer pools in the `Context`, the manager's loaders on the task system). The `TaskSystem` — a
fixed worker pool draining a work queue and returning `Task<T>` handles — is pumped once per
frame: `Frame()` calls `TaskSystem::PumpMainThread()` at the top, before `BeginFrame()` advances
the frame, so off-thread continuations land on the main thread.

**`Application` drives the viewport list each frame.** It owns a non-owning, ordered
`vector<Renderer::Viewport*>` (registration order = render order); `RegisterViewport(Viewport&)`
appends a pointer and hands the viewport a back-reference, so dropping the owner's
`Unique<Viewport>` self-unregisters it and the caller keeps ownership — only the *driving* is
central. See [src/Renderer/CLAUDE.md](src/Renderer/CLAUDE.md) for the viewport model.

**The managed viewport is a list, and split-screen is a runtime reconfigure of it.**
`ApplicationInfo::ManagedViewport` (`ManagedViewportInfo`: render extent, color format,
`SceneRendererSettings`, a normalized **`Layout`** — an offset + extent in `[0,1]` window
fractions resolved to pixels on construction and every swapchain resize — and an optional bound
**`Viewer`** seat) makes `Application` construct, register, resize-track, and expose engine-owned
`Presented` viewports. `GetManagedViewport(n)` / `GetManagedViewportCount()` reach the set (index
0 is the primary, `GetPrimaryViewport()`), and **`ReconfigureManagedViewports(span)`** — applied
at a safe point (top of frame, outside iteration) — replaces the set. A viewport that names a
`Viewer` has its camera resolved and pushed by the per-frame world drive and its region
associated with the `InputRouter` (so a free pointer over it routes to that seat); an unbound
viewport takes the scene's primary camera and routes no pointer. The gather + composite tail
already assembles every registered `Presented` viewport, so split-screen is "reconfigure to N
quadrant `Layout`s," not a bespoke render path; a single default-`Layout` managed viewport is
byte-identical to a hand-registered full-window one. The editor leaves `ManagedViewport` unset,
so `GetPrimaryViewport()` is null and it registers its own viewports.

**`Application` optionally bootstraps and drives the whole game world.** Set
`ApplicationInfo::World` (`GameWorldInfo { path Project; }`) and `Application` runs the game: at
the end of `Initialize` (after `OnInitialize`) it reads the **cooked project** (`<name>.vengproj`)
beside the executable (`ReadCookedProject`), mounts each pack it names, loads its **startup
level**, spawns the world (`Level::LoadInto`, the `Scene` owning its `SceneSimulation`), seeds
the managed viewport's topology + per-frame view from the spawned scene — the level's
`LevelRenderSettings` post knobs (`ApplyLevelRenderSettings`) plus the scene's one author-opt-in
`Sky` component (resolved by the renderer itself each `Execute`) and a `TimeOfDay` (derives the
sun direction from its hour + orbit and writes the directional light) — fires the
`OnWorldLoaded(Scene&, ResidencyBatch&)` hook, then starts the simulation. Each `Frame` it ticks
the world (`Scene::TickSimulation`, unless `SetWorldPaused`), calls `OnUpdate`, then resolves the
primary camera and pushes the `ViewState` into the managed viewport (`PushSceneView`, in
`Veng/Scene/SceneViewport.h` — the gameplay→render bridge that keeps `Renderer::Viewport`
gameplay-agnostic, falling back to `DefaultCameraView` when the scene resolves none). A game
reaches the running world through `GetWorld()` / `GetWorldViewState()` / `GetWorldLevel()` and
customizes it in `OnWorldLoaded`; the **minimal game writes no lifecycle or per-frame code at
all**. `World` unset leaves the app to load and drive its own world (the editor, or a game
wanting full control) — the explicit `Mount` → `LoadSync<Level>` → `LoadInto` →
`StartSimulation` → per-frame `TickSimulation`/`PushSceneView` path the managed world automates.

The world drive is an accumulator: the Sim phase steps at a fixed `SimTickRate`
(`GameWorldInfo`, default 60 Hz) with a monotonic tick, the View phase runs once per frame, and
the render gather blends transforms between the last two ticks — see
[src/Net/CLAUDE.md](src/Net/CLAUDE.md) for the tick model and the `ApplicationInfo::Net` wiring
(`--server` / `--join` / `--netsim`, `PumpNet`).

**A second level can be opened over the running one as a `LevelOverlay`.** `Application`
auto-ticks exactly the primary `m_World`; a game that wants a live sub-scene — a menu rendered as
a real scene, a picture-in-picture view, a full-screen modal world — opens a `Level` through the
`LevelOverlay` RAII handle (`Veng/LevelOverlay.h`): `LoadInto` a fresh scene → run a caller
**populate hook** (`std::function<void(Scene&)>`, the one cross-scene seam, run after load and
before start) → create a `Presented` viewport (registered last, so it composites on top) → route
input to the overlay's **own authored seat** (pointer association, cursor-seat handoff, and a
focus-scope suspension of the layer beneath) → `StartSimulation`. The game ticks it from
`OnUpdate` through `LevelOverlay::Update` (`TickSimulation` + `PushSceneView`) — no hidden second
scheduler — and its own viewport drives its scene's `GuiOverlay` HUD automatically. **Input focus
and simulation pause are separate knobs:** taking the overlay's seat always suspends the primary
world's *input*, but the primary keeps simulating unless the opt-in `PausePrimarySim`
(`SetWorldPaused`) is set. Dropping the handle (or `Close`) tears the overlay down in lifetime
order, restoring every router / cursor-seat / world-pause / drive-list value to the state it
captured at open; overlays **stack** (a dialog over a modal) and the handles drop LIFO. Results
flow back through a game-owned channel (a component the opener drains, a callback, or the
opener's glue writing the host) — no overlay system reaches into the primary scene.

**The engine render phase** runs between `BeginFrame()` and `EndFrame()`, uniform for every app
and not overridable: render every registered viewport in registration order (each does its own
`Execute` + `PrepareForAccess(Sample)`), so every viewport output is in `Sample` layout before
`OnRender` builds the ImGui draw data that may sample it. `OnRender` builds the ImGui frame and
records extra draws — it does not run the composite. When ImGui is on, the frame then records the
overlay and runs the **managed tail**: the `GatherPass` assembles the registered `Presented`
viewports into one full-window assembly target and `SwapChainCompositePass` composites it behind
the ImGui overlay. The managed tail's gather + composite graphs re-`Compile()` on swapchain
resize, and the composite re-targets the swapchain (`SetSwapChainTarget`) on a format change.

## Game modules: a shared lib + a launcher

A game is a **`libgame` (shared)** — the runtime: `Application` logic, components, custom runtime
types — loaded by a thin **launcher** (the shipped exe). The launcher `dlopen`s the module and
calls one C-ABI entry, `VengModuleRegister(VengModuleHost*)`; the module registers its
`Application` factory into the host-owned `ApplicationRegistry`, **its own reflected
component/type descriptors into the host-owned `TypeRegistry`** (`host->Types`), and **its own
gameplay `SceneSystem`s into the host-owned `SystemRegistry`** (`host->Systems`). A module
registers only what is **its own**: the host pre-registers the engine builtins into both
registries before the module runs (`RegisterBuiltinTypes` / `RegisterBuiltinSystems`), so a level
names them without the game re-declaring them. `Application` borrows the `TypeRegistry` and the
`SystemRegistry` (`GetTypeRegistry()` / `GetSystemRegistry()`) and owns
`Context`/`AssetManager`/`TaskSystem`; the launcher reads the factory back, constructs the app,
and calls `Run()`.

- **Same toolchain, one STL, one flag set.** Only the *entry* is C ABI; the payload is rich C++
  (`string`, `vector`, `Ref<T>` flow across freely). veng is **not** a binary-plugin platform — a
  module is recompiled with the engine from one tree. A one-integer `VengModuleAbiVersion`
  handshake (checked by `ModuleLoader` before the entry runs) **rejects a stale module loudly at
  load**. The ABI is at **version 5** (`VENG_MODULE_ABI_VERSION`, `Veng/Module/Module.h` — the
  header is authoritative). The host struct is `{ ApplicationRegistry& App; TypeRegistry& Types;
  SystemRegistry& Systems; EditorRegistry* Editor; }`. The gameplay layer adds **no** ABI
  surface: game modes are systems + components, the system catalog rides a per-system trait the
  way a component's `TypeId` does, and a `Level` is an asset — registered through the existing
  registries or authored as data, never through a new host entry.
- **`veng_add_game(<name> SOURCES … [ASSET_PACK …] [MCP])`** is the build entry: it emits
  `lib<name>` + `<name>-launcher` from one declaration, compiling the launcher exe from
  **`launcher_main.cpp`** — an **installed SDK artifact** whose path is `VENG_LAUNCHER_MAIN`,
  mode-resolved to the source tree in-tree and to the installed/build-tree location under
  `find_package(veng)` — so a downstream game builds the real shipping launcher without a veng
  source tree. The core-data paths a build references are likewise mode-resolved
  (`VENG_CORE_SHADER_DIR` / `VENG_CORE_PACK_JSON`; consumer-facing lowercase
  `veng_CORE_SHADER_DIR` / `veng_CORE_PACK_JSON`).
- **The bare `MCP` option opts the launcher into the MCP `--connect` client**: it links
  `<name>-launcher` against `veng::mcp` and compiles `VENG_LAUNCHER_MCP` into it, activating
  `launcher_main.cpp`'s `--connect` short-circuit (a one-shot client of an already-running MCP
  server, driving one tool call and exiting **before** the game module is loaded — a pure client,
  no engine init). Without `MCP` the launcher is byte-for-byte unchanged. The game's own MCP
  *server* is started by the game **module** (which links `veng::mcp` itself), never by the
  launcher.
- **The relocatable set.** The module resolves **beside the launcher** via an
  `$ORIGIN`/`@loader_path` rpath, and the cooked project + packs resolve via
  `ExecutableDirectory()` (the public executable-relative path helper) with `veng_add_game`
  copying the project + packs beside the launcher — so launcher + lib + project + packs move as
  one directory.
- **`EditorRegistry*` is the editor-host seam.** The launcher always passes `Editor = nullptr`
  (the type is only forward-declared in `libveng`); the editor host (`EditorHost`, in
  `libveng_editor`) passes a non-null `&m_EditorRegistry`, activating a module's editor-side
  registrations. The editor is the **single project-agnostic `veng-editor` exe**, launched
  against a project; **`veng_add_editor(<name> GAME_MODULE <t> [EDITOR_MODULE <t>] PROJECT <p>)`**
  builds no exe — it registers a per-project `<name>-editor` **run target**. See
  [editor/CLAUDE.md](../editor/CLAUDE.md).
- **Game-code hot-reload is out** — restart the play session. (Distinct from *asset* hot-reload,
  the async path.)

## Project settings & build configurations

`Veng/Project/` is the engine's home for **per-platform build policy** — the reflected data model
the cooker and editor both read. `libveng` carries the structs and the enum⇄name tables; cooked
blobs stay binary (the runtime load path parses no JSON).

- **`ProjectSettings`** (`Veng/Project/ProjectSettings.h`) — one per project (the JSON file
  `project.veng`): a reflected `vector<BuildConfiguration> Configurations` (a genuine
  `FieldClass::Array` field), the `ActiveConfiguration` name the editor previews through and the
  cook defaults to, a `vector<path> Packs` (the pack manifests the project owns), and a
  `StartupLevel` `AssetId`. `Packs` and `StartupLevel` are persisted by hand through the
  `"packs"`/`"startupLevel"` keys, kept off the reflected field list; the cook writes the startup
  level + pack mount names into the cooked project file (`.vengproj`), not the pack header.
- **`BuildConfiguration`** (`Veng/Project/BuildConfiguration.h`) — a named ship target: a
  `RoleToFormat` codec table (a fixed record, one `CompressionFormat` field per role — the role
  set is closed), a zstd `CompressionLevel`, a `Target` label, and an `OutputSuffix` (the single
  source of truth for the per-config pack name).
- **`CompressionRole`** (Color / Normal / Mask / HDR / UI) is a texture's **intent**, the stable
  authoring surface; **`CompressionFormat`** is the closed set of codec outputs a role table may
  name (uncompressed unorm/sRGB, BC7, ASTC 4×4, the HDR float). Both are
  `VE_LEAF(FieldClass::Enum)` so the editor draws a combo, serialized **by name** (never ordinal)
  through shared `ToString`/`Parse` tables. `CompressionFormat` is deliberately *not*
  `Renderer::Format` (which carries depth/swapchain/index formats nonsensical as a texture
  codec); a free `ToRendererFormat()` switch lowers it to the engine format at cook time. Under
  the two current codecs every role maps full-channel (`Color`→sRGB, the rest→unorm); there are
  no channel-specialized mappings (`Normal`→BC5, `Mask`→BC4).

The cooker's `ParseBuildConfiguration`/`ParseProject` (`Cooker.cpp`) and the editor's
`ProjectSettingsPanel` hand-parse the authoring JSON into these structs — the one reflected model
in the tree not bound through the shared JSON walker. The cook resolution and CMake host-default
selection are in [cooker/CLAUDE.md](../cooker/CLAUDE.md); the editor surface + host-capability
preview gate in [editor/CLAUDE.md](../editor/CLAUDE.md).
