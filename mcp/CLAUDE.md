# libveng_mcp — the optional MCP server library

`mcp/` is `libveng_mcp` (`veng::mcp`), an **optional** engine-tier library a consuming
app — a game *or* the editor — links to expose its live systems to AI agents over the
**Model Context Protocol**. The consumer constructs one `McpServer`, hands it the systems
it wants reachable (the current world, the `TypeRegistry`, the `AssetManager`, its
viewports) through an `McpHost`, and pumps it once per frame; the server runs a loopback
MCP endpoint on a background thread and marshals every engine-touching request onto the
render thread. Project-wide conventions live in the [root CLAUDE.md](../CLAUDE.md); the
engine surface these tools consume (reflection, the `Scene`/ECS layer, the `AssetManager`,
viewports) is in [engine/CLAUDE.md](../engine/CLAUDE.md) and the editor tools' host in
[editor/CLAUDE.md](../editor/CLAUDE.md).

The library is **not linked by `libveng`** and adds **no dependency to any existing
consumer**: it is a distinct `veng::mcp` target a consumer opts into, exactly as
`veng::graph` is a distinct library the editor and cooker opt into. It stays
**editor-free and importer-free** — the engine tools live in `libveng_mcp`; the editor
tools register into the same server from the editor side (`editor/src/EditorMcp.{h,cpp}`).
The public surface is **JSON-library-free**, so `veng::mcp` holds the same include-hygiene
guarantee `libveng` does.

## The shape — one server, a host seam, a pumped request queue

```
                        network thread (cpp-httplib)              render thread (the app)
  MCP client  ─HTTP─►  parse JSON-RPC ─► tool registry lookup        each frame:
                        │  protocol methods (initialize/tools/list)     server->Pump()
                        │    answered inline (no engine access)           drains the queue,
                        └► engine tool: push {args, slot} ───────────►    runs each handler on
                           block on the slot's condvar ◄─────────────     the render thread,
                        write HTTP response                                 fills + signals the slot
```

- **`McpServer`** (`Veng/Mcp/McpServer.h`) owns a loopback **Streamable-HTTP transport**
  (cpp-httplib), a **JSON-RPC 2.0** dispatch implementing the MCP `initialize` /
  `tools/list` / `tools/call` methods, a **tool registry** (name → `McpTool`), and its
  **own render-thread request queue** drained by `Pump()`. It is `Unique`, single-owner;
  `Create(const McpServerInfo&, const McpHost&)` is the factory. Dropping the `Unique`
  stops the listener thread and closes the socket (RAII — that is the whole of cleanup).
- **The server reports protocol version `2025-06-18`** in the `initialize` handshake and
  advertises the `tools` capability only. This library implements the JSON-RPC 2.0 and MCP
  spec directly; the request/response wire format is not re-documented here — the code in
  `McpServer.cpp` pins the exact protocol methods and shapes.
- **`McpServerInfo`** is the designated-init construction descriptor: `ServerName`, `Port`
  (0 picks an ephemeral port readable via `GetPort()`), `BindLoopbackOnly`, and
  `AllowMutations`.

## The network-thread ↔ render-thread request queue — the inverse of `TaskSystem`

Every engine-touching tool runs on the **render thread** at the pump point. The network
thread never touches a `Scene`, a `Viewport`, or the `AssetManager` directly; it enqueues
and blocks.

This is the **inverse** of `TaskSystem` ([engine/CLAUDE.md](../engine/CLAUDE.md)):
`TaskSystem` lands *off-thread* work *on* the render thread through a pumped queue; the MCP
server takes a request that *arrives* on the network thread, pushes it onto a render-thread
request queue, and the network thread blocks on a per-request result slot until the
render-thread `Pump()` services it. It reuses `TaskSystem`'s **mutex + condition_variable +
`Done` flag + result slot** handshake rather than `std::promise`/`std::future`, whose misuse
paths throw and so are illegal under `-fno-exceptions` (the shutdown drain is such a path).
The server owns its **own** pump — `TaskSystem::EnqueueMainThread` is private to the engine,
so `veng::mcp` cannot reach into it; the one `Pump()` call the consumer makes is the whole
of the wiring.

