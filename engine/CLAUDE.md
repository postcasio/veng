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
  durable-state subsystem: the `Store`'s families, opaque record keys, atomic whole-slot flush
  (a snapshot on the calling thread, the write in the background, per-family flush intervals),
  versioning and migration, and the capture/rehydrate scene hooks — plus its opposite number, the
  `DerivedDataCache`, where expendable derived blobs live under a generation that wipes them.
- **[src/Audio/CLAUDE.md](src/Audio/CLAUDE.md)** — `Veng/Audio/`, the audio subsystem: miniaudio
  behind the Native idiom as `AudioDevice`/`AudioEngine`, the data-driven `AudioBusGraph` bus tree
  (a game-authored topology adopted at boot, flattened into the snapshot for a tree-free RT fold),
  the real-time mixing thread fed by a triple-buffered voice snapshot, the reclamation
  handshake and lock-free retired-voice channel, the master reverb node, the single `MaxVoices`
  budget, and the null device (headless / device-loss). Every voice belongs to its scene's presentation
  scope, started through the `ScopedAudio` facade `SystemContext::Audio` is (with its replay gate), so a
  paused world's sound holds, an unpresented one's is silent, and a closed one's stops; each scope has
  its own listener, and the engine advances once per frame. The callback thread is the one sanctioned
  exception to the single-thread rule, and touches no engine state.
- **[src/Haptics/CLAUDE.md](src/Haptics/CLAUDE.md)** — `Veng/Haptics/`, gamepad rumble: the
  general `Curve1D` keyframed scalar, the cooked CPU-only `RumbleClip` asset (four motor curves), the
  `RumbleSource` component the View-phase `HapticsSystem` plays, the scoped fire-and-forget one-shot
  reached as `SystemContext::Haptics` (with its replay gate), scene-local seat targets, and the
  `HapticsEngine` — a per-pad maximum mixer, written once per frame as the motors' only writer. Every
  rumble belongs to its scene's presentation scope, so it holds with a pause, is silent while the
  scene is unpresented, and ends with the scene.
- **[src/Capture/CLAUDE.md](src/Capture/CLAUDE.md)** — `Veng/Capture/`, the video recorder:
  `VideoRecorder` as the compositor's `CaptureSink`, taking a fresh encoder-owned buffer per frame and
  appending it at the frame slot's retirement; the device-free `RecorderCore` (slot map, both
  timestamp modes, the bounded never-drop wait, the drain that needs no future frame); the platform
  backend seam and its Apple implementation over AVAssetWriter and VideoToolbox; the SDR/HDR10
  encoding table; and what the headless band proves versus what only a windowed recording can.
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

