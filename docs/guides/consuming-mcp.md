# Exposing your app to an agent with the MCP server

`veng::mcp` (`libveng_mcp`) is an **optional** library a game or the editor links to
expose its live systems to an AI agent over the **Model Context Protocol**. You construct
one `McpServer`, hand it the systems you want reachable through an `McpHost`, and pump it
once per frame; the server runs a loopback MCP endpoint on a background thread and marshals
every engine-touching request onto the render thread, so an agent can list and inspect
entities, screenshot a viewport, read render stats, and — behind an explicit gate — mutate
the world.

The library is not linked by `libveng` and adds no dependency to any app that does not ask
for it. Link it explicitly and the whole of the wiring is: fill an `McpHost`, construct the
server, and call `Pump()` once per frame. The architecture behind this guide is
[mcp/CLAUDE.md](../../mcp/CLAUDE.md); the reference implementation is
[hello-triangle](../../examples/hello-triangle/main.cpp)'s `StartMcpServerIfRequested`.

## Linking the library

`find_package(veng)` brings `veng::mcp` in every consumption mode (in-tree, build tree,
install prefix — see [consuming-veng.md](consuming-veng.md)). Link it like any other veng
target:

```cmake
find_package(veng REQUIRED)

veng_add_game(mygame SOURCES src/main.cpp PROJECT mygame_project)
target_link_libraries(mygame PRIVATE veng::mcp)
```

Because the `Veng/Mcp/` public headers are JSON-library-free — a tool handler is
`Result<string>(string_view)` — you need no nlohmann/json or httplib link of your own. The
vendored transport and JSON library stay PRIVATE to `veng_mcp`.

## Constructing and pumping a server

A game fills an `McpHost` from its own systems, constructs the server, and pumps it once per
frame at a scene-safe point. **Construct the host after the engine has initialized** — from
`OnInitialize` or later, never before `Run()`: `McpHost::Assets` binds `GetAssetManager()` by
reference, and the engine's systems (asset manager, task system, render context) exist only
once `Run()` has initialized them; a host filled earlier captures a dead reference that
crashes the first time a tool reaches through it (`GetAssetManager` asserts on the misuse).
`hello-triangle` gates the whole thing behind an `HT_MCP` environment variable so its default
`HT_SMOKE`/golden path opens no socket:

```cpp
#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

void StartMcpServerIfRequested()
{
    const char* portEnv = std::getenv("HT_MCP");
    if (portEnv == nullptr)
    {
        return;  // env-gated: the default ship path opens no socket, no thread
    }

    Mcp::McpServerInfo info;
    info.ServerName = "mygame";
    info.Port = static_cast<u16>(std::atoi(portEnv));   // 0 picks an ephemeral port
    info.AllowMutations = std::getenv("HT_MCP_WRITE") != nullptr;  // writes are a second gate

    // The host resolves live state per call on the render thread during Pump(). It is captured
    // by reference, so it must outlive the server — declare it before the server (destroyed last).
    m_McpHost.emplace(Mcp::McpHost{
        .Types         = GetTypeRegistry(),
        .Assets        = GetAssetManager(),
        .CurrentWorld  = [this] { return GetWorld(); },
        .Viewport      = [this](string_view name) -> Renderer::Viewport*
        {
            return (name.empty() || name == "primary") ? GetPrimaryViewport() : nullptr;
        },
        .ViewportNames = [] { return vector<string>{ "primary" }; },
    });

    m_McpServer = Mcp::McpServer::Create(info, *m_McpHost);
}
```

The one per-frame call drives everything:

```cpp
void OnUpdate(float delta) override
{
    // The engine has ticked; drain the MCP request queue at the render-thread-safe point,
    // before any View/Each iteration and before render — so a mutation tool never edits the
    // scene mid-iteration.
    if (m_McpServer)
    {
        m_McpServer->Pump();
    }
    // ... gameplay update ...
}
```

Member order matters: declare `m_McpHost` before `m_McpServer` (so it is destroyed last),
and reset the server before the host in `OnDispose`. Dropping the `Unique<McpServer>` stops
the listener thread and closes the socket.

`McpServerInfo` fields:

| Field              | Default   | Meaning                                                          |
|--------------------|-----------|------------------------------------------------------------------|
| `ServerName`       | `"veng"`  | The name reported in the MCP `initialize` handshake.             |
| `Port`             | `0`       | The TCP port to bind. `0` picks an ephemeral port (`GetPort()`). |
| `BindLoopbackOnly` | `true`    | Bind `127.0.0.1` only.                                            |
| `AllowMutations`   | `false`   | Expose the mutation tools. Off by default — a read-only surface. |

## What the host provides

`McpHost` is the provider seam the built-in tools reach live state through:

