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
  input → actions → `PlayerInput` → `Intent` control flow, interaction and vehicles with their
  `PhysicsPoseResolver` seam, the Sim/View tick split, the `SystemRegistry` catalog, game modes, and
  `Level`s.
- **[src/Behavior/CLAUDE.md](src/Behavior/CLAUDE.md)** — `Veng/Behavior/`, the behaviour runtime: a
  behaviour tree built in code, the `BehaviorAgent` component holding a shared tree plus this agent's
  seeded per-node running state, and the `BehaviorSystem` that ticks agents under authority with the
  ECS as blackboard — the AI arm of the `Intent` control pipeline.
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
- **[src/Physics/CLAUDE.md](src/Physics/CLAUDE.md)** — `Veng/Physics/`, rigid-body simulation: the
  per-`Scene` `PhysicsWorld`, `RigidBody`/`Collider` as reflected components, the fixed step in the
  Sim phase, the `PhysicsPose`/`SyncTransform` seam and the two-writer hazard, the closed collision
  layer table, the replay gate, and the Native containment of the vendored solver.
- **[src/Persistence/CLAUDE.md](src/Persistence/CLAUDE.md)** — `Veng/Persistence/`, the
  durable-state subsystem: the `Store`'s families, opaque record keys, atomic whole-slot flush,
  versioning and migration, and the capture/rehydrate scene hooks — plus its opposite number, the
  `DerivedDataCache`, where expendable derived blobs live under a generation that wipes them.
- **[src/Audio/CLAUDE.md](src/Audio/CLAUDE.md)** — `Veng/Audio/`, the audio subsystem: miniaudio
  behind the Native idiom as `AudioDevice`/`AudioEngine`, the fixed `Master/Music/SFX/UI/Ambience`
  bus tree, the real-time mixing thread fed by a triple-buffered voice snapshot, the reclamation
  handshake and lock-free retired-voice channel, the master reverb node, the single `MaxVoices`
  budget, and the null device (headless / device-loss). The callback thread is the one sanctioned
  exception to the single-thread rule, and touches no engine state.
- **[src/Diagnostics/CLAUDE.md](src/Diagnostics/CLAUDE.md)** — `Veng/Diagnostics/`, the CPU
  instrumentation subsystem: the `VE_PROFILE_*` scope/counter/instant vocabulary, per-thread chunk
  rings and their release/acquire publication, RAII thread registration, virtual tracks and the
  back-dated GPU bridge, the `TraceSink` seam and the binary capture format, triggered captures and
  the continuous ring (from code or over the `profile.*` MCP tools), the always-on per-frame
  aggregates the HUD reads, and the `VE_PROFILE` compile gate. Allocation-free and `Log.h`-free on
  the hot path.

The consumption exemplars are documented in [examples/CLAUDE.md](../examples/CLAUDE.md); the
offline cook in [cooker/CLAUDE.md](../cooker/CLAUDE.md); the archive format in
[assetpack/CLAUDE.md](../assetpack/CLAUDE.md); the editor framework in
[editor/CLAUDE.md](../editor/CLAUDE.md); the node-graph/material-codegen library in
[graph/CLAUDE.md](../graph/CLAUDE.md); the optional MCP server library in
[mcp/CLAUDE.md](../mcp/CLAUDE.md).

## Application

Subclass `Application`, override `OnInitialize` / `OnUpdate(delta)` / `OnRender` (and
`OnShutdown` for an engine-alive shutdown operation), and `Run(args)`. ImGui is opt-in (on by default; `nullopt` to skip), and a `Headless` flag runs
windowless to `RequestExit()` instead of a window close — that's the CI/smoke path.
`Run` returns the process exit status: 0 unless the app called `RequestExit(status)`, which the
launcher's `main` returns so a headless or server-shaped consumer can report a failed start.
Calling `RequestExit(status)` from `OnInitialize` is the fatal-startup-failure path — the world
bootstrap is skipped and the run loop never starts, while `OnShutdown`, the session save, and
every destructor still run.

