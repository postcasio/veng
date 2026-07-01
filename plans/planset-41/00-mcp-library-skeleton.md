# Plan 00 — the `veng::mcp` skeleton: transport, JSON-RPC, tool registry, pump

**Goal:** stand up `libveng_mcp` (`veng::mcp`) — a new optional shared library — with the full MCP
request path working end to end against a trivial tool, and nothing engine-specific yet. After this
plan, an MCP client connects to a loopback endpoint, completes the `initialize` handshake, lists a
single `ping` tool, calls it, and gets a result — with the call marshaled onto the render thread and
back. No `Scene`, `Viewport`, or reflection access yet; those are Plans 01–03.

## The starting point

- The build declares each library in its own subdir and a thin top-level
  [`CMakeLists.txt`](../../CMakeLists.txt) does `add_subdirectory(<lib>)` (assetpack, engine, graph,
  cooker, editor). [`graph/CMakeLists.txt`](../../graph/CMakeLists.txt) is the exact template for a
  new `veng::X` shared lib: `add_library(veng_graph SHARED …)` + `add_library(veng::graph ALIAS …)`,
  PUBLIC include dir, `target_link_libraries(veng_graph PUBLIC veng::veng)`,
  `-fno-exceptions` PRIVATE, `VE_BUILD_SHARED`, `target_precompile_headers(<lib> PRIVATE <Veng/Veng.h>)`,
  MSVC auto-export, and `nlohmann_json::nlohmann_json` PRIVATE with `JSON_NOEXCEPTION`.
