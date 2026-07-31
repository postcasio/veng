# libveng_mcp — the optional MCP server library

`mcp/` is `libveng_mcp` (`veng::mcp`), an **optional** engine-tier library a consuming
app — a game *or* the editor — links to expose its live systems to AI agents over the
**Model Context Protocol**. The consumer constructs one `McpServer`, hands it the systems
it wants reachable (the current world, the `TypeRegistry`, the `AssetManager`, its
viewports) through an `McpHost`, and pumps it once per frame; the server runs a loopback
MCP endpoint on a background thread and marshals every engine-touching request onto the
render thread. Project-wide conventions live in the [root CLAUDE.md](../CLAUDE.md); the
engine surface these tools consume is in the engine's per-system docs — reflection in
[engine/src/Reflection/CLAUDE.md](../engine/src/Reflection/CLAUDE.md), the `Scene`/ECS layer in
[engine/src/Scene/CLAUDE.md](../engine/src/Scene/CLAUDE.md), the `AssetManager` in
[engine/src/Asset/CLAUDE.md](../engine/src/Asset/CLAUDE.md), viewports in
[engine/src/Renderer/CLAUDE.md](../engine/src/Renderer/CLAUDE.md) — and the editor tools' host in
[editor/CLAUDE.md](../editor/CLAUDE.md).

