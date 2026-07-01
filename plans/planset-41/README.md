# planset-41 — the optional MCP server library (`veng::mcp`)

**Phase goal:** ship an **optional** engine-tier library a consuming app (game *or* editor) links
to expose its live systems to AI agents over the **Model Context Protocol**. The consumer
constructs one `McpServer`, hands it the systems it wants reachable (the current world, the
`TypeRegistry`, the `AssetManager`, its viewports), and pumps it once per frame; the server runs a
loopback MCP endpoint on a background thread and marshals every engine-touching request onto the
render thread. Agents get **tools** for the world (list/inspect/query entities and components),
rendering (screenshot a viewport, read cull/timing stats), controlled mutation (spawn/destroy,
set a reflected field, load a prefab), and — when the host is the editor — panel/window management,
asset inspection, and cook-on-demand.

The library is **not linked by `libveng`** and adds **no dependency to any existing consumer**: it
is a distinct `veng::mcp` target a consumer opts into, exactly as `veng::graph` is a distinct
library the editor and cooker opt into. It stays **editor-free and importer-free** — the engine
tools live in `libveng_mcp`; the editor tools register into the same server from the editor side.

## Why now

The seams this needs are already in place, so the library is mostly *assembly of existing engine
surface*, not new engine mechanism:

- **Reflection is the serializer.** The `TypeRegistry` / `FieldDescriptor` / closed `FieldClass`
  layer that drives the editor inspector and the prefab cook already walks any registered component
  generically ([engine/CLAUDE.md](../../engine/CLAUDE.md), `Veng/Reflection/`). A component dumps to
  JSON by walking the same descriptors — the read-side analogue of `DrawFieldWidget` and the cooker's
  field parse. `Scene::ForEachComponent(Entity, visitor)` is the type-agnostic enumeration; entity
  ids are stable `{ Index, Generation }`.
- **Screenshots already exist.** The smoke capture downloads a viewport's output to the host and
  writes a PPM (`Viewport::GetOutput()->GetImage()->Download()`,
  [examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp) `WriteSceneCapture`).
  An MCP `render.screenshot` tool is that path plus a PNG encode + base64.
- **Main-thread marshaling is a solved shape.** `TaskSystem` already lands off-thread work on the
  render thread through a pumped queue (`PumpMainThread`, `EnqueueMainThread`). The MCP server owns
  the *inverse* of that queue: a request arrives on the network thread, is pushed onto a
  main-thread request queue, and the network thread blocks on a per-request result slot (the same
  `mutex` + `condition_variable` handshake `TaskSystem` uses, not `std::promise`) until the
  render-thread `Pump()` services it. `EnqueueMainThread` is private to `TaskSystem`, so the MCP
  server owns its **own** pump rather than reaching into the engine's.
- **The install/SDK machinery is arriving in planset-40.** `veng::mcp` joins the same
  `vengTargets` export set so an out-of-tree `find_package(veng)` consumer can
  `target_link_libraries(app veng::mcp)`.

## The unifying design — one server, a host seam, a pumped request queue

```
                          network thread (cpp-httplib)                render thread (the app)
  MCP client  ──HTTP──►  parse JSON-RPC ─► ToolRegistry lookup           each frame:
                          │  protocol methods (initialize / tools/list)     server->Pump()
                          │    answered inline (no engine access)             drains the queue,
                          └► engine tool: push {args, slot} ───────────►     runs each handler on
                             block on slot's condvar ◄──────────────────     the render thread,
                          write HTTP response                                  fills + signals the slot
```

- **`McpServer`** (`veng::mcp`, `Veng/Mcp/`) owns: a loopback **Streamable-HTTP transport**
  (cpp-httplib), a **JSON-RPC 2.0** dispatch implementing the MCP `initialize` / `tools/list` /
  `tools/call` methods, a **`ToolRegistry`** (name → `{ description, JSON input schema, handler }`),
  and its **own main-thread request queue** drained by `Pump()`. It is **`Unique`, single-owner**;
  `Create(const McpServerInfo&)` is the factory.
- **Tool names follow a `noun.verb` / `noun.property` convention** across every plan's families —
  `world.*`, `entity.*`, `scene.*`, `render.*`, `editor.*` — so the surface reads consistently. Each
  plan that adds tools keeps to it.
- **List tools are paginated, not truncated.** Any tool returning an unbounded list
  (`world.list_entities`, `world.query`, `editor.list_assets`) takes `{ limit?, cursor? }` and returns
  `{ items…, nextCursor? }` — mirroring MCP's own `cursor`/`nextCursor` idiom. `limit` defaults to a
  sensible cap so no single call dumps a whole large world into an agent's context, and `nextCursor`
  (opaque; internally the resume offset/index) is present exactly while more remain, so the agent pages
  through the **full** set rather than losing the tail. This is a context-volume convention for a
  single trusted local client, not a DoS defense.