- nlohmann/json is already a pinned FetchContent dep (used by graph/cooker), fetched at top level.
- The vendored-third-party-TU pattern exists: [`engine/CMakeLists.txt`](../../engine/CMakeLists.txt)
  compiles `src/Vendor/ImGui.cpp` / `StbImage.cpp` / `TinyExr.cpp` as aggregation TUs, with
  `set_source_files_properties` overrides per TU (e.g. tinyexr's flags), and the vendor `.clang-tidy`
  carries `Checks: '-*'`.
- `TaskSystem` (`engine/include/Veng/Task/TaskSystem.h`) is the main-thread-marshaling reference:
  `PumpMainThread()` drains a queue on the render thread; `EnqueueMainThread(function<void()>)` is
  **private** (friend `Task` only), so the MCP server cannot reuse it and owns its own queue.
- `Veng::Result<T>` / `VoidResult` (`Veng/Core/Result.h`), the house vocabulary (`string`,
  `vector`, `function`, `u16`/`u32`), and `Log::` are all available through `<Veng/Veng.h>`.

## What lands

### 1. The library target

A new `mcp/` subdir with `mcp/CMakeLists.txt` modeled on `graph/CMakeLists.txt`:

- `add_library(veng_mcp SHARED …)` + `add_library(veng::mcp ALIAS veng_mcp)`.
- `target_link_libraries(veng_mcp PUBLIC veng::veng)` — the public headers name the house vocabulary
  and (Plan 01+) engine types (`Scene`, `Viewport`, `TypeRegistry`, `AssetManager`), resolved
  through this link.
- `target_link_libraries(veng_mcp PRIVATE nlohmann_json::nlohmann_json)` +
  `target_compile_definitions(veng_mcp PRIVATE JSON_NOEXCEPTION)`.
- cpp-httplib (`yhirose/cpp-httplib`, header-only, MIT) is **vendored as its single header** into
  `mcp/src/Vendor/httplib.h` at a pinned version, rather than fetched — matching how stb/imgui are
  vendored, so it adds no `find_dependency` to `veng-config` and never joins the export set.
- cpp-httplib compiled in a dedicated TU (`mcp/src/Vendor/HttpLib.cpp` — a single
  `#define CPPHTTPLIB_IMPLEMENTATION` / include aggregation) with a `set_source_files_properties(…
  COMPILE_OPTIONS -fexceptions)` override so its `throw`s compile; the rest of `veng_mcp` stays
  `-fno-exceptions`. Per-TU exception settings are a supported, standard mix (the runtime unwinder is
  always present; `-fno-exceptions` only forbids `throw`/`catch` in that TU and omits its cleanup
  landing pads). **This is new to veng** — the existing vendor TUs go the other way (tinyexr is built
  `TINYEXR_USE_EXCEPTIONS=0` to *disable* its exceptions), so it is not literally the tinyexr
  precedent. Safety rests on **containment**: an exception must never unwind out of the `-fexceptions`
  TU into a `-fno-exceptions` frame. cpp-httplib is built for this — its dispatch loop wraps handler
  invocation in `try/catch` and exposes `set_exception_handler`, so a throw from its parser or a
  handler is caught inside the vendored TU and turned into a 500; the veng handler lambda is
  `-fno-exceptions` and cannot throw. The plan **states this containment requirement explicitly** and
  a verification smoke (feed a malformed request, assert a clean 500, no `terminate`) confirms it on
  the vendored version. A `mcp/src/Vendor/.clang-tidy` with `Checks: '-*'` keeps the vendored TU out
  of the lint allowlist, mirroring `engine/src/Vendor/.clang-tidy`.
- `add_subdirectory(mcp)` from the top-level `CMakeLists.txt`, after `engine` (it links
  `veng::veng`), unconditionally when building from source. **No install wiring in this plan** —
  that is Plan 05, on planset-40's install block.

### 2. The public headers (`mcp/include/Veng/Mcp/`)

JSON-library-free and httplib-free — only house vocabulary + `Veng::Result`:

- **`McpServerInfo`** — the factory descriptor (designated-init `XInfo` idiom):
  `{ string ServerName = "veng"; u16 Port = 0; bool BindLoopbackOnly = true; bool AllowMutations = false; }`
  (`Port = 0` picks an ephemeral port the test can read back via `GetPort()`; `AllowMutations` and
  the tool set it gates arrive in Plan 03 — the field is introduced here so the descriptor is stable).
- **`McpTool`** — a registered tool: `{ string Name; string Description; string InputSchemaJson;
  function<Result<string>(string_view argsJson)> Handler; }`. `InputSchemaJson` is a JSON-schema
  string surfaced verbatim in `tools/list`; `Handler` receives the `arguments` object as a JSON
  string and returns a JSON string (the tool result payload) or a located error (surfaced as an MCP
  `isError` tool result, not a JSON-RPC protocol error).
- **`McpServer`** — `static Unique<McpServer> Create(const McpServerInfo&)`; private ctor, `Native`
  idiom hiding the httplib server + the JSON machinery + the thread + the request queue in
  `mcp/src/McpServer.cpp`. Public surface:
  - `void RegisterTool(McpTool tool)` — adds to the `ToolRegistry`; asserts on a duplicate name.
  - `void Pump()` — drains the render-thread request queue: for each pending request it runs
    `tool.Handler(args)` on the calling (render) thread, stores the result in the request's slot, and
    signals its condition variable to wake the blocked network thread. Called once per frame by the
    owner at a scene-safe point.
  - `[[nodiscard]] u16 GetPort() const` — the bound port (resolves `Port = 0` to the actual one).
  - dtor stops the listener thread and closes the socket (RAII; matches "dropping the `Unique` is the
    whole of cleanup").

### 3. The server internals (`mcp/src/`)

- A `ToolRegistry` (name → `McpTool`) — plain map, main-thread-only mutation (tools register at
  construction, before `Pump()` runs).
- The **network thread**: `Create` spawns a thread running the httplib listener bound to
  `127.0.0.1` (or all interfaces if `BindLoopbackOnly == false`) on `Port`. A single POST endpoint
  (the MCP Streamable-HTTP message endpoint) receives a JSON-RPC 2.0 message. Once the socket is
  bound, the server **logs `Log::Info` "MCP server listening on <ip>:<port>"** with the resolved
  bind address and port — a startup record and the readiness signal a launcher/test waits on (Plan
  05).
- **Origin rejection.** A request carrying a non-empty `Origin` header is rejected (403) before
  dispatch. A loopback bind does not stop a browser tab on the same machine from issuing a
  `fetch()` POST to `127.0.0.1:<port>`; a real MCP client sends no `Origin`, a browser always does,
  so this defeats the same-host browser-origin vector cheaply. (Non-loopback exposure and auth stay
  future — this is only the local browser defense.)
- **JSON-RPC dispatch** implementing the MCP methods:
  - `initialize` — answered **inline on the network thread** (no engine access): returns
    `protocolVersion` (pin the MCP spec version at implementation time), `serverInfo`
    (`ServerName` + a version), and `capabilities` (`tools: {}`).
  - `notifications/initialized` — accepted, no-op.
  - `tools/list` — answered inline from the `ToolRegistry`: `{ tools: [{ name, description,
    inputSchema }] }` (the registry is immutable once serving, so reading it off-thread is safe).
  - `tools/call` — the engine path: parse `{ name, arguments }`, look up the tool, **enqueue** the
    request (with its result slot) onto the render-thread request queue, block the network thread on
    the slot's condition variable (with the request timeout), and on completion wrap the handler's
    returned JSON string in an MCP tool result (`{ content: [{ type: "text", text: <json> }] }`,
    `isError` from a `Result` error). An unknown tool name is a `tools/call` error result. A
    malformed JSON-RPC envelope is a JSON-RPC error response.
- **The request queue** is the inverse of `TaskSystem`'s, and reuses `TaskSystem`'s exact handshake
  primitive rather than `std::promise`/`std::future`: each pending request carries a small
  result-slot struct (a `std::mutex` + `std::condition_variable` + a `Done` flag + a
  `Result<string>` value), mirroring `TaskSystem`'s `Detail::TaskState<T>`. `std::promise`/`future`
  are **deliberately avoided** — they are used nowhere in veng because their misuse paths (a promise
  destroyed without a value → `broken_promise`) *throw*, which is fatal under `-fno-exceptions`; the
  shutdown-drain below is exactly such a path. The network thread pushes a request, then blocks on
  its condition variable (with the timeout below); `Pump()` pops, runs the handler, fills the slot,
  and signals. A request enqueued while the server is shutting down (the owner dropped the `Unique`
  between accept and `Pump`) is failed with an error so the network thread unblocks — the dtor drains
  and rejects any in-flight request before joining the thread.
- **A request timeout bounds the network-thread wait.** The block on the slot's condition variable
  uses a `wait_for` with a bounded timeout (a few seconds); on expiry the network thread returns a
  JSON-RPC error result ("host busy — the render thread did not pump in time") rather than blocking
  forever. This defends the one real stall: any synchronous main-thread modal on the render thread
  (a native `Window::OpenDialog`/`SaveDialog`, a debugger breakpoint) holds the event loop so `Pump()`
  never runs; without a timeout the network thread would wait indefinitely. The timeout also means a
  request whose slot is never serviced eventually frees its network thread. (`mcp/CLAUDE.md`, Plan 06,
  documents that a native dialog stalls the server for the timeout window.)

### 4. The `ping` tool

Registered by the test (not built into the library): `Description = "Echoes its message back."`,
`InputSchemaJson` describing `{ message: string }`, `Handler` returning `{ "echo": <message> }`. It
proves the whole loop — network parse → enqueue → render-thread `Pump` → handler → response —
without any engine dependency.

## The threading contract (state it in the header)

- `RegisterTool` is called **before** the server serves engine tools, on the render thread, at
  construction — not concurrently with `Pump()`.
- `Pump()` runs on the render thread; it is the **only** place a handler executes, so a handler may
  freely touch engine state (once Plans 01+ give it any). A handler must not block on another MCP
  request (no re-entrancy).
- The network thread touches only the immutable `ToolRegistry` (for `tools/list`) and the
  request queue; it never touches engine state.

## Files (sketch — the agent confirms against the tree)

- `mcp/CMakeLists.txt` (new) — the target, modeled on `graph/CMakeLists.txt`.
- `mcp/include/Veng/Mcp/McpServer.h`, `McpServerInfo.h`, `McpTool.h` (new) — the public surface.
- `mcp/src/McpServer.cpp` (new) — `Native`, the thread, JSON-RPC dispatch, the queue.
- `mcp/src/Vendor/HttpLib.cpp`, `mcp/src/Vendor/.clang-tidy` (new) — the vendored transport TU.
- `mcp/src/Vendor/httplib.h` (vendored) — the pinned single header.
- Top-level `CMakeLists.txt` — `add_subdirectory(mcp)`, plus the `mcp_loopback` /
  `mcp_include_hygiene` test targets. **There is no `tests/CMakeLists.txt`** — test executables are
  wired inline in the root `CMakeLists.txt` (`add_executable` + `add_test` + `set_tests_properties`,
  the `veng_test_gpu`/loader-test pattern). This plan adds the two `mcp_*` binaries there; the later
  plans (01–03) **add doctest cases to the Plan 00 `mcp` test binary** rather than standing up a new
  executable each, sharing its fixtures the way `veng_test_unit`/`veng_test_gpu` aggregate.
- `tests/mcp_*.cpp` (new) — the loopback + hygiene test sources.

## Verification

- Clean build with `veng_mcp` linked into the test; a dedicated **`mcp_include_hygiene`** sibling
  (not folded into the existing `include_hygiene`) compiles the `Veng/Mcp/` public headers with only
  veng's PUBLIC deps and asserts no json/httplib leak. It is a *different* boundary than
  `include_hygiene`'s Vulkan/GLFW exclusion, so it gets its own test rather than growing that one's
  link line — a failure names which boundary broke.
- A new **`mcp_loopback`** ctest (labelled to skip cleanly where a loopback socket is unavailable):
  constructs an `McpServer` on `Port = 0` with the `ping` tool, drives a background pump loop, and
  from the test thread performs a real HTTP `initialize` → `tools/list` (asserts `ping` present) →
  `tools/call ping` (asserts the echo). It uses the same vendored httplib client, so it needs no new
  dep. This is a pure-logic + loopback test — **no GPU** — so it runs in the default band.
- `ctest` green; the existing smoke/golden band is untouched (no engine or example change yet).

## Out of scope (later plans)

- Any engine-touching tool, the `McpHost` seam, reflection→JSON (Plan 01).
- Screenshots, render stats (Plan 02).
- Mutations, the `AllowMutations` gate's tool set (Plan 03).
- Editor tools (Plan 04); install/SDK wiring (Plan 05); the example wiring + docs (Plans 05–06).