- **The pump point is scene-safe.** The consumer calls `Pump()` once per frame at a
  render-thread-safe point — the top of its per-frame update, before any `View`/`Each`
  iteration and before render — so both reads and mutations run outside any scene iteration
  and no engine concurrency rule is bent. A mutation tool never edits the scene
  mid-iteration (the `Scene` contract).
- **The listener thread starts on the first `Pump()`, not at `Create`.** The socket binds
  in `Create` (so `GetPort()` resolves immediately), but the network thread starts on the
  first pump. Tools registered between `Create` and the first pump therefore land before the
  network thread ever reads the tool registry, which is **immutable once serving** — read
  off-thread by `tools/list` without a lock. `RegisterTool` after the server has started is a
  fatal assert.
- **A request times out.** The network thread waits a bounded window (`RequestTimeout`, 5s)
  for the render thread to pump; on expiry it returns a host-busy error rather than blocking
  forever. A **synchronous main-thread modal** stalls the pump for its whole window: a native
  file dialog (or a debugger breakpoint) holds the render thread, so `Pump()` does not run and
  in-flight requests time out until the modal returns. A tool handler must not block on another
  MCP request — there is no re-entrancy.
- **Shutdown drains cleanly.** The destructor sets `ShuttingDown`, resolves every queued and
  in-flight request with a shutdown error so its network thread unblocks, stops the listener,
  and joins.

## The JSON-library-free public surface

A tool handler is `Result<string>(string_view argsJson)` (`Veng/Mcp/McpTool.h`): it receives
its `arguments` object as a JSON string and returns a JSON string (the tool-result payload) or
a located error, which the server surfaces as an MCP `isError` tool result — not a JSON-RPC
protocol error. The library parses/serializes internally with **nlohmann/json**
(`JSON_NOEXCEPTION`, PRIVATE); cpp-httplib and the JSON type never appear in a public header.
This keeps `veng::mcp` inside the include-hygiene guarantee — the `mcp_include_hygiene` test
compiles every `Veng/Mcp/` header while linking only the PUBLIC deps, so a leaked backend or
JSON include fails to build. Internally, `ReflectToJson.h` (which *does* name `nlohmann::json`)
is an implementation header, never part of the public surface.

An `McpTool` also carries an `InputSchemaJson` (surfaced verbatim as the tool's `inputSchema`
in `tools/list`) and a `ReturnsContentBlocks` flag: a plain tool's returned JSON is wrapped in
a single text content block; a content-block tool (e.g. `render.screenshot`, returning an image
block) returns the `content` array itself, spliced in verbatim.

## The transport and the vendored-httplib exception boundary

The transport is **loopback Streamable HTTP** over cpp-httplib, bound to `127.0.0.1` by
default. A GUI process owns its stdio, so the stdio MCP transport is unavailable; Streamable
HTTP is the standard transport an MCP client connects to a running local server over. The
server answers each POST with a JSON body — no SSE stream is opened, so server→client
notifications are not used, which is spec-compliant and keeps the transport minimal.

cpp-httplib is header-only and vendored (`src/Vendor/httplib.h`). It carries inline
`try`/`throw` bodies, so every TU that includes it is compiled **`-fexceptions`**: the vendor
aggregation TU (`src/Vendor/HttpLib.cpp`) and `src/McpServer.cpp`, which owns the
`httplib::Server`. The rest of `veng_mcp` stays `-fno-exceptions`. Per-TU exception settings
are a supported mix — the runtime unwinder is always present; `-fno-exceptions` only forbids
`throw`/`catch` in that TU. **Safety rests on containment:** cpp-httplib catches any throw at
its own dispatch boundary (`set_exception_handler` / its routing try/catch), and veng's own
handler code never throws, so **no exception unwinds out of the vendor TU** into a
`-fno-exceptions` frame. (This is the opposite lever from tinyexr, which is built the other
way — `TINYEXR_USE_EXCEPTIONS=0`.)

## The security posture

The server is a live local read (and optionally write) surface of the running app; the
defenses are three, together:

- **Loopback-only bind + `Origin`-header rejection.** `BindLoopbackOnly` (default true) binds
  `127.0.0.1`. A loopback bind alone does not stop a browser tab on the same machine from
  POSTing to the port, so a request carrying a **non-empty `Origin` header is rejected** (a
  real MCP client sends none) — the standard same-host browser defense.
