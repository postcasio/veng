# Plan 06 — docs + roadmap pass

**Goal:** document the MCP library end to end — a module `CLAUDE.md`, the root-`CLAUDE.md` layout
entry, a `docs/guides/` consumption guide, and a `future/README.md` area note for the deferred work —
and run the full verification band. The closer. Depends on Plans 00–05.

## What lands

### 1. `mcp/CLAUDE.md` — the module guide

A new per-module architecture doc (the fifth module guide, joining engine/editor/cooker/assetpack),
covering: the `McpServer` / `McpHost` / `ToolRegistry` model — including the **fully-assembled
`McpHost` struct** (all fields together: `Types`, `Assets`, `CurrentWorld`, `Viewport`,
`ViewportNames`, `ApplyMutation`), which the plans build up incrementally across 01–03 and which a
consumer must see whole; the network-thread ↔ render-thread request-queue-and-pump contract (and why
it is the inverse of `TaskSystem`, reusing its mutex/condvar/slot handshake rather than
`std::promise`), including the **request timeout** and the fact that a synchronous main-thread modal
(a native file dialog) stalls the server for that window; the JSON-library-free public surface + the
`Result<string>(string_view)` handler shape; the loopback Streamable-HTTP transport, the
vendored-httplib-TU `-fexceptions` override **and its containment boundary** (no exception crosses out
of the vendor TU); `ReflectToJson`/`JsonToFields` as the reflection (de)serializer; the built-in
engine tool families (`world.*`/`entity.*`/`scene.*`/`render.*`) and the editor-side tool families
(`editor.*`); the shared **list-pagination convention** (`{ limit?, cursor? }` → `{ items…,
nextCursor? }`, MCP's `cursor`/`nextCursor` idiom); and the **security posture** as one place: loopback-only bind + `Origin`-header
rejection (the same-host browser defense) + `AllowMutations` off by default, with the explicit facts
that **(a)** a shipped build must never default the MCP env-gate on (an on-by-default server is a live
local read/screenshot surface of the game), and **(b)** no MCP tool argument is ever a filesystem
path — every engine reference crosses the wire as an opaque `AssetId`, matching `AssetManager`'s own
external contract (`Mount` takes a raw path and is never exposed to a tool). Close on the "optional,
separately-linked, editor-free" posture. Written to the house comment/doc rules (no plan citations,
present-tense fact).

### 2. Root `CLAUDE.md` — the layout entry

Add an `mcp/` bullet to the **Layout** list and a link in the **Module guides** list, describing
`libveng_mcp` (`veng::mcp`) as the optional MCP server library: links `veng::veng` PUBLIC, json +
cpp-httplib PRIVATE (vendored), not linked by `libveng`, opted into by a consumer, editor-free (the
editor registers its own tools). Note it in the dependency prose beside where graph/editor are
described.

### 3. `docs/guides/` — the consumption guide

A task-oriented guide (matching the existing `writing-gameplay-systems` / `wiring-a-level` guides):
how a game or editor links `veng::mcp`, constructs an `McpServer` with an `McpHost`, pumps it each
frame, registers a custom tool, and connects an MCP client to the loopback endpoint — with the
`hello-triangle` `HT_MCP` wiring as the reference. Cover the editor `GetInspectables()` seam (how a
new panel exposes its reflected model to agents) since that is the extension point a downstream editor
author touches. Add it to `docs/README.md`.

### 4. `future/README.md` — the area note

Record planset-41 as delivered and enumerate the deferred follow-ons as future work (a new area or a
note under the events/input area, since streaming rides those seams): server→client notifications /
SSE streaming; MCP `resources/*` and `prompts/*`; auth / non-loopback exposure; a richer
world-editing vocabulary (reparent, multi-entity transactions); **rich node-graph editing via the
`NodeGraph` serialize + mutation vocabulary** (not a hand-mirrored node API); **reflected commands /
method reflection / a scripting surface** (explicitly *not* built this planset — parked until a broad
need justifies the subsystem); and **programmatic dock-layout choreography** (deferred; floating OS
windows a standing non-goal). Cross-reference the delivered engine seams the MCP library assembled
(reflection, the `GetInspectables()` editor seam, the screenshot download path, the task-pump shape).

### 5. Planset README status

Flip the planset-41 README status column to `done` across the plans as they land, and add planset-41
to `plans/README.md`'s planset index with a one-paragraph summary.

## Verification — the full band

- `cmake -B build -S . && cmake --build build -j` clean; `ctest --test-dir build --output-on-failure`
  green, including the new `mcp_*` tests.
- The **validation build**: `cmake -B build-debug -S . -DVE_DEBUG=ON && cmake --build build-debug`
  clean (the debug build is `-Werror` per [[project_validation_gate_werror]] — a non-exhaustive
  switch in the new tool code would only surface here) and `ctest --test-dir build-debug -L validation`
  (the `validation_gate`) green — the MCP server opens a socket + thread but issues no Vulkan work off
  the render thread, so it must not introduce a validation error.
- `smoke_golden` unchanged (server off in the default smoke path).
- `git clang-format` clean on touched lines; a clang-tidy-enabled build shows no new finding on the
  `veng_mcp` targets (the vendored httplib/stb TUs carry the `-*` override).
- The `/fix-comments`-style pass over the new headers: every public `Veng/Mcp/` declaration carries a
  complete `@brief` Doxygen doc comment (the surface is public API).

## Notes

- This plan is documentation + verification only — no new mechanism. If verification surfaces a real
  defect, fix it here and note it, rather than deferring.
- Keep the `mcp/CLAUDE.md` scoped to *architecture the code does not already state*; do not restate
  the JSON-RPC or MCP spec — reference it and pin the protocol version the server reports.
