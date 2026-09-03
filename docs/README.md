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
- **[Writing AI behaviours](guides/writing-ai-behaviors.md)** — the behaviour
  runtime: building a `BehaviorTree` from composites, decorators, and consumer
  `Task` leaves; the ECS as the blackboard; giving an entity a `BehaviorAgent`
  and a seed; how the `BehaviorSystem` resolves the pawn and ticks under
  authority; and why an AI is just another `Intent` producer.
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
  contract-versus-guidance, input-suspend versus the opt-in `CoveredWorld` pause, stacking, and
  reading results back); and authoring a mirror or probe with a `CaptureSurface` (the
  same-entity material binding and the `everyFrame`/`onDemand` refresh policy).
- **[Authoring a data table](guides/authoring-data-tables.md)** — the
  `TableSchema` / `DataTable` pair: declaring columns as reflected types (and the
  fully-qualified type-name spelling authoring JSON uses), which types can be a
  key, authoring rows and what the cook rejects, reading a row back through
  `FindRow` plus the zero-copy `GetColumn<T>` / general `ReadCell` / typed
  `ReadRow<T>` accessors, the fixed-stride fast path nothing should branch on,
  and the two editor panels.
- **[Consuming veng](guides/consuming-veng.md)** — discovering veng from a game
  project with `find_package(veng)`: the three consumption modes (in-tree, build
  tree, install prefix), the `veng_ROOT` / `CMAKE_PREFIX_PATH` /
  `FETCHCONTENT_SOURCE_DIR_VENG` discovery incantations, and the co-development
  loop that needs no reinstall.
- **[Defining your own asset type](guides/custom-asset-types.md)** — the open
  asset-type space: minting an `AssetTypeId`, sharing one cooked-layout header between
  a runtime loader and an offline importer, the `lib<game>_cook` cook module and why it
  links `veng::cook_interface` rather than the cooker, registering the identity and a
  loader factory through `VengModuleRegister`, the `COOK_SOURCES` / `COOK_MODULE` build
  wiring, and the module-handle lifetime rule the registries impose.
- **[Networking: server-authoritative multiplayer](guides/networking.md)** — building
  a client/server game: the server-authoritative model and fixed tick, marking a
  component to replicate with `VE_REPLICATED`, a connection becoming a seat and the
  owner-threading spawn rule that pawns it, the authority-filter idiom for a game's
  Sim systems, the launch modes (listen server, dedicated server, joining client),
  and the two-world in-process integration suite — plus what v1 leaves to a later
  phase (prediction, delta compression, interest management).
- **[Exposing your app to an agent with the MCP server](guides/consuming-mcp.md)** —
  linking the optional `veng::mcp` library: filling an `McpHost` from your
  systems, constructing and pumping an `McpServer`, the read-only versus mutation
  tool families, connecting an MCP client to the loopback endpoint, the editor host
  and its `GetInspectables()` extension point, and the loopback/`Origin`/mutation
  safety model.
- **[Viewing a profiling capture](guides/profiling-captures.md)** — converting a
  binary `.vtrace` capture to Chrome Trace Event JSON with `vengtrace` and opening
  it in Perfetto or speedscope: the `convert` command and its options, the exit-code
  map, what each track (frames, CPU threads, GPU, counters, instants) means, and why
  the JSON is a lossy viewer-facing projection nothing in veng reads.

## Reference data

- **[Build-cost baseline](build-cost-baseline.md)** — the checked-in whole-tree compile cost
  this repository is measured against, with the provenance that makes the figure meaningful.
  Generated and checked by `scripts/check_build_cost.py`; see the README's
  [Build-time tracing](../README.md#build-time-tracing) section for when to run the check and
  how to read a delta.

Every type, macro, and method these guides name exists in the engine as written,
and the worked examples cross-reference the real
[hello-triangle](../examples/hello-triangle/) game module so the prose stays
honest against the code.