- **`AllowMutations` off by default.** With it off (the default), a server registers only the
  read-only inspection and screenshot tools; `tools/list` honestly reflects the server's write
  capability. Write access is a deliberate flip — combined with the loopback bind, a default
  server is a safe local read surface.
- **A shipped build must never default the env-gate on.** The consumer gates server
  construction behind an explicit opt-in (`HT_MCP` in hello-triangle, `--mcp` in the editor). An
  on-by-default server is a live local read/screenshot surface of the game; the recipe env-gates
  it so the default ship path opens no socket and no thread.

Two structural facts back this up. **No MCP tool argument is ever a filesystem path** — every
engine reference crosses the wire as an opaque `AssetId`, matching `AssetManager`'s own external
contract: `Mount` takes a raw path and is never exposed to a tool, so an agent addresses assets
by id exactly as cooked data does, never by path. And **every engine-touching call runs at the
render-thread pump point**, never on the network thread — so a request cannot race scene state.

## Reflection is the (de)serializer

A component reads out and writes back through the existing `FieldDescriptor` walk — one JSON
encoding for every registered type, agent and editor and cook alike, with the same
schema-drift tolerance (an unknown field is skipped).

- **`FieldsToJson`** (`src/ReflectToJson.{h,cpp}`) is the read side: it walks a type's
  `FieldDescriptor`s and emits each field by its closed `FieldClass`, keyed by the
  serialization `Name` (never the display label). It is the read-side analogue of the editor
  inspector's `DrawFieldWidget` and the cooker's JSON → field parse — the canonical MCP
  component encoding every dumping tool reuses. Per class: Scalar → number/bool;
  Vector/Quaternion → array; Matrix → nested array; String → string; Enum → the enumerator
  name plus the raw integer; `AssetHandle` → the referenced `AssetId` as a decimal string;
  `Reference` → the entity's `{ index, generation }`; Struct → a recursed object; Variant →
  `{ type, value }`; Array → a JSON array.