- **The public surface is JSON-library-free.** A tool handler is
  `Result<string>(string_view argsJson)` — it receives its arguments as a JSON string and returns a
  JSON string (or a located error); the library parses/serializes internally with nlohmann/json
  (`JSON_NOEXCEPTION`, PRIVATE), and cpp-httplib never appears in a public header. This keeps
  `veng::mcp` inside the same include-hygiene guarantee `libveng` holds (see the decisions below).
- **`McpHost` is the provider seam** (mirroring `VengModuleHost`): the references and provider
  closures the app fills so the built-in tools reach live state —
  `{ TypeRegistry& Types; AssetManager& Assets; function<Scene*()> CurrentWorld;
  function<Renderer::Viewport*(string_view)> Viewport; }`. A game fills `CurrentWorld` with its
  managed world and `Viewport` with the primary viewport; the editor fills them with the active
  document's scene and its panel viewports. A null `CurrentWorld()` (no world loaded, a closed
  document) makes the world tools return an empty result, never a null deref. Plans 02 and 03 extend
  the struct (`ViewportNames`, `ApplyMutation`); Plan 06 documents the fully-assembled shape.
- **Central pumping, local ownership.** The app owns the `Unique<McpServer>` and calls `Pump()` at a
  render-thread-safe point (the top of its per-frame update, before any `View`/`Each` iteration and
  before render) — so a mutation tool never edits the scene mid-iteration (the `Scene` contract).
  Dropping the `Unique` stops the thread and closes the socket. `libveng` does **not** own or pump
  the server (it cannot — json/httplib are not `libveng` deps); the one `Pump()` call is the whole
  of the wiring.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | The `veng::mcp` skeleton | New `mcp/` → `libveng_mcp` / `veng::mcp`; cpp-httplib (new pinned FetchContent dep, vendored TU) + nlohmann/json (`JSON_NOEXCEPTION`) PRIVATE. `McpServer` + `McpServerInfo` + `ToolRegistry` + the network thread + the MCP handshake (`initialize` / `tools/list` / `tools/call`) + the main-thread `Pump()`; a trivial `ping` tool proves the loop. Headless loopback test. | done |
