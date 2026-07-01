# Plan 05 — SDK install + hello-triangle wiring + conformance

**Goal:** make `veng::mcp` a first-class SDK artifact an out-of-tree `find_package(veng)` consumer can
link, wire the **maximal** sample (`hello-triangle`) to expose an MCP server behind an env flag as the
worked example, and add a headless conformance smoke that proves an installed/linked server answers
the MCP handshake and lists the engine tools. Depends on Plans 00–04b **and on planset-40** (it edits
the install/export block planset-40 is reworking).

## The starting point

- planset-40 turns veng into an installable SDK: `install(TARGETS veng … EXPORT vengTargets)` (+
  `assetpack`, `graph`, `editor`) into an export set, a generated `veng-config.cmake`
  ([`cmake/veng-config.cmake.in`](../../cmake/veng-config.cmake.in)), a build-tree `export(EXPORT)`
  for co-development, and the `veng_add_*` authoring vocabulary. `veng::graph` joining the export set
  is the exact precedent for `veng::mcp` joining it.
- `hello-triangle` is the maximal sample + smoke; it is a game module + launcher, driven headless in
  `HT_SMOKE` mode ([examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp)). Its
  `Application` subclass owns the managed viewport and pumps its own per-frame logic in `OnUpdate`.
- `examples/template` is the minimal sample; planset-40 makes it the out-of-tree `find_package`
  exemplar (removed from the engine build, exercised only by the SDK conformance test).
- The relocatable-trio + build-time conformance tests (planset-40's Plan 05) are the model for a
  staged-install-then-consume check.

## What lands

### 1. Install `veng::mcp` into the SDK

On top of planset-40's completed install machinery:

- `mcp/CMakeLists.txt` gains the install block: `install(TARGETS veng_mcp EXPORT vengTargets …)`,
  install the `mcp/include/Veng/Mcp/` public headers, and (since json/httplib are PRIVATE and
  vendored, not found) **no new `find_dependency`** in `veng-config` — the vendored httplib header
  and `JSON_NOEXCEPTION` nlohmann usage are compiled into `libveng_mcp`, so a consumer linking
  `veng::mcp` needs nothing extra. Confirm the export set + `veng-config` alias (`veng::mcp`) resolve
  in all three consumption modes (in-tree, build-tree, install prefix) the same way `veng::graph`
  does.
- `veng::mcp` is **always built** from a full source build (not behind a build option) and installed
  as part of the SDK; a consumer opts in purely by linking it. This matches planset-40's "veng is the
  tools" posture — built, installed, linked-on-demand.

### 2. Wire `hello-triangle` as the worked example

- `hello-triangle`'s `CMakeLists.txt` links `veng::mcp` (in-tree, via the target).
- Its `Application` subclass constructs an `McpServer` when `HT_MCP=<port>` (or `HT_MCP=1` for a
  default port) is set, fills the `McpHost` from `GetTypeRegistry()`/`GetAssetManager()`/the managed
  world/`GetPrimaryViewport()`, and calls `server->Pump()` at the top of `OnUpdate` (before the
  spinner logic and render). `AllowMutations` follows a second env flag (`HT_MCP_WRITE`), default off.
  Kept env-gated so the default smoke path and the shipped sample do not open a socket.
- This is the ~10-line consumer recipe the docs (Plan 06) point at: link the lib, construct with an
  `McpHost`, pump once per frame.

### 3. `examples/template` stays minimal

The template is the smallest correct app and the standing minimal-conformance check; it does **not**
gain an MCP server (MCP is not part of "the smallest correct app"). Its migration this planset is
**none in code** — MCP is additive and optional. The docs show the template-as-`find_package`-consumer
how to add `veng::mcp` if desired, but the shipped minimal sample stays MCP-free. (This keeps the
co-migration rule satisfied: the template still compiles and runs unchanged.)

### 4. The conformance smoke

A headless ctest (`mcp_conformance`) that is the build-time analogue of the loopback tests but through
the **shipping consumer path**:

- Runs `hello_triangle-launcher` (or the game) under `HT_SMOKE` **plus** `HT_MCP=<ephemeral>`, and
  from the test harness performs `initialize` → `tools/list` over loopback, asserting the engine tool
  families are present (`world.*`, `render.*`, and — with `HT_MCP_WRITE` — `entity.set_field` etc.)
  and that `world.list_entities` returns the sample scene's entities. Labelled `gpu` (it drives the
  real render path), `SKIP_RETURN_CODE 77`.
- If the staged-install conformance harness from planset-40 is in place, additionally add an
  **out-of-tree** consumption check: a throwaway configure+build of a tiny app that
  `find_package(veng)` + `target_link_libraries(app veng::mcp)` + constructs a server, proving the
  installed `veng::mcp` alias and headers resolve. Reuse planset-40's staged-install fixture rather
  than building a new one.

## Notes & constraints

- **This plan is the only one that touches the install/export block** — merge it *after* planset-40
  lands so it stacks on the finished `install(EXPORT)`/`export(EXPORT)`/`veng-config`, not a moving
  target. Everything the earlier plans built is inside the from-source build and needs no install
  edit.
- Keep the server **env-gated** in the sample so the default `HT_SMOKE`/golden path is byte-identical
  (no socket, no thread) — the golden capture must not move.

## Files (sketch)

- `mcp/CMakeLists.txt` — the install/export block joining `vengTargets`.
- `cmake/veng-config.cmake.in` — the `veng::mcp` alias recreation (mirroring `veng::graph`), if
  planset-40's config needs an explicit alias line.
- `examples/hello-triangle/CMakeLists.txt` — link `veng::mcp`.
- `examples/hello-triangle/main.cpp` — the env-gated `McpServer` construct + pump.
- `tests/…` — `mcp_conformance` + (optionally) the out-of-tree consumption fixture.

## Verification

- Clean build; `ctest` green including `mcp_conformance` where a device exists.
- **The `smoke_golden` capture is unchanged** (server is off in the default smoke path) — re-run
  `smoke_golden` to confirm no golden drift, per [[project_mv_bak_stale_rebuild]] re-verify after any
  rebuild trickery.
- A staged install (planset-40 fixture) exposes `veng::mcp`; a throwaway consumer links it and
  constructs a server.
- The full `hello_triangle-launcher` smoke still writes a correct-sized PPM and exits 0.