**`Run` ends with its operations, in a fixed order:** (1) a running video capture is stopped and
finalized; (2) quiesce — `WaitIdle`, `WaitForAll`; (3) `OnShutdown()`; (4) the session `SaveAll()`;
(5) **`WorldRunner::CloseAllWorlds()`** — every open world closed newest first, each system's `OnStop`
running with a context the factory builds over the live services, the world's own id and its own
role; (6) `WaitForAll()` again, for anything an `OnStop` queued. So **`OnShutdown` and the save see
every world open; then every world's systems stop, while every engine service and every application
member is still alive; then members are destroyed.** The close comes after rather than before the
two because both read live worlds (a shutdown checkpoint captures each open world's state). The
consequences for a consumer: a service `OnShutdown` stops is already gone when the `OnStop`s run, an
`OnStop` wanting its effect durable flushes it itself, and an application-held handle onto a world —
a `SeatFocusScope`, a `WorldPauseScope` — routinely outlives its world and must tolerate it (both
do).

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
the pool: it splits `[0, count)` into contiguous ranges and blocks until all finish. When a
`TaskSystem` pool is ambient on the calling thread — true on every worker and on the main thread —
the split runs *on that pool* through `TaskSystem::RunParallel` with the caller participating, so it
spawns no threads and total concurrency stays bounded to the pool plus the caller. Off a
veng-spawned thread (a unit test, an external `std::thread`) no pool is ambient and it falls back to
owning short-lived threads for the call. Caller participation is what keeps it safe from a
`TaskSystem` worker — the caller's own claim-loop completes every range even if no helper runs, so a
call from a worker (or a nested call) cannot starve or deadlock the pool — so a job already running
off the main thread can still fan a coarse inner loop across cores. Concurrency is
bounded/best-effort, not a guaranteed thread per range. It is for occasional, CPU-bound batch work
(a one-shot bake, a bulk transform), not per-frame hot paths; steady per-frame work submits to the
pool.

**The frame clock has two modes, and one of them is what a recording rides.** `Frame` reads one
clock — `Time::Update()` — and every consumer (the fixed-step simulation, the views, the audio pump,
the engine-global shader clock) takes that delta. In **wall** mode it is the high-resolution wall
clock. **`DriveFrameClock(FrameClockInfo)`** puts it in **driven** mode, where it returns a fixed
`1/fps` however long the frame actually took: the world advances one step per frame at the nominal
rate, the run-loop frame cap is skipped, every managed viewport's render scale is held at its
ceiling (so frame-time pressure cannot soften the picture), and the audio device is driven — its
hardware stopped, each pump mixing exactly the frame's samples. `Time::Now()` stays wall time
throughout, so session timeouts, directory reaping and the net pumps keep their real cadence.
`ReleaseFrameClock()` reverses all of it; both take effect at the top of the next frame, and
`IsFrameClockDriven()` reports the applied state.

**`GetVideoRecorder()`** is the one consumer that drives that mode for its own reason: the
`Capture::VideoRecorder` records what the application presents to a video file through the
platform's hardware encoder, and a **lockstep** capture is exactly the driven clock — every frame
simulated, rendered and encoded before the next begins, so the file plays back at the full rate
however slowly the machine rendered it, with the sound track sample-locked to the picture. The
recorder exists in every build and on every platform; where the platform has no encoder behind a
shareable surface, or the run is headless and presents no frame, `IsAvailable()` is false and
`Start` refuses with a reason. A running capture is stopped and its file finalized before the
engine's services tear down. See [src/Capture/CLAUDE.md](src/Capture/CLAUDE.md) — which also carries
the engine-owned control panel (`Veng::UI::VideoCapturePanel`) and the `render.capture_*` tools a
host reaches the recorder through.

**`Application` is a composition root that delegates to collaborators.** It owns the services
above and three collaborators it drives each frame:

- **`ViewportCompositor`** (`Veng/Renderer/ViewportCompositor.h`) — the render surface. It owns
  the render-order viewport drive-list, the capture drive-list, render-all, and the gather +
  composite tail (the gather running only when placements need assembling).
  `RegisterViewport(Viewport&)` / `RegisterCapture(SceneCapture&)` forward to it:
  each stores a non-owning pointer (registration order = render order) and hands the resource a
  back-reference, so dropping the owner's `Unique` self-unregisters it and the caller keeps
  ownership — only the *driving* is central. It also hands every viewport it registers the device
  engines its Gui drivers play sound and rumble through (`SetDevices`, set once by the Application). It also resolves each `Layout`-carrying viewport's
  pixel region + UI scale on swapchain resize. And it **drives the scenes' authored
  `CaptureSurface`s from the viewports that present them**: a pre-pass ahead of the capture renders
  drives each scene a registered viewport will render this frame — once, from its first such
  viewport — plus a waiting rebind's destination, building at most one new capture per frame and
  reusing a released one from the compositor's `SceneCapturePool`. See
  [src/Renderer/CLAUDE.md](src/Renderer/CLAUDE.md).
- **`ManagedViewportSet`** (`Veng/ManagedViewports.h`) — the managed-viewport policy. It owns the
  engine-managed `Presented` viewports, registers them into the compositor, and each frame **pulls**
  each viewport's camera from the `WorldRunner` by the viewport's `{ WorldInstanceId, Viewer }`
  binding and pushes it (`PushViews`) — a one-directional gameplay→render bridge.
- **`WorldRunner`** (`Veng/WorldRunner.h`) — the sim-domain scheduler. It owns a **flat set of
  first-class worlds** and ticks every one each frame. It carries no render state: what a world
  presents is drawn, and its captures driven, by the viewports presenting its scene.

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
it registers its own viewports through the compositor (which still mints their ids), binding a
playing document's viewport to its play world as below.

**A viewport the engine did not build presents a world by registering as its presentation.**
`ManagedViewportSet::RegisterBoundViewport(viewport, BoundViewportInfo)` binds a caller-owned
viewport of **any role** to a world — `{ World, Viewer, PullsCamera }` — and from then
on that world is presented exactly as a managed viewport's is: `CollectPresentingSeats` returns its
`Viewer` so `SyncLocalControl`
stamps `LocalControl` on that seat's pawn, and the context factory resolves the world's
`SystemContext::View`/`Debug` from it through **`FindPresentingViewport(world, scene)`** — managed
viewports in index order, then bound ones in registration order, each matching only once its
retained scene is the world's live scene (so a bound viewport not yet pushed engages nothing). A
`Presented` viewport a consumer registers and drives itself, never binding it, still gives its
scene's world a `View` after those. Captures need no binding at all: any registered viewport that
renders a scene drives that scene's capture surfaces. `PullsCamera` (default
true) has `PushViews` pull the seat's camera into the viewport each frame; false leaves the
viewport's `ViewState` to its owner — the editor's Offscreen document viewport, which renders Play
through a camera it resolves itself. Registration hands the viewport the set's Gui driver catalog,
translator and localization, and `UnregisterBoundViewport` clears them again, so a viewport kept
past its binding drives no overlay; the sound and rumble engines its drivers play through come from
the compositor it is registered on, bound or not. **A viewport its host stops drawing stops
presenting.** An on-demand viewport (`RenderOnDemand`) whose owner lets a whole frame pass without
pushing a view releases its scene at that render and reads `IsShown() == false`, so a bound one stops
counting for `CollectPresentingSeats` and gives its world no `View` — the
world's sound and rumble mute, its captures stop, its seat's `LocalControl` lifts — until the next
push. That is what makes a hidden editor Play tab unpresented, with no editor code. `ResolvePresentationSeat(scene, boundViewer)` (also
in `Veng/ManagedViewports.h`) is the seat rule a rebind applies and a binder resolves its `Viewer`
with.