- **`JsonToFields`** is the inverse and the JSON analogue of the binary `ReadFields`: it walks
  the descriptors and, for each key present in the source, parses the value by the field's
  class into storage. The update is **partial and tolerant** — an omitted field keeps its
  value, an unknown key is skipped — so a mutation touches only the fields it names. A value
  whose JSON kind does not match the field's class is a **located error, not a skip**: a
  malformed request is reported, never silently ignored. Every agent-supplied type name (a
  Variant's active-type `QualifiedName`, an enum enumerator) goes through a fallible lookup and
  yields a located error on a miss, never an asserting registry access; the Array arm clamps
  the incoming element count to a sanity cap.

## The tool families

Tool names follow a **`noun.verb` / `noun.property`** convention across every family, so the
surface reads consistently. The built-in engine tools live in `libveng_mcp`; the `editor.*`
family registers from the editor side.

- **`world.*` / `entity.* / scene.*`** (`src/WorldTools.cpp`, read-only) —
  `world.list_entities` (paginated), `entity.get` (a component dump via `FieldsToJson`),
  `world.query` (filter by component set, paginated), `scene.stats`. A null `CurrentWorld()`
  returns an empty result, never a null deref.
- **`render.*`** (`src/RenderTools.cpp`) — `render.screenshot` (viewport `Download` → PNG →
  base64 image content block, the smoke capture's `Download` path plus a PNG encode),
  `render.list_viewports` (over `McpHost::ViewportNames`), `render.stats` (cull counts +
  `GetLastGpuFrameTimeMs`). The PNG encode uses stb_image_write, vendored PRIVATE into
  `src/Vendor/StbImageWrite.cpp` — never a public header. A null/unknown viewport reports "no
  viewport".
- **`world.load_prefab` and the `entity.*` mutation verbs**
  (`src/MutationTools.cpp`, registered only when `AllowMutations` is set) —
  `entity.add_component`, `entity.remove_component`, `entity.set_field`, `entity.spawn`,
  `entity.destroy`, `world.load_prefab`. Each builds a resolved, validated `McpMutation` and
  applies it at the mutation-safe pump point.
- **`editor.*`** (`editor/src/EditorMcp.cpp`, registered by the `veng-editor` exe, not the
  library) — split by write posture exactly as the built-in tools are, so a read-only editor
  server honestly lists no write verbs. `RegisterEditorReadTools` (always) registers the
  inspection verbs `editor.list_panels`, `editor.inspect`, `editor.list_assets` (paginated),
  `editor.screenshot_panel`, and `editor.cook_status` (a poll that reads status only).
  `RegisterEditorWriteTools`, registered by the exe **only when `AllowMutations` is set** (the
  analogue of `RegisterMutationTools`), registers the mutating verbs `editor.set_field`,
  `editor.save`, `editor.undo`, `editor.redo`, `editor.open_asset`, `editor.set_panel_visible`,
  and `editor.request_cook` — the ones that change document, project, or editor-navigation state.
  So `--mcp` without `--mcp-write` opens a read-only editor surface, matching the same posture the
  engine tools hold.

### List pagination

Any tool returning an unbounded list (`world.list_entities`, `world.query`,
`editor.list_assets`) takes `{ limit?, cursor? }` and returns `{ items…, nextCursor? }` —
mirroring MCP's own `cursor`/`nextCursor` idiom. `limit` defaults to a cap so no single call
dumps a whole large world into an agent's context; `nextCursor` (opaque; internally the resume
offset) is present exactly while more remain, so the agent pages through the **full** set
rather than losing the tail. This is a context-volume convention for a single trusted local
client, not a DoS defense.

## `McpHost` — the provider seam

`McpHost` (`Veng/Mcp/McpHost.h`) mirrors `VengModuleHost`: the references and provider closures
the app fills so the built-in tools reach live state. The built-in tools capture the host by
reference, so the host must **outlive the server** (the app owns both). Every accessor runs on
the render thread during `Pump()`, so a closure may freely touch engine state. The fully
assembled struct:

```cpp
struct McpHost
{
    TypeRegistry&                                     Types;          // resolve a TypeId to fields
    AssetManager&                                     Assets;         // asset queries, id → name
    function<Scene*()>                                CurrentWorld;   // the scene to inspect, or null
    function<Renderer::Viewport*(string_view name)>   Viewport;      // resolve a viewport by name
    function<vector<string>()>                        ViewportNames;  // the viewport names to expose
    function<bool(const McpMutation&)>                ApplyMutation;  // optional editor routing hook
};
```

- A **game** fills `CurrentWorld` with its managed world and `Viewport`/`ViewportNames` with
  its primary viewport (under a well-known name like `""` or `"primary"`). The **editor** fills
  them from the active document's scene and its panel viewports. A null `CurrentWorld()` (no
  world loaded, a closed document) or a null/unknown `Viewport` makes the respective tools
  return an empty/"no viewport" result, never a null deref.
- **`ApplyMutation`** is the optional editor routing hook, consulted before a mutation touches
  the scene. Null in a game host: a mutation tool applies its `McpMutation` **raw** to
  `CurrentWorld()`. Set by an editor host: the tool hands the `McpMutation` to the host, which
  pushes the corresponding editor command onto the `CommandStack` (so an agent's edit is
  undoable and marks the document dirty) and returns true; a return of false means the tool
  falls back to the raw path. The tools never branch on host kind — they consult the hook and
  fall back.

An **`McpMutation`** is a resolved, validated description of one scene edit (its `Kind`,
`Target`, `Component` `TypeId`, `Values`/`Components` JSON strings, `Asset`, `Name`). The
mutation tools build one per `tools/call`, having already resolved and validated the target
entity, component `TypeId`, and any asset id, so both the raw and the routed applier read only
the fields the verb needs.

## The editor consumes reflection, never mirrors into MCP

