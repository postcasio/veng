# veng guides

Hand-written, task-oriented documentation for building with veng. These guides
teach the *patterns* — how the pieces fit and why they are shaped the way they
are. They are separate from the generated API reference, which documents every
symbol: build the Doxygen reference with `cmake --build build --target docs`
(output under `build/docs/html`) and read it alongside these guides.

## Guides

- **[Writing gameplay systems](guides/writing-gameplay-systems.md)** — the full
  path from an empty `SceneSystem` to a system running in a level: the system
  lifecycle, choosing the Sim or View phase, the Input → Intent → Movement
  pattern, configuring a system through components, registering it into the
  catalog, and a worked example built from scratch.
- **[Wiring a level](guides/wiring-a-level.md)** — the `Level` asset from the
  author's side: world prefab versus level-scoped data (game mode, the active
  system set, render settings), why a level is not a prefab, and the
  load-to-play flow.
- **[Authoring input actions](guides/authoring-input-actions.md)** — the
  action-mapping layer: declaring action-id constants, writing a
  `*.inputmap.json` (actions + bindings), activating it from a seat's
  `InputContextStack` in the player prefab, and reading actions by name in a
  control system — plus why `InputMappingSystem` must run before the control
  system and how the context stack switches schemes and gates focus.
- **[Multi-seat input and split-screen](guides/multi-seat-input.md)** — routing
  input per seat: the `SeatInput` component naming each seat's devices, the
  `DeviceAssignmentSystem` auto-assigning pads, the per-seat filtered view, how the
  pointer routes by viewport region (and is inert under cursor capture),
  reconfiguring the managed viewport list into quadrants with
  `ReconfigureManagedViewports`, and spawning a second seat — with the boundary that
  routing stops at `PlayerInput`.
- **[Authoring a UI document](guides/authoring-ui-documents.md)** — the `Veng::Gui`
  game UI layer: authoring a `*.vui.xml` HUD + a `*.vuss` stylesheet + a font,
  cooking them into assets, instantiating a `Gui::Document` and binding it a
  reflected view-model, attaching it to a viewport, resolving its bindings each
  frame, and opening a `SeatFocusScope` to make a menu interactive — plus the
  boundary against `Veng::UI` (the editor/debug ImGui vocabulary).
- **[Diegetic and glowing UI](guides/diegetic-ui.md)** — putting a `Veng::Gui`
  document in the world with a `GuiSurface`: the HDR render target, the translucent
  and opaque-emissive material domains, the `rgb()` linear-float colors that let a
  color exceed 1.0 and bloom through the scene's own bloom, why a screen-space
  overlay stays LDR and does not glow, and the hot-core desaturation gotcha.
- **[Screen-space UI, level overlays, and scene captures](guides/screen-space-ui-and-overlays.md)** —
  the three engine-driven scene components authored as data: presenting a HUD with a
  `GuiOverlay` (the screen-space sibling of `GuiSurface`, its C++ state-component +
  binding-system interface, and seat-based multi-viewport claiming); opening a whole level
  as a secondary, simulated overlay with `LevelOverlay` (the lifecycle, the populate hook's
  contract-versus-guidance, input-suspend versus the opt-in `PausePrimarySim`, stacking, and
  reading results back); and authoring a mirror or probe with a `CaptureSurface` (the
  same-entity material binding and the `everyFrame`/`onDemand` refresh policy).
- **[Consuming veng](guides/consuming-veng.md)** — discovering veng from a game
  project with `find_package(veng)`: the three consumption modes (in-tree, build
  tree, install prefix), the `veng_ROOT` / `CMAKE_PREFIX_PATH` /
  `FETCHCONTENT_SOURCE_DIR_VENG` discovery incantations, and the co-development
  loop that needs no reinstall.
- **[Exposing your app to an agent with the MCP server](guides/consuming-mcp.md)** —
  linking the optional `veng::mcp` library: filling an `McpHost` from your
  systems, constructing and pumping an `McpServer`, the read-only versus mutation
  tool families, connecting an MCP client to the loopback endpoint, the editor host
  and its `GetInspectables()` extension point, and the loopback/`Origin`/mutation
  safety model.

Every type, macro, and method these guides name exists in the engine as written,
and the worked examples cross-reference the real
[hello-triangle](../examples/hello-triangle/) game module so the prose stays
honest against the code.