**Teardown order is what makes "hold engine resources as members" safe.** An app's engine
resources are its members, released by its destructor — which runs *before* the engine's own
members (`AssetManager`, `TaskSystem`, `Context`, the registries) tear down, so every service the
release touches is still alive; member declaration order (and explicit destructor logic) encodes
any intra-app ordering. A shutdown *operation* that is not a resource release — one that must run
while the app is fully alive, e.g. flushing state ahead of the engine's own durability save — goes
in the app's **`OnShutdown()`** override, which `Run` invokes before teardown begins. A resource
that outlives the context still fails loudly: the `Disposed` tripwire (set in `~Context`) asserts
on any handle retiring after teardown. The ownership rule these serve (`Ref` vs `Unique`, the
per-frame retire path) is in [the root CLAUDE.md](../CLAUDE.md#resource-ownership--lifetime).

**The engine writes nothing relative to the working directory, and does not move it.** A shipped
application cannot assume its working directory is writable — inside a macOS bundle it is the
bundle, whose contents are sealed by the code signature — so every engine-owned file has an
explicit home. ImGui's layout is the one that would otherwise default to a relative path: the
layer takes `ImGuiLayerInfo::IniPath`, and `Application` fills an unset one with
`UserConfigDir(ApplicationInfo::Name) / "imgui.ini"`, falling back to persisting no layout at all
(with a warning) when no writable configuration directory resolves. The other half is GLFW, whose
Cocoa init `chdir`s into a bundle's `Contents/Resources` by default; `Window` disables that
(`GLFW_COCOA_CHDIR_RESOURCES`), because relocating the host process's working directory is not the
windowing layer's call. A consumer choosing its own path — `ApplicationInfo::PipelineCachePath` is
the other such knob — is unaffected by either.

`Application` owns the `AssetManager` (`GetAssetManager()`), the render `Context`, and the
`TaskSystem` (`GetTaskSystem()`), and threads them explicitly into each other (per-worker
transfer pools in the `Context`, the manager's loaders on the task system). The `TaskSystem` — a
fixed worker pool draining a work queue and returning `Task<T>` handles — is pumped once per
frame: `Frame()` calls `TaskSystem::PumpMainThread()` at the top, before `BeginFrame()` advances
the frame, so off-thread continuations land on the main thread.

**`Veng::ParallelFor(count, body)`** (`Veng/Task/ParallelFor.h`) is the data-parallel complement to
the pool: it splits `[0, count)` into contiguous ranges across short-lived threads it owns for the
call, the caller participating in one range, and blocks until all finish. Because it owns its
threads rather than re-entering the pool, it is safe to call from *any* thread — including a
`TaskSystem` worker, where recursing into the pool could starve it — so a job already running off
the main thread can still fan a coarse inner loop across cores. It is for occasional, CPU-bound
batch work (a one-shot bake, a bulk transform), not per-frame hot paths; steady per-frame work
submits to the pool.

**`Application` is a composition root that delegates to collaborators.** It owns the services
above and three collaborators it drives each frame:

- **`ViewportCompositor`** (`Veng/Renderer/ViewportCompositor.h`) — the render surface. It owns
  the render-order viewport drive-list, the capture drive-list, render-all, and the gather +
  composite tail. `RegisterViewport(Viewport&)` / `RegisterCapture(SceneCapture&)` forward to it:
  each stores a non-owning pointer (registration order = render order) and hands the resource a
  back-reference, so dropping the owner's `Unique` self-unregisters it and the caller keeps
  ownership — only the *driving* is central. It also resolves each `Layout`-carrying viewport's
  pixel region + UI scale on swapchain resize. See [src/Renderer/CLAUDE.md](src/Renderer/CLAUDE.md).
- **`ManagedViewportSet`** (`Veng/ManagedViewports.h`) — the managed-viewport policy. It owns the
  engine-managed `Presented` viewports, registers them into the compositor, and each frame **pulls**
  each viewport's camera from the `WorldRunner` by the viewport's `{ WorldInstanceId, Viewer }`
  binding and pushes it (`PushViews`) — a one-directional gameplay→render bridge.
- **`WorldRunner`** (`Veng/WorldRunner.h`) — the sim-domain scheduler. It owns a **flat set of
  first-class worlds** and ticks every one each frame. Its per-frame render-side drive is narrower
  than its tick: `DriveCaptureSurfaces` walks only the worlds a view **presents**, asked through the
  caller's `IsPresented` hook (`Application::IsWorldPresented`), since a capture rendered from a world
  nothing shows can be sampled by nothing — see
  [src/Renderer/CLAUDE.md](src/Renderer/CLAUDE.md).

**Every `Viewport` has a `ViewportId`.** Minted at `Viewport::Create` and retired at destruction,
resolved through the `Context`-owned **`ViewportRegistry`** (the render-domain registry joining
`BindlessRegistry`). The input layer stores ids, not pointers, and resolves them live: `InputRouter`
and `SeatFocusScope` key each viewport↔seat association by `ViewportId` and re-resolve it against
the registry every hit-test, so a destroyed viewport's association becomes an inert no-op and
address reuse cannot transfer a seat association to a new viewport. See
[src/Renderer/CLAUDE.md](src/Renderer/CLAUDE.md) for the viewport model.

**The managed viewport is a set, and split-screen is a runtime reconfigure of it.**
`ApplicationInfo::ManagedViewport` (`ManagedViewportInfo`: render extent, color format,
`SceneRendererSettings`, a normalized **`Layout`** — an offset + extent in `[0,1]` window
fractions resolved to pixels on construction and every swapchain resize — a `{ WorldInstanceId,
Viewer }` world binding, and render knobs) makes `Application` construct, register, resize-track,
and expose engine-owned `Presented` viewports; `ApplicationInfo::ManagedViewports` is the
multi-viewport form (the singular field is sugar for a one-element set). `GetManagedViewports()`
reaches the `ManagedViewportSet` — `Get(n)` a viewport (index 0 the primary; there is **no**
`GetPrimaryViewport()`), `GetCount()` the size — and **`ReconfigureManagedViewports(span)`** —
applied at a safe point (top of frame, outside iteration) — replaces the set. A viewport names the
world it presents by `WorldInstanceId`; the per-frame pull resolves that world through the
`WorldRunner` and pushes its scene, resolving a bound `Viewer`'s camera (`ResolveCameraView`) or
the scene's primary camera. A viewport whose world was closed renders a cleared target (inert,
never a dangling read); a viewport with no bound world is left for the game to drive through
`SetViewState`. A bound `Viewer`'s region is associated with the `InputRouter` (by `ViewportId`),
so a free pointer over it routes to that seat. The gather + composite tail assembles every
registered `Presented` viewport, so split-screen is "reconfigure to N quadrant `Layout`s," not a
bespoke render path; a single default-`Layout` managed viewport is byte-identical to a
hand-registered full-window one. The editor leaves the managed set unset, so `Get(0)` is null and
it registers its own viewports through the compositor (which still mints their ids).

**A world rebind is a complete operation, and presentation state is queryable.**
`RebindManagedViewport(index, world)` records a deferred rebind applied at the top-of-frame safe
point, where it is a **complete rebind**: it detaches the *departed* world's engine-driven overlay
documents from the viewport (`GuiOverlay::Detach`, the exact inverse of the per-frame `Drive` — the
runtime host survives, only what the engine attached is touched, hand-attached documents untouched),
**re-resolves the seat** in the destination scene (the bound `Viewer` when it still resolves
there, else the scene's sole/first `Viewer`, else cleared), re-pointing the `InputRouter` association
and — when the departed association owned it — the cursor seat, and resetting `Info.Viewer`, and
**re-seeds the viewport's render settings and per-frame view knobs** from the destination's authored
`LevelRenderSettings` (the same seed the bootstrap world takes; a destination authoring none keeps
the viewport's current settings); input *focus* is left to the game. `GetManagedViewportWorld(index)` returns the applied binding and
`GetPendingManagedViewportWorld(index)` the destination of an in-flight rebind (so a pending world
counts as presented and is not reaped in its own rebind gap). **`RebindManagedViewportWhenReady(index,
world)`** is the front-door / world-jump path: it holds the viewport on its current world until the
destination is **ready** (resolves, its scene installed, its simulation started, its `World::Pending`
residency batch resident, and its clock ticked ≥ 1), then swaps in one frame — no empty-world frame,
no consumer polling loop. It is superseded by any later rebind of the same index (last wins); a **timed-out wait retries with
a fresh clock** up to `PresentReadyAttempts`, so a transient stall clears with no consumer recovery
loop, and the rebind is **abandoned** (surfaced through `GetAbandonedManagedPresentWorld(index)`)
either **once the attempts are spent** or immediately if its **destination vanishes mid-wait**
(idle-reaped or closed out from under the wait), so a never-ready or reaped destination does not
strand the viewport on the old world. `ManagedViewportSet` carries the same surface (`GetViewportWorld` /
`GetPendingViewportWorld` / `RebindWorldWhenReady` / `GetAbandonedPresentWorld`).

**The engine's readiness is necessary, and a consumer may say it is not sufficient.**
**`SetWorldPresentReadyGate(gate)`** (`ManagedViewportSet::SetPresentReadyGate`) installs a
`WorldPresentReadyGate` — a `bool(const World&)` predicate the present-on-ready path consults *after*
its own test passes, once per waiting rebind per frame. A destination world runs its systems from the
moment it opens, whether or not anything presents it, so per-world work that must finish before the
first visible frame (a bake, a stream, a generation pass) is already progressing during the wait; the
gate is how a consumer says it has not finished yet, and the outgoing world stays up meanwhile. The
wait clock keeps running while the gate refuses, so a gate that never opens abandons through the
timeout path above rather than stranding the viewport. An unset gate (the default) presents on the
engine's test alone.

**`Application` optionally bootstraps and drives worlds through the `WorldRunner`.** Set
`ApplicationInfo::World` (`GameWorldInfo { path Project; }`) and `Application` runs the game: it reads
the **cooked project** (`<name>.vengproj`) beside the executable (`ReadCookedProject`) and mounts each
pack it names **before `OnInitialize`**, so a subclass can load a cooked asset (a startup palette, a
config table, a boot UI atlas) during initialization; then, at the end of `Initialize` (after
`OnInitialize`), it reuses that same parsed project to open the startup level
as **world #0** through `WorldRunner::OpenWorld` — a first-class `World` bundling
`{ WorldInstanceId, Unique<Scene> (+ its SceneSimulation), a per-world clock, pause state }`. The
open seeds the managed viewport's topology + per-frame view from the spawned scene (the level's
`LevelRenderSettings` post knobs via `ApplyLevelRenderSettings`, plus the scene's author-opt-in
`Sky` and `TimeOfDay`, resolved by the renderer itself each `Execute`), fires the
`OnWorldLoaded(WorldInstanceId, Scene&, ResidencyBatch&)` hook, then starts the simulation, and
binds world #0 to managed viewport #0 (`SetViewportWorld`). Each `Frame` the runner ticks every
world and `ManagedViewportSet::PushViews` pulls each viewport's camera and pushes it.

**The boot session restore is opt-out, and the restore is consumer-triggerable.**
`GameWorldInfo::RestoreLocalSessionOnBoot` (default `true`) has the bootstrap resume the local
account's saved gameplay world once world #0 is bound — the continue-style posture, zero consumer
code. Set `false` and the bootstrap runs no restore at all: world #0 (the startup level) stays
presented and the consumer drives the identical path through the public
**`Application::RestoreLocalSession()`**, once it has opened the store the record lives in. That is
what a consumer whose front end owns the first travel wants (nothing races its own first world
open), and what a player-less headless host — which has no local account to restore — sets. The
restore is reversible: **`Application::ReleaseLocalSession()`** drops the pins it took and evicts
the cached record, so one process can switch stores without relaunching. See
[src/Net/CLAUDE.md](src/Net/CLAUDE.md) for the session-record model and the failure contract.

**An application declares the command-line options it accepts.** The launch parser recognizes the
engine's own flags and fatally rejects every other `--` token before `OnInitialize`, and `Run` is
non-virtual and called from the SDK-owned launcher main — so `ApplicationInfo::LaunchOptions` is how
an application takes an argument of its own. Each `LaunchOptionInfo { Name, TakesValue }` is
consumed by the parser into the **`LaunchArguments::GameOptions`** map (`name → value`; a declared
value-less option that appeared maps to an empty string, a declared option absent from the command
line has no entry), read back through `GetLaunchArguments()`. The unknown-argument guard is intact:
an *undeclared* `--flag` still errors, so a typo is caught rather than ignored. Engine flags are
matched first, so a shared name resolves to the engine flag, and a declared option missing its value
is a parse error with the engine flags' diagnostic shape. Declaring nothing leaves the grammar
exactly as it was.

**Worlds are flat peers addressed by `WorldInstanceId`.** There is no privileged primary world and
no engine code path special-cases world #0 — it is privileged only in that bootstrap opens it
first. Every world API is handle-keyed: `GetWorldRunner()` reaches the runner (a game opens further
worlds at runtime with `OpenWorld`, closes them with `CloseWorld`), `GetManagedWorldId()` returns
world #0's handle, `ResolveWorld(id)->GetScene()` resolves a world's scene, and
`GetWorldViewState(id)` / `GetWorldLevel(id)` / `SetWorldPaused(id, …)` key the managed world's
knobs by handle. The sim domain has **no back-reference out**: a `World` holds no viewport, no seat,
and no `NetRole` — it does not know it is presented or replicated. Presentation points *inward* by
handle (a viewport names its world; `ManagedViewportSet` asks `WorldRunner::ResolveCameraView`, a
pure query), and the runner holds no pointer back. The **minimal game writes no lifecycle or
per-frame code at all** — the bootstrap auto-bind (world #0 → managed viewport #0) is the zero-code
path. `World` unset leaves the app to load and drive its own scene (the editor, or a game wanting
full control), and the runner is device-free when given no asset manager or context (it drives
empty-scene worlds without a GPU).

The world drive is an accumulator: each world's Sim phase steps at its own fixed `SimTickRate`
(`GameWorldInfo`, default 60 Hz) with a monotonic tick, its View phase runs once per frame, and the
render gather blends transforms between the last two ticks — see
[src/Net/CLAUDE.md](src/Net/CLAUDE.md) for the tick model and the `ApplicationInfo::Net` wiring
(`--server` / `--dedicated` / `--join` / `--netsim`, `PumpNet`, and the runtime `StartHosting()` /
`Connect()` / `StopNet()` operations that mount the same hosts after boot). **Pause is a refcount, not
a boolean:**
`WorldRunner::PauseScope(id)` is an RAII pause held for a scope's lifetime (a world is paused while
any scope is held), composing with the explicit `SetWorldPaused(id, …)` toggle so stacked overlays
and a game pause do not clobber each other.

**Networking is per-world and multiplexed over one connection.** `NetRole` is a **per-world**
property, not a process-global one: each world ticks under its own authority (a `Host`-side role map,
`NetState::WorldRoles`, feeds each world's `SystemContext`; the `World` itself holds no role), and the
**two axes are orthogonal** — a world's authority role (`Server`/`Client`) is separate from whether
the *process* binds a transport (`--server`/`--join`, `GetNetRole()`). So a standalone world is
`Server` with no transport, and a single process can host one world while displaying another. One
connection **multiplexes N worlds** across **three id spaces**: the process-private `WorldInstanceId`
(a runner handle, never on the wire), the opaque consumer-defined **`WorldKey`** (a 128-bit name that
rides only the join request), and the per-connection **`JoinId`** (a `u16` wire tag framed ahead of
every world message). A client **joins by `WorldKey`**, which the `ServerHost` resolves through a
**get-or-place policy** — a key maps to N live buckets, and a join lands in an existing bucket (default:
convergence, one bucket, so two clients on a key share one instance) or opens a fresh one through the
`WorldFactory`; the built-in capacity policy (`MaxPlayersPerInstance`, 0 = no max = convergence) buckets
a busy key into instances of ≤ N, each its own `ReplicationServer`. The factory is also the **seam that
keeps single-player net-free** (a standalone process invokes the world-open path with no `Host` and no
replication instantiated at all). Each world carries
its **own replication instance** (one `ReplicationServer`/`ReplicationClient` per world, muxed at the
`Host`), so **ack/baseline isolation is structural over the shared reliability channels** — one
world's ack never advances a peer's baseline — though the wire stream and compute stay coupled, so the
honest guarantee is per-world **convergence, not independent streams**. The join reply echoes a
**content digest** the client validates its reconstructed world against (fail-loud carried into the
join tier); worlds are **server-owned** — refcounted by live joins, idle-reaped after a keep-warm
dwell, and bounded by a server-wide cap (with a per-connection join cap); and clock/tick-sync scopes
**per `JoinId`**. The wire break **fails loudly**: `Net::ProtocolVersion` is **5** and the
`ConnectAcceptMessage` carries only the connection id (the level/seat moved to the per-world join
reply). Who a connection *is* is a consumer-minted, opaque **`Net::AccountId`** presented at the
handshake (the `GameNetInfo::Identity` / `AdmitAccount` hooks) and threaded through seats,
authorization, and directory membership — see [src/Net/CLAUDE.md](src/Net/CLAUDE.md). World lifetime is the role-neutral **`WorldDirectory`** (`Veng/WorldDirectory.h`) — the
`WorldKey → live-instance` map, get-or-place, presence refcount (live joins **plus** presentation
pins), keep-warm dwell, and idle reap — which a `ServerHost` borrows and a standalone `Application`
constructs; travel rides an opaque **`Net::Blob`** through
`Authorize`/`Placement`/`WorldFactory` and the join reply, and the server can **direct** a client's
travel (make-before-break). Beside the directory at the same host tier sits the per-account
**`Net::SessionRegistry`** (`Veng/Net/Session.h`) — each account's standing joins and last gameplay
world as **(key, factory params, pose)**, so **reconnecting is reattaching**: an admitted account's
recorded worlds are restored through the directory (a reaped dynamic world re-materializing from its
params), durable across a host restart through a `LoadSession`/`SaveSession` hook pair. It is not a
component — it outlives the connection and keys by account, so single-player continue and multiplayer
reattach are one code path. Beside the replicated state tier, the hosts carry the **game message
channel**: named (`Net::ChannelId`), reliable-ordered, connection-scoped opaque blobs with
frame-safe receipt — the event complement to world-state (invites, chat, request/response).
See [src/Net/CLAUDE.md](src/Net/CLAUDE.md) for the full model.

**Application-level operations are reached from gameplay through builtin request components.** A
`SystemContext` carries no `Application` back-reference, so a gameplay system cannot call the
operations that open and close worlds, bind the transport, exit, or hold an input-focus token. The
builtin, **local-only** request components (`Veng/Scene/Requests.h`) are that data channel:
`TravelRequest`, `HostRequest`, `ConnectRequest`, `StopNetRequest`, `ExitRequest`, and
`FocusRequest`. A system stamps one onto any world's scene; `Application::Frame` **drains** them at
its frame-safe point (right after the deferred managed-viewport reconfigure, before the world tick),
in the fixed order **StopNet → Host → Connect → Travel → Exit** over a snapshot of the open worlds.
None is `VE_REPLICATED` — a request never rides a snapshot, and on a `Client`-tier world it lowers
to the client-side meaning. Consumption is uniform: a handled request is **removed** (absence is the
ack), an unhandleable one is left **Pending** to retry, and a failed one is marked
`RequestStatus::Failed` with an `Error` and held exactly one frame so the stamper can read the
outcome before re-stamping. **`Application::Travel(TravelInfo)`** is the one travel primitive the
`TravelRequest` drain lowers onto — resolving standalone (directory get-or-place → present-on-ready
rebind → pin/unpin), client (travel-request → server-directed travel), or listen-host — and
`FocusRequest` drives the `InputRouter`'s coarse gameplay/UI focus for a seat through an
engine-owned per-seat token (so a stateless system can capture or release focus). See
[src/Scene/CLAUDE.md](src/Scene/CLAUDE.md) for the request idiom and
[src/Net/CLAUDE.md](src/Net/CLAUDE.md) for `Travel`.

**A second level can be opened over the running one as a `LevelOverlay`.** `LevelOverlay`
(`Veng/LevelOverlay.h`) is a thin **preset over `WorldRunner::OpenWorld`**: opening an overlay
opens an owned world (its own scene, systems, and HUD, ticked by the runner like any world) and
applies an **overlay policy** — register a `Presented` viewport on top (composited over the covered
world, its camera pulled by the managed-viewport presentation path and its region re-fit on resize
by the compositor), hand the cursor seat and the covered seat's focus off to the overlay's own seat
(a `SeatFocusScope`), and hold a refcounted `WorldRunner::PauseScope` on the caller-named
`CoveredWorld`. The one cross-scene seam is `LevelOverlayInfo::Populate` (run after load, before
start). **There is no `LevelOverlay::Update`:** the runner ticks the overlay's simulation and the
engine pushes its camera each frame, so there is no per-frame game call and no hidden second
scheduler. **Input focus and simulation pause are separate knobs:** taking the overlay's seat
always suspends the covered seat's *input*, but a world simulates unless a `CoveredWorld` pause is
held. An overlay's `GetViewState()` knobs are captured at open — retuning them takes a re-open, not
an in-place per-frame edit. Dropping the handle (or `Close`) unwinds the policy LIFO, restoring
every router / cursor-seat / pause value to the state it captured at open, and closing the world;
overlays **stack** (a dialog over a modal) and the handles drop LIFO. Results flow back through a
game-owned channel (a component the opener drains, a callback) — no overlay system reaches into the
covered scene.

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
  load**. The ABI is at **version 12** (`VENG_MODULE_ABI_VERSION`, `Veng/Module/Module.h` — the
  header is authoritative). The host struct is `{ ApplicationRegistry& App; TypeRegistry& Types;
  SystemRegistry& Systems; AssetTypeRegistry& AssetTypes; AssetLoaderRegistry& AssetLoaders;
  GuiDriverRegistry* Drivers; EditorRegistry* Editor; }` — the `Drivers` registry (the
  per-instance presentation-binding catalog, see [src/Gui/CLAUDE.md](src/Gui/CLAUDE.md)) bumped
  the ABI 5 → 6, and the asset-type + loader-factory pair (game-defined asset types, see
  [src/Asset/CLAUDE.md](src/Asset/CLAUDE.md)) bumped it 6 → 7. Version 8 left the host struct
  alone: what changed was `AssetTypeInfo`, which a module passes *through* `AssetTypes` **by
  value** and which grew `HandleFieldType`, so a stale module would register a short struct —
  the handshake covers everything crossing the boundary, not only the host layout. **Version 9**
  is the same class of change one level down: `FieldDescriptor` grew an
  `AllowUnreplicatedReference` flag (a reflected `Entity` field declaring it may name a
  non-replicated target — see [src/Net/CLAUDE.md](src/Net/CLAUDE.md)), and a module registers its
  component descriptors *through* `Types`, so a module built against ABI 8 would register a short
  descriptor and be read past its end. **Version 10** is the same class again, on the editor seam:
  `AssetEditorContext` — which the host constructs and passes to a module-registered asset-editor
  factory's `OpenEditor` — grew the host audio engine, the asset's source path, and the recook
  `CookDriver` (so a game panel can audition and save, not only view — see
  [editor/CLAUDE.md](../editor/CLAUDE.md)), so a game module built against ABI 9 would read those
  fields past the end of a short host-constructed context. **Version 11** grows the same
  `AssetEditorContext` once more, with the host `TaskSystem&` (so a game panel offloads a heavy
  export or bake off the UI thread, the way the cook-on-demand already runs on it), so a module
  built against ABI 10 reads it past the end of a short context. **Version 12** adds the editor
  `StatusTracker&` to that same context (so a game panel's background task reports into the status
  bar beside the cook), a module built against ABI 11 reading it past the end. The
  gameplay *simulation* layer still adds **no** ABI surface: game modes are systems + components,
  the system catalog rides a per-system trait the way a component's `TypeId` does, and a `Level`
  is an asset — registered through the existing registries or authored as data, never through a
  new host entry.
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