- **`Types`** / **`Assets`** — the `TypeRegistry` (to resolve a component's fields) and the
  `AssetManager` (asset queries, id → name).
- **`CurrentWorld`** — returns the `Scene` an agent inspects, or null when no world is
  loaded. A game returns its managed world; a null return makes the world tools return an
  empty result, never a crash.
- **`Viewport`** / **`ViewportNames`** — resolve a viewport by name and name the viewports
  the render tools expose. A game names its primary viewport (under `""`/`"primary"`) and
  resolves it; leaving `ViewportNames` unset makes `render.list_viewports` report none.
- **`ApplyMutation`** — an optional editor routing hook (below); leave it null in a game and
  mutations apply raw to `CurrentWorld()`.

Every closure runs on the render thread during `Pump()`, so it may freely touch engine
state.

## The tools an agent sees

With `AllowMutations` off (the default), the server registers the read-only families:

- **`world.list_entities`**, **`entity.get`**, **`world.query`**, **`scene.stats`** — list,
  inspect (a full component dump via reflection), filter, and summarize the world. The list
  tools are paginated: `{ limit?, cursor? }` in, `{ items…, nextCursor? }` out.
- **`render.screenshot`** (a viewport captured to a base64 PNG image block),
  **`render.list_viewports`**, **`render.stats`** (cull counts + last GPU frame time).

Flip `AllowMutations` on and the write family appears: **`entity.add_component`**,
**`entity.remove_component`**, **`entity.set_field`**, **`entity.spawn`**,
**`entity.destroy`**, **`world.load_prefab`**. A component's fields read out and write back
through the same reflection walk the editor inspector and the cook use, so a new field on a
registered component appears over MCP with no MCP change.

## Connecting a client

The server speaks JSON-RPC 2.0 over a loopback Streamable-HTTP endpoint (POST to `/`). Point
an MCP client at `http://127.0.0.1:<port>/`. A minimal manual check:

```sh
mygame &                                   # started with HT_MCP=8765 (or your gate)
curl -s http://127.0.0.1:8765/ -d '{
  "jsonrpc":"2.0","id":1,"method":"initialize",
  "params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl"}}
}'
curl -s http://127.0.0.1:8765/ -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

The server reports protocol version `2025-06-18` and the `tools` capability.

## The editor host

The `veng-editor` exe links `veng::mcp` and runs the server behind `--mcp[=port]` (add
`--mcp-write` to enable mutations). It fills an `McpHost` from the focused document's scene
and its panel viewports, and registers the editor's own tool family — `editor.list_panels`,
`editor.inspect`, `editor.set_field`, `editor.save`/`undo`/`redo`, `editor.open_asset`,
`editor.set_panel_visible`, `editor.list_assets`, `editor.screenshot_panel`,
`editor.request_cook`, `editor.cook_status` — through the parallel `EditorMcpHost` seam
(`editor/src/EditorMcp.h`). The library itself stays editor-free; the editor contributes its
tools exactly as it contributes panels.

The editor host also sets `McpHost::ApplyMutation` to route an agent's world edit through the
focused document's `CommandStack`, so it is undoable and marks the document dirty exactly like
a human's edit.

### The `GetInspectables()` extension point

The editor's property tools are **fully generic over reflection** — there is no per-panel MCP
code. A panel exposes its reflected models to agents by overriding one `EditorPanel` seam:

```cpp
// EditorPanel virtuals (editor/include/VengEditor/EditorPanel.h)
struct Inspectable { Veng::string Name; Veng::TypeId Type; void* Data; };

/// The reflected models this panel exposes to inspection/editing. Default empty.
virtual Veng::vector<Inspectable> GetInspectables() { return {}; }

/// Called after an external write into an inspectable, so the panel runs its existing
/// apply path (recook, mark dirty, re-resolve). Default no-op.
virtual void OnInspectableChanged(Veng::string_view name) {}
```

Return the reflected objects the panel already edits (each `{ Name, Type, Data }`), and
`editor.inspect` / `editor.set_field` walk them with the same `FieldsToJson`/`JsonToFields`
the inspector uses. Adding a field to a reflected model surfaces over MCP with no MCP change;
after a write, the tools call `OnInspectableChanged` so the panel recooks and marks dirty as a
UI edit would. This is the one seam a downstream editor author touches to make a new panel's
model agent-reachable.

## The safety model

- **Loopback-only, with `Origin` rejection.** The server binds `127.0.0.1` by default, and a
  request carrying a non-empty `Origin` header is rejected (a real MCP client sends none) — the
  standard defense against a browser tab on the same machine POSTing to the port.
- **Mutations are off by default.** A default server is a read-only inspection/screenshot
  surface; write access is a deliberate flip of `AllowMutations`.
- **Never default the gate on in a shipped build.** Construct the server behind an explicit
  opt-in (an env var, a CLI flag) as the samples do. An on-by-default server is a live local
  read/screenshot surface of the running game.
- **No tool argument is a filesystem path.** Every engine reference crosses the wire as an
  opaque `AssetId`, matching the `AssetManager`'s own external contract — an agent addresses
  assets by id, never by path.

Authentication, non-loopback exposure, and server→client streaming are out of scope of this
transport; see [plans/future/README.md](../../plans/future/README.md).