| 01 | Reflection→JSON + world tools | A generic `ReflectToJson` `FieldClass` walk (the read side of the inspector) + the `McpHost` provider seam; read-only tools `world.list_entities`, `entity.get`, `world.query`, `scene.stats`. Depends on 00. | done |
| 02 | Screenshot + render tools | stb_image_write PRIVATE; `render.screenshot` (viewport `Download` → PNG → base64 image content), `render.list_viewports`, `render.stats` (cull counts + `GetLastGpuFrameTimeMs`). Depends on 01. | proposed |
| 03 | Mutation tools + capability gating | Write-side reflection (JSON → field bytes, the `ReadFields` analogue); `entity.add_component`/`remove_component`, `entity.set_field`, `entity.spawn`/`destroy`, `world.load_prefab`, all at the mutation-safe pump point; an `AllowMutations` gate (off by default) + loopback-only bind + an optional `ApplyMutation` routing hook (for editor undo). Depends on 01. | proposed |
| 04a | Editor reflection seam + generic tools | A generic `Inspectable`/`OnInspectableChanged` seam on the editor panel base (panels hand back the reflected objects they already edit); generic `editor.inspect`/`set_field`/`save`/`undo`/`redo` tools consuming it; editor mutations routed through the `CommandStack` (undoable). No per-panel MCP code, no method reflection. Depends on 02 + 03. | proposed |
| 04b | Editor host tools + `veng-editor` wiring | The exe links `veng::mcp` behind `--mcp[=port]`, fills `McpHost` from the focused document + panel viewports, and adds `editor.open_asset`, `editor.set_panel_visible`, `editor.list_assets` (over the `AssetSourceIndex`), `editor.request_cook`, `editor.screenshot_panel`. `libveng_mcp` stays editor-free. Depends on 04a. | proposed |
| 05 | SDK install + hello-triangle wiring + conformance | Install `libveng_mcp` (+ headers) into `vengTargets`/the export set (on planset-40's install block); `hello-triangle` constructs + pumps an `McpServer` behind `HT_MCP=<port>`; a headless conformance smoke (`initialize` + `tools/list`, assert the engine tools) wired into ctest. `examples/template` stays MCP-free (the minimal sample), documented. Depends on 00–04b **and on planset-40**. | proposed |
| 06 | Docs + roadmap pass | `mcp/CLAUDE.md` module guide; the root `CLAUDE.md` layout entry; a `docs/guides/` MCP-consumption entry; a `future/README.md` area note (incl. graph-editing-via-`NodeGraph`-serializer + reflected-commands as deferred); the full verification band. The closer. Depends on 00–05. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **00** is foundational — the library, the transport, the JSON-RPC loop, the tool registry, and the
  pump. Everything registers tools into it.
- **01** depends on 00 (it adds the `McpHost` seam and the first real tools; the `ReflectToJson`
  walk is the shared serializer 02/03/04 all reuse).
- **02** and **03** both depend on 01 and are otherwise **independent** — 02 adds render/screenshot
  tools, 03 adds mutation tools; they touch different tool files but share the `McpHost` seam and the
  `ToolRegistry`. They can be built in parallel off 01's integration commit, merging in number order
  (02 then 03) since both append to the same built-in-tool registration list.
- **04a** depends on 02 + 03 (it needs the screenshot path and the mutation gate + the `ApplyMutation`
  routing hook) and is the first plan touching the editor — it adds the panel reflection seam + the
  generic editor tools. **04b** depends on 04a — it wires the `veng-editor` exe and adds the editor
  host tools (open/list-assets/cook/screenshot). They share `editor/src/EditorMcp.{h,cpp}`, so merge
  04a then 04b. The editor work is split into two lettered sub-plans (rather than a flat 04/05) because
  it is two plan-sized chunks that share one file and must land as one coherent editor unit before the
  SDK plan; keeping them `04a`/`04b` leaves the SDK/docs plans as `05`/`06`.
- **05** depends on 00–04b **and on planset-40** — it edits the same install block
  ([`CMakeLists.txt`](../../CMakeLists.txt) `install(TARGETS … EXPORT vengTargets)` +
  [`cmake/veng-config.cmake.in`](../../cmake/veng-config.cmake.in)) planset-40 is reworking, so it
  must merge **after planset-40 lands** to avoid fighting its install/export edits. The rest of the
  planset (00–04) is **independent of planset-40** and can proceed while planset-40 finishes.
- **06** is the closer, depending on 00–05.

Dependent plans must build on the **prior plan's integration commit**, not `origin/main`. Per
[[project_megaexec_worktree_base]], `isolation: "worktree"` branches from `origin/main` and will
not see a locally-committed-but-unpushed base: dispatch each plan against a manual worktree cut from
the prior plan's integration commit. The chain is **00 → 01 → { 02, 03 } → 04a → 04b → 05 → 06**, with
02/03 the one parallelizable pair.

## Relationship to planset-40

planset-40 (the installable veng SDK: imported tools, tri-mode `veng-config`, the `veng_add_*`
renames) is **complete** — its install/export machinery (`install(EXPORT)` + the build-tree
`export(EXPORT)` + `veng-config`) is landed. The one seam of overlap with this planset is that set:
only **plan 05** installs `libveng_mcp` into `vengTargets`, joining it the same way
`assetpack`/`graph`/`editor` do (see the plan-05 dependency note above). Plans 00–04 never touch the
install block, so nothing here waits on it. Plan 05's dispatch should re-confirm planset-40's status
column rather than trust this note if time has passed.

## The decisions this planset settles

- **The MCP server is an optional, separately-linked library, not an engine feature.** `libveng`
  gains no dependency; a consumer opts in with `target_link_libraries(app veng::mcp)`. This mirrors
  `veng::graph`'s posture (a distinct lib the editor/cooker link) and keeps json/httplib/a network
  thread out of every app that does not ask for them. It is **not** gated by a build option — from a
  full build it is always *built* (like the cooker after planset-40 retires `VENG_BUILD_TOOLS`),
  just not *linked* unless a consumer names it.
- **The public surface is JSON-library-free; handlers speak JSON strings.** A tool is
  `Result<string>(string_view)`; nlohmann/json (`JSON_NOEXCEPTION`) and cpp-httplib are PRIVATE and
  never reach a public header, so `veng::mcp` holds the same include-hygiene guarantee `libveng`
  does. An `include_hygiene`-style compile check over the `Veng/Mcp/` headers enforces it.
- **Transport is loopback Streamable HTTP (cpp-httplib), bound to `127.0.0.1` by default, with
  `Origin`-header rejection.** A GUI process owns its stdio, so the stdio MCP transport is
  unavailable; Streamable HTTP is the standard transport an MCP client connects to a running local
  server over. A request/response tool server answers each POST with a JSON body (no SSE stream is
  opened — server→client notifications are not used), which is spec-compliant and keeps the transport
  minimal. A loopback bind alone does not stop a browser tab on the same machine from POSTing to the
  port, so a request carrying a non-empty `Origin` header is rejected (a real MCP client sends none) —
  the standard local-dev-server defense. cpp-httplib is header-only and vendored into a dedicated TU
  compiled `-fexceptions` so its `throw`s compile while the rest of `veng_mcp` stays
  `-fno-exceptions`; per-TU exception settings are a standard, supported mix (**not** the tinyexr
  precedent — tinyexr goes the other way, built `TINYEXR_USE_EXCEPTIONS=0`). Safety rests on
  containment: cpp-httplib catches exceptions at its own dispatch boundary (`set_exception_handler`),
  so none unwind out of the vendor TU into a `-fno-exceptions` frame.
- **Every engine-touching tool runs on the render thread at the pump point.** The network thread
  never touches a `Scene`, a `Viewport`, or the `AssetManager` directly; it enqueues and blocks. The
  app calls `Pump()` once per frame before any scene iteration, so both reads and mutations are
  render-thread-safe and outside any `View`/`Each` — no engine concurrency rule is bent.
- **Reflection is the (de)serializer for component state.** A component reads out and writes back
  through the existing `FieldDescriptor` walk — one JSON encoding for every registered type, agent
  and editor and cook alike, with the same schema-drift tolerance (unknown field ⇒ skipped).
- **The editor surface is *consumed from* reflection, never *mirrored into* MCP.** An asset editor
  hands back the reflected object(s) it edits through one small seam (`GetInspectables()`), and the
  MCP tools walk them with the same `ReflectToJson`/`JsonToFields` the inspector walks through
  `DrawFieldWidget` — so adding a field to `LevelRenderSettings` or a material's params appears over
  MCP with **zero MCP change**. The per-panel code lives *in the panel, beside the data it already
  draws*, not as a second API surface to keep in sync. The bounded non-field verbs (save/undo/redo)
  ride `AssetEditorPanel`'s existing virtuals; editor world-edits route through the `CommandStack`
  so an agent's edit is undoable. **No method/function reflection** — the property surface is data
  reflection (already have it) and the action surface is ~5 lifecycle verbs (already virtuals);
  a general reflected-command layer is parked as future rather than built to serve those.
- **Mutations are opt-in and gated.** `McpServerInfo::AllowMutations` defaults **off**; a read-only
  server exposes only the inspection/screenshot tools. Combined with the loopback-only bind, a
  default server is a safe local read surface, and write access is a deliberate flip.
- **Editor tools live in the editor, not the library.** `libveng_mcp` stays editor-free; the
  `veng-editor` exe registers panel/asset/cook tools into the server it constructs, exactly as the
  editor contributes panels through `EditorRegistry` without `libveng` knowing about them.

## What remains future

- **Server→client notifications / streaming (SSE).** This planset is request/response only (POST →
  JSON). An SSE stream for push events (an entity spawned, a cook finished, a frame rendered) rides
  the same transport later, behind the event/input seams (the events/input area,
  [plans/future/README.md](../future/README.md)).
- **Prompts and resources.** MCP `resources/*` (expose assets/levels as addressable resources) and
  `prompts/*` are natural follow-ons; this planset ships **tools** only, the core agent-actuation
  surface.
- **Authentication / non-loopback exposure.** The default is loopback + `Origin` rejection + no auth.
  A remote/auth-gated server (a shared dev instance, a hosted build) reachable off-host is a separate
  concern layered on the transport — distinct from the same-host browser defense this planset ships.
- **A richer world-editing vocabulary.** Reparenting and multi-entity transactions are additive
  tools on the same seam. (Component add/remove and single-edit undo-in-editor land *this* planset.)
- **Rich node-graph editing.** Adding/connecting nodes in the material editor is exposed later
  through the `NodeGraph` serialize + mutation vocabulary (`veng::graph`), **not** a hand-mirrored
  node API — the same "consume the existing serializer" principle, one seam deeper.
- **Reflected commands / a scripting surface, and non-C++ tool authorship.** Method/function
  reflection is deliberately *not* built here; if a broad need emerges (a data-driven command
  palette, gameplay-system actions, scripted tools), it earns its keep then and the MCP tools ride
  it for free.
- **Programmatic dock-layout choreography.** Moving/splitting/retabbing panels is `DockBuilder` +
  `.ini` state manipulation, deferred as fiddly and low-value; floating OS windows are a standing
  design non-goal (multi-viewport is disabled). The editor window verbs this planset ships are
  list/open/show-hide/focus only.