The library is **not linked by `libveng`**: it is a distinct `veng::mcp` target a
consumer opts into, exactly as `veng::graph` is a distinct library the editor and cooker
opt into. It stays **editor-free and importer-free** — the engine tools live in
`libveng_mcp`; the editor tools register into the same server from the editor side
(`editor/src/EditorMcp.{h,cpp}`). No `Veng/Mcp/` header names a JSON type, but the public
surface is **not** JSON-library-free end to end: `veng::mcp` links `veng::veng` PUBLIC,
which itself carries nlohmann/json PUBLIC, so linking `veng::mcp` at all pulls nlohmann in
transitively — see [The public surface](#the-public-surface-httplib-free-not-json-library-free)
below.

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

## The client half — `McpClient` + `RunClientCli`

`veng::mcp` ships the request/response **client** beside the server, so a running server (a game
or the editor) is drivable from a shell without a separate MCP client library or a standalone
tool. Both halves live in the one library and share its vendored transport.

- **`McpClient`** (`Veng/Mcp/McpClient.h`) is the reusable transport half: `Create(const
  McpClientInfo&)` opens a loopback connection; `CallTool(name, argumentsJson)` and
  `ListTools()` each perform one `POST /`, parse the single JSON body, and return a `Result`.
  Because the server is stateless (no `Mcp-Session-Id`, no SSE, no gated `initialize`), a call
  is a bare `POST` + one `json::parse` with nothing to tear down. It mirrors `McpServer`'s
  discipline exactly: the public header names **no** httplib or nlohmann type, the vendored
  transport stays PRIVATE, and its TU compiles `-fexceptions`. `McpCallResult` carries the
  content blocks plus the `isError` flag, so a caller distinguishes a JSON-RPC protocol error
  from a tool error.
- **`RunClientCli(args, out, err, label) -> int`** (`Veng/Mcp/McpClientCli.h`) is the shared
  shell-facing driver: it parses the CLI argument vector, drives one `McpClient` call (or the
  tools listing), writes the tool payload to `out` / a human-readable error to `err`, and
  returns the process exit code. `label` is the invoking exe's name, so an error line is
  attributed to the exe the user ran. It lives here so both front ends share one arg grammar and
  one exit-code map: **0** ok · **1** usage (including an image-returning tool called without
  `--output`) · **2** cannot reach the host · **3** JSON-RPC protocol error · **4** tool result
  flagged `isError`. An image content block is binary: it is written to the `--output <file>`
  the client requires for it, **never** printed to stdout (no base64 in any form) — the file
  write is a client-side concern, honoring "no MCP tool argument is ever a filesystem path"
  (`--output` is a CLI argument, not a server tool argument). The grammar and the exit-code table
  are the normative copy — [docs/guides/consuming-mcp.md](../docs/guides/consuming-mcp.md)'s
  "Driving a running server from the shell" is the reader-facing view.

**There is no standalone `veng-mcp` tool.** The client is a capability of the exes that already
link `veng::mcp`: `veng-editor --connect` (behind `VENG_EDITOR_WITH_MCP`) and a game's
`<name>-launcher --connect` (behind `veng_add_game(... MCP)`). Both `main`s are ~10 lines that
strip `argv[0]` and call `RunClientCli` before any engine init — a pure client, no window, no
device, no module load.

## The network-thread ↔ render-thread request queue — the inverse of `TaskSystem`

Every engine-touching tool runs on the **render thread** at the pump point. The network
thread never touches a `Scene`, a `Viewport`, or the `AssetManager` directly; it enqueues
and blocks.

This is the **inverse** of `TaskSystem`:
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

## The public surface: httplib-free, not JSON-library-free

A tool handler is `Result<string>(string_view argsJson)` (`Veng/Mcp/McpTool.h`): it receives
its `arguments` object as a JSON string and returns a JSON string (the tool-result payload) or
a located error, which the server surfaces as an MCP `isError` tool result — not a JSON-RPC
protocol error. No `Veng/Mcp/` header names `nlohmann::json` or `httplib::` directly — the
library parses/serializes internally with **nlohmann/json** (`JSON_NOEXCEPTION`, PRIVATE) and
transports over cpp-httplib (PRIVATE); `ReflectToJson.h` (which *does* name `nlohmann::json`)
is an implementation header, never part of the public surface.

That per-header discipline is not the whole story: `veng::mcp` links `veng::veng` PUBLIC, and
`veng::veng` itself carries nlohmann/json PUBLIC (the engine's `Veng/Reflection/JsonSerialize.h`
names `json` types — see [engine/src/Reflection/CLAUDE.md](../engine/src/Reflection/CLAUDE.md)),
so nlohmann reaches every `veng::mcp` consumer transitively regardless of what an Mcp header
names. **`mcp_include_hygiene`'s contract is httplib-only** — it proves no `Veng/Mcp/` header
leaks the vendored transport, but it cannot distinguish "an Mcp header leaks nlohmann" from
"nlohmann always rides along via `veng::veng`," so it asserts nothing about the JSON half. A
consumer picks up nlohmann the moment it links `veng::mcp` (or, for that matter, `veng::veng`
alone) — it is not an *extra* dependency `veng::mcp` adds on top.

An `McpTool` also carries an `InputSchemaJson` (surfaced verbatim as the tool's `inputSchema`
in `tools/list`) and a `ReturnsContentBlocks` flag: a plain tool's returned JSON is wrapped in
a single text content block; a content-block tool (e.g. `render.screenshot`, returning an image
block) returns the `content` array itself, spliced in verbatim.

**The layer names the tool, the handler names the reason.** `MakeToolResult` in
`McpServer.cpp` is the single point at which a failed call's text is assembled, and it
prefixes the name `tools/call` dispatched on — so the reported error reads `<tool>: <reason>`
whether the failure came from the handler, an unresolved tool name, the request timeout, or
shutdown, and a handler never has to know what it is registered as. The client half adds only
its own `<label>` on top (`<label>: <tool>: <reason>`); a handler that prefixes its own name
as well makes the tool read twice, which is the shape the split exists to prevent.

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

- **`FieldsToJson`/`JsonToFields`** (`src/ReflectToJson.{h,cpp}`) are thin wrappers over the
  shared `Veng::JsonWriteFields`/`JsonReadFields` walker (`Veng/Reflection/JsonSerialize.h`):
  MCP supplies its own entity-addressing hooks (a `Reference` field reads/writes
  `{ index, generation }`); the walker itself encodes an `AssetHandle` as the canonical hex
  string, so a 64-bit `AssetId` round-trips losslessly through any JSON client with no
  MCP-local re-encoding. This is the canonical MCP component encoding every dumping and mutation
  tool reuses. Per class: Scalar → number/bool; Vector/Quaternion → array; Matrix → nested
  array; String → string; **Enum → the bare enumerator name string** (never an object or the
  raw integer); `AssetHandle` → the referenced `AssetId` as a canonical hex string; `Reference` →
  the entity's `{ index, generation }`; Struct → a recursed object; Variant →
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
- **`render.*`** (`src/RenderTools.cpp`) — `render.screenshot` (viewport `Download` → PNG → an
  image content block, the smoke capture's `Download` path plus a PNG encode; the `--connect`
  CLI requires `--output <file>` to write the PNG and never prints it to stdout),
  **`render.screenshot_window`** (the *presented* frame — the composite of the scene and the UI
  overlay drawn over it, which is what the app looks like on screen; a viewport carries scene
  colour alone and no UI, so this is the capture an agent drives an interface by. **A presented
  image is not readable** — it belongs to the presentation engine until it is acquired again, so
  transitioning it for a readback after the present is a write-after-present hazard the
  synchronization validation layer reports as an error. So registering the render tools **arms the
  context's presented-frame mirror** (`Context::ArmPresentedFrameCapture`) and the tool reads that:
  an engine-owned image each frame blits its finished composite into at the end of the frame, the
  last point at which the frame still owns the swap chain image. Arming at registration — ahead of
  the first `Pump`, which is what starts the listener thread — means the first call already finds a
  mirrored frame; the mirror then costs one full-window blit per frame for the context's lifetime,
  which is why it stays unarmed until a consumer asks. Requires the surface to grant
  transfer-source usage on its swap chain images, and is unavailable headless — where there is
  no swap chain and, because ImGui needs a window, no UI overlay to capture),
  `render.list_viewports` (over `McpHost::ViewportNames`), `render.stats` (cull counts +
  `GetLastGpuFrameTimeMs`). The PNG encode uses stb_image_write, vendored PRIVATE into
  `src/Vendor/StbImageWrite.cpp` — never a public header. A null/unknown viewport reports "no
  viewport".
- **`world.load_prefab` and the `entity.*` mutation verbs**
  (`src/MutationTools.cpp`, registered only when `AllowMutations` is set) —
  `entity.add_component`, `entity.remove_component`, `entity.remove_component_many`,
  `entity.set_field`, `entity.spawn`, `entity.destroy`, `entity.destroy_many`,
  `world.load_prefab`. Each builds a resolved, validated `McpMutation` and applies it at the
  mutation-safe pump point. The two **batch delete** verbs (`entity.destroy_many`,
  `entity.remove_component_many`) take a list capped at `MaxBatchSize` (20) and share the
  single verbs' per-item appliers (`DestroyOne` / `RemoveComponentOne`): they **validate the
  request shape up front** (non-empty, within the cap, each item well-formed) and reject a
  structural error as the whole call, then **apply each edit independently**, reporting a
  per-item `{ id, error }` for one that can't be performed — a stale entity (including one an
  earlier destroy in the same batch already took), an absent or unregistered component, the
  unremovable `Hierarchy` link, or a component another on the same entity declares it requires
  (`VE_REQUIRES`, checked through `Scene::FindRequirer` before the mutation is routed) — without
  aborting the rest. The cap mirrors the list tools'
  pagination limit: a context-volume convention for a single trusted local client, not a DoS
  defense.