**A world rebind is a complete operation, and presentation state is queryable.**
`RebindManagedViewport(index, world)` records a deferred rebind applied at the top-of-frame safe
point, where it is a **complete rebind**: it detaches the *departed* world's engine-driven overlay
documents from the viewport (`GuiOverlay::Detach`, the exact inverse of the per-frame `Drive` — the
runtime host survives, only what the engine attached is touched, hand-attached documents untouched),
**re-resolves the seat** in the destination scene (the bound `Viewer` when it still resolves
there, else the scene's sole/first `Viewer`, else cleared), re-pointing the `InputRouter` association
and — when the departed association owned it — **moving the cursor seat with the focus it holds**
(`InputRouter::MoveCursorSeat`), and resetting `Info.Viewer`. The destination's look needs no
re-seed: the viewport resolves the destination scene's `RenderLook` itself the first frame it
renders it (see below; a destination authoring none keeps the viewport's current settings). The carried focus is the user's, not the departed world's: a
captured cursor stays captured across the swap rather than releasing until the destination
re-requests it, and a UI layer above it comes along too. Beyond that carry, input focus is left to
the game. `GetManagedViewportWorld(index)` returns the applied binding and
`GetPendingManagedViewportWorld(index)` the destination of an in-flight rebind (so a pending world
counts as presented and is not reaped in its own rebind gap). **`RebindManagedViewportWhenReady(index,
world)`** is the front-door / world-jump path: it holds the viewport on its current world until the
destination is **ready** (resolves, its scene installed, its simulation started, its `World::Pending`
residency batch resident, its clock ticked ≥ 1, and its visible `GuiOverlay`s' documents instantiated
— the wait prepares them, `GuiOverlay::Prepare`, so the presenting frame does not), then swaps in one
frame — no empty-world frame,
no consumer polling loop. It is superseded by any later rebind of the same index (last wins); a **timed-out wait retries with
a fresh clock** up to `PresentReadyAttempts`, so a transient stall clears with no consumer recovery
loop, and the rebind is **abandoned** (surfaced through `GetAbandonedManagedPresentWorld(index)`)
either **once the attempts are spent** or immediately if its **destination vanishes mid-wait**
(idle-reaped or closed out from under the wait), so a never-ready or reaped destination does not
strand the viewport on the old world. `ManagedViewportSet` carries the same surface (`GetViewportWorld` /
`GetPendingViewportWorld` / `RebindWorldWhenReady` / `GetAbandonedPresentWorld`).

**Both outcomes of a rebind are delivered, not polled.** The engine knows the exact frame a viewport
starts presenting a world and the exact frame a present-on-ready request gives up, so it says so
rather than leaving a consumer to compare the queries above against a remembered value every frame.
**`OnWorldPresented(index, world, seat)`** fires once per completed rebind — deferred or
present-on-ready — at the same frame-safe point the rebind applied on, *after* the seat association,
the cursor seat and the unbound-seat resolution have settled, carrying the seat the viewport ended up
adopting (`Entity::Null` when the destination seats none). It is where a consumer gives a presented
seat its input posture, by stamping one `FocusRequest`; the seat arrives already holding whatever
focus the cursor carried in, and beyond that carry focus policy stays the consumer's. **`OnWorldPresentAbandoned(index, destination)`** fires once when a
present-on-ready request is abandoned, so a transition aborts on the frame it failed; the
`GetAbandonedManagedPresentWorld` record stands afterwards for a reader that arrives late. Both run
beside `OnWorldArrival` and after it, so arrival state is applied before the presentation moment is
reported.

**Leaving presentation is delivered too.** **`OnWorldDeparted(world)`** fires once when the last
managed viewport presenting a world stops presenting it (typically a rebind to another world) while
the world stays open. It fires from `SyncPresentationPins`, *before* the world is unpinned — so the
world is alive, still pinned, and has not ticked since the last frame that presented it — which is
where a consumer tidies what its local player left there before anything else sees it. The frame
order is `FireWorldArrivals` → `FirePresentationHooks` → `SyncPresentationPins`, so in the frame of a
switch `OnWorldPresented` for the destination precedes `OnWorldDeparted` for the source. Departure is
judged on the applied bindings only, never a pending destination, so an abandoned present-on-ready
destination does not depart (`OnWorldPresentAbandoned` covers it); a world the runner no longer
resolves closed while presented and does not depart either (`OnWorldClosing` covers a factory-opened
one); and nothing fires at shutdown.

**The OS cursor hides where a presented document already draws one.** `GuiOverlay::DrawsCursor` (a
reflected field, default false) declares that an overlay's document renders a pointer of its own;
nothing draws two pointers on purpose, so the engine hides the OS cursor while such an overlay is
presented and restores it on the frame the last one goes away. The rule is scoped to the **viewport
drive-list** the `GuiConsumer` walks — not the managed set — so an overlay world presented through a
viewport a consumer registered itself counts, and it reads each viewport's routable document list
(`Renderer::Viewport::IsDrawingCursor` over `GetInputDocuments`), which means an overlay that goes
display-only or hidden stops counting the same frame it stops taking input. Ownership is explicit:
the engine writes cursor visibility only from the first frame it sees such an overlay, and an
application that authors the flag nowhere is never written to — so a consumer managing the cursor
itself simply does not author it. The immediate-mode layer keeps the cursor whenever it wants the
mouse, its widgets drawing no pointer of their own.

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
`{ WorldInstanceId, Unique<Scene> (+ its SceneSimulation, which holds the pause), a per-world clock }`. The
open spawns the level (its render block seeded as the scene's `RenderLook`, which the presenting
viewport resolves itself — below — beside the scene's author-opt-in `Sky` and `TimeOfDay`, resolved
by the renderer each `Execute`), fires the
`OnWorldLoaded(WorldInstanceId, Scene&, ResidencyBatch&)` hook, then starts the simulation, and
binds world #0 to managed viewport #0 (`SetViewportWorld`). Each `Frame` the runner ticks every
world and `ManagedViewportSet::PushViews` pulls each viewport's camera and pushes it.

**The three per-machine settings stores are built and loaded before `OnInitialize` too.** Graphics
(`graphics.json`), audio (`audio.json`, only when `ApplicationInfo::AudioSettingsSchema` names a
schema) and language (`locale.json`, with the index-backed `Localization` service built on the
language it names) need only the mounted packs and the user config directory, so `Run` constructs
and `Load()`s each between the pack mount and `OnInitialize`. A consumer therefore reads its
persisted choices while initializing — telling a first run from a returning one through
`SettingsStoreBase::WasLoadedFromFile()`, which keeps reporting that one boot load for the rest of
the run — and acts on them before the first presented frame, with no first-update flag.
**Applying them is the consumer's**, so `OnResolveGraphics` never runs before `OnInitialize` and an
app with no settings opinion is unchanged; audio is the single exception the engine applies for it,
once after `OnInitialize`, when the authored bus graph has been adopted.

**A scene's render look is a component, and every viewport resolves the look of the scene it
presents.** `RenderLook` (`Veng/Scene/Components.h`; one per scene, the first walked winning with a
warning, the `Sky` rule) carries the view-wide post and pipeline knobs — exposure and the tone curve,
auto-exposure, bloom, shadows, AO, SSR, refraction, depth of field, the ambient floor. A level's
`render` block seeds it (`SeedLevel`), a prefab may carry one, and a system may write it. Each
`Renderer::Viewport` reads its presented scene's `RenderLook` in `Render`, before the `SceneView` is
built: when the look differs from the last one it resolved (or `InvalidateLook` was called) it runs
its **look resolver** (`Viewport::SetLookResolver`) from its current settings and `Configure`s the
result — a no-op for an unchanged topology — and every frame it writes the resolved per-frame knobs
over the view it was pushed (`CopyLookKnobs`). So a look edit is live on the next frame (a per-frame
field with no rebuild, a topology field with exactly one), two worlds presented at once keep two
looks, and a prefab bringing a look into a scene that had none applies it. A scene with no look keeps
the pushed values. A view push therefore carries only the scene, camera, delta and alpha — plus the
output calibration below — and `Application` holds no view knobs of its own.

**The player's quality composes per viewport, through `OnResolveGraphics`.** `Application` hands its
compositor a look resolver (`ViewportCompositor::SetLookResolver`), which the compositor gives every
viewport it registers — managed, overlay, a consumer's own: it maps the look (`ApplyRenderLook`) and
runs `OnResolveGraphics` over it with `GraphicsResolveInput::AuthoredLook` that look — one composition
rule, run per viewport and per look change rather than per presentation path. It reuses the existing
resolve virtual rather than adding a second one, because a look *is* the authored input that seam
already takes. **`ApplyGraphicsSettings`** applies the machine-shaped half itself — the display group,
the output calibration (`OutputBrightness`/`OutputGamma`, carried on every managed and overlay push
through `ManagedViewportSet::SetOutputCalibration`), each managed viewport's render scale and
dynamic-resolution choice, and the primary's `Global` facet — and has every registered viewport
re-run its resolver once at its next render (`ViewportCompositor::InvalidateLooks`). The default
`OnResolveGraphics` is the identity, so a consumer with no resolver — and the editor, whose authoring
previews show what the author wrote — sees each look exactly as authored.

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
`GetWorldLevel(id)` / `SetWorldPaused(id, …)` key the managed world's
state by handle. The sim domain has **no back-reference out**: a `World` holds no viewport, no seat,
and no `NetRole` — it does not know it is presented or replicated. Presentation points *inward* by
handle (a viewport names its world; `ManagedViewportSet` asks `WorldRunner::ResolveCameraView`, a
pure query), and the runner holds no pointer back. The **minimal game writes no lifecycle or
per-frame code at all** — the bootstrap auto-bind (world #0 → managed viewport #0) is the zero-code
path. `World` unset leaves the app to load and drive its own scene (the editor, or a game wanting
full control), and the runner is device-free when given no asset manager (it drives empty-scene
worlds without a GPU).

**Every scene the runner holds owns a presentation scope, and device work runs once per frame in one
place.** `Application` owns a **`PresentationScopes`** registry (`Veng/Scene/PresentationScope.h`,
`GetPresentationScopes()`) beside its audio device and haptics engine, declared before the runner so it
outlives every scene the runner destroys, and hands it to the runner (`WorldRunnerInfo::Presentation`,
required). The runner installs a fresh scope on every scene it holds — at `OpenWorld`, and on the
replacement scene of `InstallScene` — and the scene owns it (see
[src/Scene/CLAUDE.md](src/Scene/CLAUDE.md), "Presentation scopes"). `Application::Frame` runs one
**presentation step** after `OnUpdate`: each held scene's scope is stamped with the presentation rank
of the first registered viewport presenting it, `PresentationScopes::Resolve()` latches every scope's
state from the leases the worlds' View phases renewed this frame (and each rank), then the device
engines run their once-per-frame updates (the audio engine's `Update` — every voice judged by its
scope, spatialized against its scope's listener, the scopes' music requests arbitrated and the music
crossfade advanced — and the haptics engine's rumble mix),
after the states are latched and after every
system and `OnUpdate` has started what it will this frame, once per frame rather than once per world.
The registry's **application scope** (`GetApplicationScope()`) is the one
sanctioned owner of what plays outside any scene — a debug panel's test, an editor audition — and is
always `Live`.

**Every `SystemContext` a world receives is built by one factory.** `Application` installs it on its
runner at initialization (`WorldRunner::SetContextFactory`), and the runner builds through it at every
lifecycle point it drives — a world's start (`OpenWorld`, and `StartWorld` for a world opened
unstarted: a client join target, an overlay), each Sim step and View pass, and every stop (below)
— so every context names its world, carries every service, and stamps the world's own
role. A caller stepping a world outside those points builds through `WorldRunner::BuildContext` with a
`SystemContextRequest` (world, scene, phase, tick, alpha, step edges): the reconciliation replay does,
through `ReplaySimStep`. **Every context names a world** — `BuildContext` asserts a valid id, and that
the request's scene carries a presentation scope (a scene the runner does not hold presents nothing and
gets no context) — since every simulation the engine drives, the editor's Play included, is a runner world (below); there is
no separate drive with input hooks of its own to keep in step. A runner with no factory (a device-free one) drops a started world at `CloseWorld`
(or a started scene at `InstallScene`) without running `OnStop`, and asserts on any start, stop or
tick.

**The runner stops every scene it lets go of.** `CloseWorld` stops a started world before dropping
it; **`InstallScene`** stops a started scene it replaces before retiring and dropping it (the world
stays open); **`StopWorld(id)`** stops a started world and leaves it open and unticked until a later
`InstallScene`, `StartWorld` or close — the runtime connect uses it, stopping a running standalone
managed world under the role its systems ran under before retargeting it as the `Client` join target,
so the world holds still from the connect to the join reply; and **`CloseAllWorlds()`** closes every
world in descending id order, so an overlay stops before the world it covers. Stopping is idempotent
— a second stop, or a close after a stop, runs no second `OnStop` — and `StopWorld` / `CloseAllWorlds`
are not legal inside `Tick`. `~WorldRunner` still drops its worlds without `OnStop` (a destructor has
no context); an owner with a factory calls `CloseAllWorlds` first, as `Application::Run` does.

**A closed world drops its input focus.** The runner's **world-closed hook**
(`SetWorldClosedHook`, fired once per closed world after it is erased, in close order — not by
`InstallScene`, which closes no world) is where `Application` calls `InputRouter::ForgetWorld`,
forgets its request-driven focus tokens for that world, and drops the world's request-driven pause
and its request policy. `ForgetWorld` drops every focus stack and
viewport association whose seat names the world and **retires** each dropped entry's token: a retired
token is not live, and its holder's `PopFocus` is a silent no-op that forgets it, so a holder
outliving the world (a `SeatFocusScope`, an editor capture) keeps its pop-exactly-once discipline,
while a never-issued or double-popped token still asserts. A held cursor capture on the world's seat
releases; where the cursor seat goes next is presentation's call.

**A world can be opened over a scene its caller built.** `WorldRunner::OpenWorld(info, Unique<Scene>)`
adopts a ready scene as the world's own (`info.Source` empty): an engaged `info.Systems` builds and
attaches its `SceneSimulation`, replacing any the scene carried, and the load hook and the start run
as for the other overloads. That is how the editor's Play runs — the document's scene is cloned,
seeded, and opened as a world — so Play gets every runner behaviour (the change tick, `World`,
haptics, the request drain, role resolution, `LocalControl`, edge resets while paused,
`OnStop` on every exit) by being one, rather than by copying each across. A caller wanting every
registered system enumerates `SystemRegistry::Entries()`.

**A system may open and close worlds from its own tick.** `WorldRunner::Tick` walks the worlds it
holds, so a system deciding mid-update that a world must go — reaping a finished match, reloading a
level, tearing down the session it came from — calls `CloseWorld` directly rather than publishing a
marker for the application to drive back through. A close issued while `IsTicking()` is **deferred**:
the world is queued, takes no further Sim or View phase that frame, and is stopped (`OnStop`) and
dropped once the walk finishes, in issue order — it still resolves until then, so the caller's scene
reference stays live for the rest of its call, and closing one world twice within a tick closes it
once. An `OpenWorld` is **immediate** — the load hook and the start run nested, so the caller holds a
live world when it returns — but the new world first ticks the next frame. Neither is legal from the
viewport's *render* walk, where a `GuiDriver` runs and `IsTicking()` is false: a driver wanting a
world opened or closed stamps a request component a system acts on from its tick.

**A closing world's scene leaves every viewport before it is destroyed.** A viewport retains the
scene it last presented until its next view push, and the push runs after the tick, so a world
closed at the top of a frame (a departure, a reap, a drained request) would otherwise leave
`GetPresentedScene` dangling for the frame-top pointer routing. The runner's scene-retiring hook
(`SetSceneRetiringHook`, fired by a close and by `InstallScene`'s replacement, each after the
scene's `OnStop`) lets the
`Application` call `Viewport::ReleasePresentedScene` on every registered viewport first.

The world drive is an accumulator: each world's Sim phase steps at its own fixed `SimTickRate`
(`GameWorldInfo`, default 60 Hz) with a monotonic tick, its View phase runs once per frame, and the
render gather blends transforms between the last two ticks. **A frame's steps are bounded twice**:
by `MaxTicksPerFrame` (default 5) and, when set, by `MaxSimMillisecondsPerFrame`, a wall-clock
budget (both on `WorldOpenInfo` and forwarded from `GameWorldInfo`). The backlog either bound leaves
is dropped, not chased, so a world whose step costs more than the time it simulates **dilates
time at a bounded frame cost** instead of paying the cap's worth of steps every frame; the dropped
time is the `WorldRunner/DroppedMs` profiler counter. `SimClock::Run` decides each step's
`SimStepInfo` before it runs — first, last, and whether its pose is one of the frame's final two —
and the runner snapshots transform history only after those two, the only ones interpolation
reads. A driven frame clock disables the budget, so a driven run's step count depends only on its
frame deltas — see
[src/Net/CLAUDE.md](src/Net/CLAUDE.md) for the tick model and the `ApplicationInfo::Net` wiring
(`--server` / `--dedicated` / `--join` / `--netsim`, `PumpNet`, and the runtime `StartHosting()` /
`Connect()` / `StopNet()` operations that mount the same hosts after boot).

**Pause is a refcount, not a boolean, and it lives on the simulation.** A world's whole pause is held
by its scene's `SceneSimulation`: a refcount (`AcquirePause` / `ReleasePause`) beside an explicit
toggle (`SetPaused`), paused while either holds. `WorldRunner::PauseScope(id)` is an RAII pause held
for a scope's lifetime, composing with the explicit `SetWorldPaused(id, …)` toggle so stacked
overlays and a game pause do not clobber each other; both forward to the world's live simulation, a
scope releases onto whatever simulation the world holds when it drops (`InstallScene` carries the
pause onto a replacement scene), and a scope outliving its world, or one taken on a world with no
simulation, releases nothing. **A paused world runs no phase** — neither its Sim steps nor its View
pass — and clears its accumulator and frame edges. Because the pause is on the simulation, anything
holding only the scene asks **`Scene::IsSimulationPaused()`** (a system's `OnStart`/`OnStop`, a Gui
driver, presentation code); `SystemContext` carries no pause flag, since no phase that receives one
runs while paused. **Gameplay requests a pause through the builtin `PauseRequest`** (below), the
engine holding one request-driven pause per world.

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
**per `JoinId`**. The wire break **fails loudly**: `Net::ProtocolVersion` is **9** and the
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
`TravelRequest`, `HostRequest`, `ConnectRequest`, `StopNetRequest`, `ExitRequest`, `FocusRequest`
and `PauseRequest`. A system stamps one onto any world's scene; `Application::Frame` **drains** them
at its frame-safe point (right after the deferred managed-viewport reconfigure, before the world
tick), in the fixed order **StopNet → Host → Connect → Travel → Focus → Pause → Exit** over a
snapshot of the open worlds.
None is `VE_REPLICATED` — a request never rides a snapshot, and on a `Client`-tier world it lowers
to the client-side meaning. Consumption is uniform: a handled request is **removed** (absence is the
ack), an unhandleable one is left **Pending** to retry, and a failed one is marked
`RequestStatus::Failed` with an `Error` and held exactly one frame so the stamper can read the
outcome before re-stamping. **What a world's requests reach is its request policy**
(`Application::SetWorldRequestPolicy(world, WorldRequestPolicy { Mode, OnExit })`, held by world id
and dropped by the world-closed hook; `Veng/Scene/Requests.h`). The two knobs are independent:
`OnExit`, when set, takes the world's `ExitRequest` in place of `RequestExit` in either mode, so a
world that is one part of the application ends itself; `Mode = Sandboxed` fails `TravelRequest`,
`HostRequest`, `ConnectRequest` (and an `ExitRequest` with no `OnExit`) with "not available in a
sandboxed world" and makes `StopNetRequest` a handled no-op, while `FocusRequest` and `PauseRequest`
drain as anywhere. A world with no policy drains as `Full`. The editor's Play is the sandboxed case,
its `OnExit` stopping Play. The wrap is `ApplyRequestPolicies` beside `DrainRequests`
(`src/Scene/RequestDrain.h`), device-free-tested. **`Application::Travel(TravelInfo)`** is the one travel primitive the
`TravelRequest` drain lowers onto — resolving standalone (directory get-or-place → present-on-ready
rebind → pin/unpin), client (travel-request → server-directed travel), or listen-host — and
`FocusRequest` drives the `InputRouter`'s coarse gameplay/UI focus for a seat through an
engine-owned per-seat token (so a stateless system can capture or release focus), dropped when the
seat's world closes. `PauseRequest` pauses or resumes **the world it is stamped in** through one
engine-held `WorldPauseScope` per world (`Paused = true` acquires it when none is held, `false`
releases it, a repeat is a no-op success), dropped when the world closes; it is one more holder of
the refcount, so it never releases an overlay's pause or the explicit toggle, and it fails on a
`Client`-tier world, whose time is the server's. A paused world runs no system, so its resume is
stamped from outside it — a Gui driver presenting it, a system in another world, the application.
See
[src/Scene/CLAUDE.md](src/Scene/CLAUDE.md) for the request idiom and
[src/Net/CLAUDE.md](src/Net/CLAUDE.md) for `Travel`.

**A second level is opened over the running one by a `LevelOverlay` component.** `LevelOverlay`
(`Veng/LevelOverlay.h`; local-only, never replicated) on an entity asks the engine to open its
`Source` level as a world of its own — its own scene, systems and HUD, ticked by the runner like any
world — and present it over the entity's world. `Application` keeps the open overlays (the internal
`OverlayWorld`s, `src/OverlayWorld.h`, keyed by opener world and entity) and **reconciles them once
per frame, right after the request drain and before the tick**: it closes the ones whose request
went (newest first, so one frame's closes unwind in reverse open order), then opens each new request
whose level is resident (starting the load and retrying otherwise, or blocking on it and the spawn
for `WaitForResidency`). An open runs the **overlay policy**: open the world unstarted, copy the
`Seed` entity's reflected components into one new overlay entity (references cleared), fire
**`Application::OnOverlayLoaded(opener, entity, overlay, scene)`** for state with no reflected form,
register a `Presented` viewport placed by `Layout` and re-fit on resize, bind it to the overlay world
through its seat (`RegisterBoundViewport`, so that seat's camera is pulled and its pawn marked
`LocalControl`) and to that seat for role dispatch (`Viewport::SetSeat`), hand the cursor seat to the
overlay's seat and suspend `SuspendSeat` (or the prior cursor seat) beneath a `SeatFocusScope`, hold
a pause on the opener's world for `PauseOpener`, disable every viewport presenting the opener's world
for `Opaque`, give the overlay world a request policy whose `OnExit` closes it, publish
**`LevelOverlayState`** (`World`, `Seat`) beside the request, and start it. `OnWorldLoaded` does not
fire for an overlay world. **An overlay never outlives what it covers:** it closes when the request
is removed or its entity destroyed, when its opener's world closes or has its scene replaced (the
world-closed hook closes it that frame), or when the overlay world closes — by its own `ExitRequest`
or anything else — in which case the opener's `LevelOverlay` is removed too, so it does not reopen.
Closing unwinds the policy in reverse, restoring the cursor seat; overlays **stack** (a dialog over a
modal), and one closed out of order hands its cursor-seat restore and its suspension to the overlay
above it. The fields are read once, at open: an edit to a live request does nothing until it is
removed and re-added. A world under a `Sandboxed` request policy opens none. Systems reach an open
overlay through `LevelOverlayState` and the overlay's own scene; presentation code reaches its
viewport through `Application::FindOverlayViewport(world)`. **Input focus and simulation pause are
separate knobs:** the overlay always suspends the input beneath it, but its opener simulates unless
`PauseOpener` holds it.

**The engine render phase** runs between `BeginFrame()` and `EndFrame()`, uniform for every app
and not overridable: render every registered viewport in registration order (each does its own
`Execute` + `PrepareForAccess(Sample)`), so every viewport output is in `Sample` layout before
`OnRender` builds the ImGui draw data that may sample it. `OnRender` builds the ImGui frame and
records extra draws — it does not run the composite. When ImGui is on, the frame then records the
overlay and runs the **managed tail**: the `GatherPass` assembles the registered `Presented`
viewports into one full-window assembly target and `SwapChainCompositePass` composites it behind
the ImGui overlay. The gather runs only when placements need assembling — one viewport covering the
window is sampled by the composite directly, and none composites a black stand-in — and an ImGui
frame that draws nothing records no overlay pass. An app with nothing to draw runs no ImGui frame at
all: `IsImGuiFrameWanted()` (default true) is consulted before the ImGui frame would begin, and a
false answer skips NewFrame and Render outright (`ImGuiLayer::SkipFrame`), so `OnRender` checks
`ImGuiLayer::IsFrameOpen()` before building UI. The managed tail's gather + composite graphs
re-`Compile()` on swapchain resize, and the composite re-targets the swapchain
(`SetSwapChainTarget`) on a format change.

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
  load**. The ABI is at **version 88** (`VENG_MODULE_ABI_VERSION`, `Veng/Module/Module.h` — the
  header is authoritative, and its prose records why each version moved). The host struct is `{ ApplicationRegistry& App; TypeRegistry& Types;
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
  bar beside the cook), a module built against ABI 11 reading it past the end. **Version 13** is the
  first bump the runtime *simulation* surface drove: `SystemContext` — the per-tick services struct
  the host builds and hands to every module-registered `SceneSystem::OnUpdate` — grew `World` (the
  runner handle of the ticking world), inserted mid-struct, so a module built against ABI 12 reads
  the fields after it at shifted offsets each tick. So the gameplay simulation layer adds **no**
  ABI surface *by registration* — game modes are systems + components, the system catalog rides a
  per-system trait the way a component's `TypeId` does, and a `Level` is an asset — but the struct
  the host passes into a system each tick is itself boundary-crossing, and growing it bumps the ABI
  exactly as growing `AssetEditorContext` does. **Version 24** is the first bump `Application`
  itself drove: `OnWorldPresented` and `OnWorldPresentAbandoned` are new virtuals declared beside
  `OnWorldArrival`, and a module subclasses `Application`, so a module built against ABI 23 carries
  a vtable short of the slots the host dispatches through — the same class of hazard as a short
  struct, on the class a module derives from rather than one it is handed. It also adds
  `GuiOverlay::DrawsCursor`, a field on a component the host reflects and a module's prefabs spawn.
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
  `StartupLevel` `AssetId`, a `DefaultUiContext` `AssetId` (the input map whose role actions
  every Gui document navigates from — see [src/Gui/CLAUDE.md](src/Gui/CLAUDE.md)), and a
  `vector<path> EditorPacks` — packs only the editor mounts (authoring aids a shipped game has no
  use for). `Packs`, `EditorPacks`, `StartupLevel` and `DefaultUiContext` are persisted by hand
  through the `"packs"`/`"editorPacks"`/`"startupLevel"`/`"defaultUiContext"` keys, kept off the
  reflected field list; the cook writes the startup level, the default UI context and the game
  packs' mount names into the cooked project file (`.vengproj`), not the pack header, and leaves
  the editor packs out of it. A managed game sets the default UI context from it before
  `OnInitialize`, and the editor host from `project.veng`, so Play navigates as the game does.
  A `ProjectPreviewSettings Preview` (the `"preview"` object: a `level` whose render block the
  editor's previews render under, a `fovY`, an opening `environment`) is editor-only too.
- **`BuildConfiguration`** (`Veng/Project/BuildConfiguration.h`) — a named ship target: a
  `RoleToFormat` codec table (a fixed record, one `CompressionFormat` field per role — the role
  set is closed), a zstd `CompressionLevel`, a `Target` label, and an `OutputSuffix` (the single
  source of truth for the per-config pack name).
- **`CompressionRole`** (Color / Normal / Mask / HDR / UI / Packed) is a texture's **intent**, the stable
  authoring surface; **`CompressionFormat`** is the closed set of codec outputs a role table may
  name (uncompressed unorm/sRGB, BC7, ASTC 4×4, the HDR float). Both are
  `VE_LEAF(FieldClass::Enum)` so the editor draws a combo, serialized **by name** (never ordinal)
  through shared `ToString`/`Parse` tables. `CompressionFormat` is deliberately *not*
  `Renderer::Format` (which carries depth/swapchain/index formats nonsensical as a texture
  codec); a free `ToRendererFormat()` switch lowers it to the engine format at cook time. A
  configuration may map a role to a **channel-specialized** codec — the desktop configurations
  map `Normal`→BC5 (two channels) and `Mask`→BC4 (red only) — so a texture carrying several
  independent channels declares `Packed`, which every configuration maps full-channel.

The cooker's `ParseBuildConfiguration`/`ParseProject` (`Cooker.cpp`) and the editor's
`ProjectSettingsPanel` hand-parse the authoring JSON into these structs — the one reflected model
in the tree not bound through the shared JSON walker. The cook resolution and CMake host-default
selection are in [cooker/CLAUDE.md](../cooker/CLAUDE.md); the editor surface + host-capability
preview gate in [editor/CLAUDE.md](../editor/CLAUDE.md).

## Steering

`Veng/Math/Steering.h` is the pure, device-free arithmetic an autonomous mover's controller composes
into a desired velocity and a per-tick facing command. It knows nothing of components or the scene,
allocates nothing, and every function is a pure function of its inputs; the frame conventions it
shares (forward -Z, up +Y, right +X; all vectors in one caller-chosen frame) are stated once at the
top of the header.

- **Toward a goal:** `ArriveSpeed` (the capped braking curve), `Arrive`, `Seek`, and
  `ApproachMovingPoint` (arrive in a moving target's frame).
- **Facing:** `AngleBetween`, `ShortestArc`, and `FacingRates` (per-axis yaw/pitch/roll for this
  tick, rate-capped).
- **Keeping clear:** `ClosestApproach` (when and how near two constant-velocity points pass), and
  `AvoidObstacles`, which predicts each `SteeringObstacle` (a moving sphere) over a horizon and
  returns the velocity nearest the desired one that keeps the combined radius, searching a fixed,
  bounded ladder of cones and speeds that starts on a preferred `AvoidSide`. It returns the desired
  velocity bit-exactly when nothing conflicts. Finding the obstacles is the caller's, through the
  physics queries; only spheres are avoided.

The contracts, the candidate ladder, and its work bound are in the header's doc comments.