The editor surface is **consumed from** reflection, not **mirrored into** MCP. An asset editor
hands back the reflected object(s) it edits through one small seam — `EditorPanel`'s
`GetInspectables()` (returning `{ Name, Type, Data }` records) / `OnInspectableChanged()` — and
the editor tools walk them with the same `FieldsToJson`/`JsonToFields` the inspector walks
through `DrawFieldWidget`. Adding a field to `LevelRenderSettings` or a material's params
appears over MCP with **zero MCP change**; the per-panel code lives in the panel, beside the
data it already draws, not as a second API surface to keep in sync. The bounded non-field verbs
(save/undo/redo) ride `AssetEditorPanel`'s existing virtuals, and editor world-edits route
through the `CommandStack` via `ApplyMutation` so an agent's edit is undoable. There is **no
method/function reflection** — the property surface is data reflection (already present) and the
action surface is a handful of lifecycle verbs (already virtuals). The `EditorMcpHost`
(`editor/src/EditorMcp.h`) is the editor-side analogue of `McpHost`: closures resolving the
panel set, the focused document, the document scene, the asset source index, and the
open/list/cook/screenshot verbs — so the tools reach the editor without the MCP library knowing
about any panel.

## How a consumer opts in

The whole of the wiring is: link `veng::mcp`, fill an `McpHost` from the app's systems,
construct the server, and call `Pump()` once per frame.

```cpp
// CMake:  target_link_libraries(app PRIVATE veng::mcp)

Mcp::McpServerInfo info{ .ServerName = "mygame", .Port = port, .AllowMutations = allowWrite };
m_McpHost.emplace(Mcp::McpHost{
    .Types         = GetTypeRegistry(),
    .Assets        = GetAssetManager(),
    .CurrentWorld  = [this] { return GetWorld(); },
    .Viewport      = [this](string_view n) { return n.empty() ? GetPrimaryViewport() : nullptr; },
    .ViewportNames = [] { return vector<string>{ "primary" }; },
});
m_McpServer = Mcp::McpServer::Create(info, *m_McpHost);
// ... each frame, at a scene-safe point before any View/Each iteration:
m_McpServer->Pump();
```

`hello-triangle` ([examples/hello-triangle/main.cpp](../examples/hello-triangle/main.cpp)) is
the worked reference — its `StartMcpServerIfRequested` is that recipe, env-gated behind
`HT_MCP` so the default `HT_SMOKE`/golden path opens no socket. The consumption walkthrough is
[docs/guides/consuming-mcp.md](../docs/guides/consuming-mcp.md).

## Build & install

`veng_mcp` builds **unconditionally from source** when veng is built, but is linked **only**
when a consumer names it — it is not gated by a build option, mirroring `veng::graph`'s posture.
It links `veng::veng` PUBLIC (so a consumer resolves the house-vocabulary includes through the
link); nlohmann/json, cpp-httplib, stb_image_write, and `Threads::Threads` are PRIVATE. It joins
the `vengTargets` export set and installs its `Veng/Mcp/` headers beside `libveng`, so an
out-of-tree `find_package(veng)` consumer can `target_link_libraries(app veng::mcp)` and the
installed `veng-editor` links it. Because nlohmann/json and httplib stay PRIVATE, a consumer
needs no extra `find_dependency` in `veng-config`.

## Tests

- **`mcp_loopback`** — the headless loopback smoke: construct, pump, and drive the JSON-RPC
  handshake (`initialize` / `tools/list` / `tools/call ping`).
- **`mcp_world`** — the read-only world tools over a populated scene (`FieldsToJson`,
  pagination).
- **`mcp_screenshot`** — `render.screenshot` (`gpu`-labelled: the viewport `Download` → PNG
  path).
- **`mcp_mutation`** — the mutation tools behind `AllowMutations`, including the routed
  `ApplyMutation` hook.
- **`mcp_include_hygiene`** — compiles every `Veng/Mcp/` public header linking only the PUBLIC
  deps, guarding the JSON-library-free surface.
- **`mcp_conformance`** — the shipping path: drive the hello-triangle server behind `HT_MCP`,
  assert the engine tool set is present and `render.stats` executes against the primary
  viewport.
- **`editor_mcp_conformance`** — the editor shipping path: launch `veng-editor --mcp` against
  the hello-triangle project and drive `render.stats` plus an `editor.set_field` Bloom toggle
  (a `Configure` recompile) against the startup level document, asserting each call succeeds
  and the editor survives. Needs a display as well as a device (the editor opens a window);
  either missing skips like the rest of the `gpu` band.
- The editor `editor_mcp` cases cover the `editor.*` tools over a host.