- **`input.*`** (`src/InputTools.cpp`, registered only when `AllowMutations` is set — injecting
  input mutates app state, so it rides the same write gate as the mutation verbs) — `input.send`
  applies an **ordered batch** of synthetic input events so an agent can drive a running app:
  `key_down` / `key_up` (by engine `Key` enumerator name), `mouse_down` / `mouse_up` (`Left` /
  `Right` / `Middle`), `mouse_move` (window-space `{ x, y }`), `scroll` (`{ dx, dy }`), `text` (a
  UTF-8 `{ text }` run, up to 256 codepoints, expanded to one `KeyTypedEvent` per codepoint — the
  event a platform character callback raises, so a driven run reaches a focused text field down the
  same path a typing user does; a `key_down` raises no character and never fills a field). Each
  resolves to a `Veng::Event` and folds into the app's input through the `McpHost::InjectInput`
  closure at the mutation-safe pump point — a game fills it from
  `GetInputRouter()::PostInjectedEvent`, which queues every foldable kind (key/button down·up,
  move, scroll, text) for paced release at the frame's pre-tick input point, so an injected event
  is **indistinguishable from a real window event** and the action/mapping layer resolves it naturally on the next tick.
  **A driven run wants `--background-input`**: a window that loses OS focus normally surrenders any
  held gameplay focus, and every focus-gated context stops resolving with it — so an app being driven
  goes inert the moment the operator works in another window, while its always-on contexts keep
  responding and make the failure look like a mapping problem. The flag holds the focus token across
  the blur; it grabs nothing, so OS focus still moves away as usual. It **validates the
  batch shape up front** (a non-empty array within `MaxInputBatchSize` (64), each event
  well-formed with a known type/key/button) and rejects a structural error as the whole call
  before any event applies (the batch verbs' validate-then-apply discipline); a host that leaves
  `InjectInput` null makes the tool report injection unavailable rather than no-op silently.
  Injecting at the **raw device-event level** (not the resolved-action level) is deliberate: the
  same events a window would produce, so mappings resolve exactly as they do for a human.
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
    function<void(Event&)>                            InjectInput;   // optional synthetic-input sink
    function<Renderer::Context*()>                    RenderContext; // optional presented-frame capture source
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
- **`InjectInput`** is the optional synthetic-input sink the `input.*` tools feed. A game fills it
  with `[this](Event& e){ GetInputRouter().Dispatch(e); }` so a fabricated event routes exactly as
  a real window event (through the focus stack); an app that leaves it null makes `input.send`
  report injection unavailable. It runs on the render thread at the pump point, so it may freely
  touch the input service.

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
    .CurrentWorld  = [this]() -> Scene* {
        const World* w = GetWorldRunner().ResolveWorld(GetManagedWorldId());  // null before it loads
        return w != nullptr ? &w->GetScene() : nullptr;
    },
    .Viewport      = [this](string_view n) { return n.empty() ? GetManagedViewports().Get(0) : nullptr; },
    .ViewportNames = [] { return vector<string>{ "primary" }; },
});
m_McpServer = Mcp::McpServer::Create(info, *m_McpHost);
// ... each frame, at a scene-safe point before any View/Each iteration:
m_McpServer->Pump();
```

`hello-triangle` ([examples/hello-triangle/main.cpp](../examples/hello-triangle/main.cpp)) is
the worked reference — its `StartMcpServerIfRequested` is that recipe, env-gated behind
`HT_MCP` so the default `HT_SMOKE`/golden path opens no socket. Its convenience targets serve
on fixed loopback ports so a client has a known address: `hello_triangle-run` binds the game
server on **`127.0.0.1:5200`** (`HT_MCP=5200`, writes enabled), and the `veng-editor` target
binds the editor server on **`127.0.0.1:5201`** (`--mcp=5201 --mcp-write`). The consumption
walkthrough is [docs/guides/consuming-mcp.md](../docs/guides/consuming-mcp.md).

## Build & install

`veng_mcp` builds **unconditionally from source** when veng is built, but is linked **only**
when a consumer names it — it is not gated by a build option, mirroring `veng::graph`'s posture.
It links `veng::veng` PUBLIC (so a consumer resolves the house-vocabulary includes, and
nlohmann/json transitively, through the link); cpp-httplib, stb_image_write, and
`Threads::Threads` are PRIVATE, and `veng_mcp`'s own nlohmann/json link is a redundant PRIVATE
edge the PUBLIC one already covers. It joins the `vengTargets` export set and installs its
`Veng/Mcp/` headers beside `libveng`, so an out-of-tree `find_package(veng)` consumer can
`target_link_libraries(app veng::mcp)` and the installed `veng-editor` links it. Because
httplib stays PRIVATE and `veng-config` already carries `find_dependency(nlohmann_json)` for
`veng::veng`, a `veng::mcp` consumer needs no *extra* `find_dependency` of its own.

## Tests

- **`mcp_loopback`** — the headless loopback smoke: construct, pump, and drive the JSON-RPC
  handshake (`initialize` / `tools/list` / `tools/call ping`).
- **`mcp_world`** — the read-only world tools over a populated scene (`FieldsToJson`,
  pagination).
- **`mcp_screenshot`** — `render.screenshot` (`gpu`-labelled: the viewport `Download` → PNG
  path).
- **`mcp_mutation`** — the mutation tools behind `AllowMutations`, including the routed
  `ApplyMutation` hook and the batch delete verbs' per-item / over-limit result model.
- **`mcp_input`** — `input.send` behind `AllowMutations` over a headless `Input`: key/button/move/
  scroll events land in the snapshot through `InjectInput`, a `text` run reaches a live
  `Gui::Document`'s focused `TextInput` and edits its value, the batch shape-validation errors are
  whole-call, a rejected batch applies nothing, a read-only server omits the tool, and a null
  `InjectInput` host reports it unavailable.
- **`mcp_client`** — the client transport smoke: stand a server up in-process, drive
  `McpClient::ListTools` + `CallTool ping` + both failure paths (protocol error, tool `isError`).
- **`mcp_cli`** — `RunClientCli` in-process against an in-process server: the arg grammar,
  `--list` / `--search`, `--json`, the `key=value` assembly, the exit-code map, and the
  label-prefixed error lines — driven directly (bypassing any `main`).
- **`mcp_include_hygiene`** — compiles every `Veng/Mcp/` public header (the client's
  `McpClient.h` / `McpClientInfo.h` / `McpClientCli.h` among them) linking only the PUBLIC deps,
  guarding the surface's httplib-free contract (nlohmann rides the transitive `veng::veng` edge,
  so the test asserts only the httplib half).
- **`mcp_conformance`** — the shipping path: drive the hello-triangle server behind `HT_MCP`,
  assert the engine tool set is present and `render.stats` executes against the primary
  viewport.
- **`mcp_cli_conformance`** — the shipping *client* path: launch `hello_triangle-launcher` behind
  `HT_MCP` + `HT_SMOKE`, then drive it a second time as a `--connect` client (`--list`, a
  `render.stats` + `world.list_entities` `tools/call`, and both nonzero error paths), asserting
  the client's own exit codes and round-tripped payloads through the shipped exe. `gpu`-labelled,
  skips 77 when the launcher skipped for want of an ICD.
- **`editor_mcp_conformance`** — the editor shipping path: launch `veng-editor --mcp` against
  the hello-triangle project and drive `render.stats` plus an `editor.set_field` Bloom toggle
  (a `Configure` recompile) against the startup level document, asserting each call succeeds
  and the editor survives. Needs a display as well as a device (the editor opens a window);
  either missing skips like the rest of the `gpu` band.
- **`editor_mcp_cli_conformance`** — the editor shipping *client* path: launch `veng-editor
  --mcp` against the hello-triangle project and drive it with `veng-editor --connect` (`--list`,
  `editor.list_panels`, both error paths), exercising the editor exe's gated `--connect` seam end
  to end. `gpu`-labelled, skips 77 with no device/display.
- **`render.screenshot_window` has no automated coverage, deliberately.** The presented frame exists
  only where there is a swap chain, so every headless test — the whole `gpu` band and the
  `validation_gate`, which runs display-free binaries only — is structurally unable to reach it. The
  one *windowed* server in the band is `editor_mcp_cli_conformance`'s editor, and driving the capture
  there is **not reliably assertable** for the reason below: at a real window's size the capture can
  miss its reply window, an outcome that flips run to run with machine load. A test written over it
  either flakes or passes vacuously, so it is verified by hand: run a windowed consumer under
  `VE_DEBUG` (`HT_MCP=<port> hello_triangle-launcher`), drive `--connect=<port>
  render.screenshot_window --output <file>`, and read the consumer's log — a regression prints
  `WRITE_AFTER_PRESENT hazard detected` at `[ERROR]`.
- **Either screenshot tool can exceed the 5 s reply window at a real window's size.** The PNG encode
  runs inside the handler, on the render thread, and a debug build encoding a HiDPI frame (2560×1440
  is an ordinary 1280×720 window at 2×) can outlast `RequestTimeout` — so the client reports host-busy
  even though the render thread is pumping normally and `render.stats` answers instantly. It affects
  `render.screenshot` and `render.screenshot_window` alike, since both encode a full frame. Moving the
  encode off the render thread (or lengthening the window for a content-block tool) is unbuilt.
- The editor `editor_mcp` cases cover the `editor.*` tools over a host.
