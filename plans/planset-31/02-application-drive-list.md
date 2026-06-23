# Plan 02 — Application drive-list, managed composite, and the frame loop

**Goal:** give `Application` the **central drive-list** of viewports, the **managed gather + composite
tail**, and an engine-owned **render phase** that runs for every app (the sample *and* the editor).
The owner constructs a viewport with the house factory and registers it
(`m_vp = Viewport::Create(info); host.RegisterViewport(*m_vp);`); the engine stores only a non-owning
pointer, and `~Viewport` self-unregisters. An optional engine-owned **managed primary
viewport** is the plug-and-play path for a game; the managed tail gathers every `Presented` viewport
into its region and composites behind ImGui. Then **migrate hello-triangle** onto it — deleting its
`SceneRenderer`, `Sampler`, ImGui texture, `SwapChainCompositePass`, and `CompiledGraph` boilerplate.
The golden must not move.

**Depends on Plan 00 (the `Viewport` type) and Plan 01 (the gather pass + composite tail).**

## Why it is its own plan

This is where the abstraction earns its keep: the frame loop, the registry, and the managed tail are
one coherent change, and the sample migration is the proof. It is also the golden-risk plan — moving
the sample off its hand-wired path while keeping the pinned smoke capture byte-identical is the
verification that the engine-driven path renders exactly what the hand-wired one did. Keeping it
separate from the editor migration (Plan 03) isolates that risk to one consumer.

## What lands

- **The drive-list on `Application`, RAII-owned.**
  - `void RegisterViewport(Renderer::Viewport&)` — stores a **non-owning** `Viewport*` in an ordered
    `vector` and hands the viewport a back-reference to this drive-list. Construction stays the house
    factory: the owner calls `Viewport::Create(info)`, holds the returned `Unique`, and registers it
    — `m_vp = Viewport::Create(info); host.RegisterViewport(*m_vp);`. Registration is purely list
    membership, not construction; double-register is a fatal `VE_ASSERT`.
  - **Registration is explicit; cleanup is automatic.** Dropping the owner's `Unique` runs
    `~Viewport`, which removes its own pointer from the drive-list via the back-reference — no
    explicit unregister call at any call site. The removal is an **order-preserving erase**
    (registration order is render order, below), never a swap-and-pop. `RegisterViewport` must not be
    called from inside the per-frame drive loop (single render thread; destruction happens between
    frames), so the list is never mutated mid-iteration.
  - **Registration order is render order** (Plan 05 leans on this for producer-before-consumer RTT).
  - The list is driven in `Frame` (below). Empty is valid — an editor with no document open, or a
    headless tool, drives zero viewports.

- **The managed primary viewport (opt-in, the game's plug-and-play path).** `ApplicationInfo` gains
  an optional `ManagedViewport` config (extent defaulting to `InternalRenderExtent`,
  `SceneRendererSettings`, `ColorFormat`). When set, `Application` constructs one `Role::Presented`
  viewport via `Viewport::Create`, registers it, and **holds the `Unique` itself** — its region the
  whole window (tracking swapchain resize); it is reachable via `[[nodiscard]] Renderer::Viewport*
  GetPrimaryViewport()` (null when not configured). The app calls `SetViewState` each frame (scene +
  camera + knobs) and never touches a `SceneRenderer`, sampler, texture, or composite. When unset
  (the editor), `Application` owns no primary viewport.

- **The managed gather + composite tail.** When ImGui is present, `Application` owns the gather pass,
  the `SwapChainCompositePass`, and the `CompiledGraph`, builds them at init, and on swapchain
  invalidation **re-targets and rebuilds**: it calls `compositePass.SetSwapChainTarget(
  Context::GetSwapChainFormat(), Context::GetActiveDisplayColorSpace())` **before** recompiling the
  graph — the two-step the sample registers by hand today (a window moving to an HDR display
  re-negotiates the encode/format). The composite's color space is sourced from `Context`; paper-
  white / peak-nits use the engine defaults (they are *display* properties, **not**
  `ManagedViewport` fields). Each frame `Application` gathers the registered **`Presented`**
  viewports into the gather pass's placement list — `{ vp.GetOutput(), vp.GetRegion() }` per viewport
  — runs the gather, then the composite (zero placements ⇒ ImGui over a clear, the editor's case).
  Placements rebind per frame; to avoid per-frame bindless churn, the gather re-registers a placement
  slot only when its output view actually changed (guard on `GetOutput()` identity).

- **An engine-owned render phase in `Frame` — for every app.** `Application::Frame`'s render section
  becomes, between `Context::BeginFrame()` and `Context::EndFrame()`, a fixed engine-driven sequence
  that subclasses do **not** override:
  1. **Render every registered viewport in order** — `vp.Render(cmd)` (each `Execute` + the `Sample`
     barrier). The central drive.
  2. `OnRender()` — the app builds its ImGui frame (`UI::Image(vp.GetOutput())` for any surfaced
     `Offscreen` viewport) and records any extra draws. **Its contract narrows**: it no longer runs
     `Execute`, `ImGuiLayer::Render`, or the composite — those move into the engine phase around it.
     Every existing override (the sample, the editor) is migrated to the narrowed contract here and
     in Plan 03.
  3. When ImGui is on: `ImGuiLayer::Render(cmd)`, then the managed gather + composite to the
     swapchain.

  Viewports render *before* `OnRender` builds the ImGui draw data, so a `UI::Image` referencing a
  viewport output samples a frame already in `Sample` layout. An app pushing its camera in `OnUpdate`
  renders the current frame; an editor panel computing its region/camera in its UI keeps its existing
  one-frame latency — the region (like the camera) is pushed in the UI pass and consumed by the next
  frame's engine render, and the gather reads the same `GetRegion()` the producing `Render` used, so a
  mid-resize frame never places a texture into a rect it was not rendered for.

- **hello-triangle migration.** Drop `m_SceneRenderer`, `m_SceneSampler`, `m_SceneTexture`,
  `m_Composite`, `m_CompositeGraph`, `BuildCompositeGraph`, `CompositeToSwapChain`, the swapchain
  callback, and `ReconfigureScene`'s texture/composite re-fetch. `OnInitialize` configures the
  `ManagedViewport` from the level's render subset. `OnUpdate` ticks the simulation and
  `SetViewState`s the primary viewport (scene + `ResolvePrimaryCameraView` + the tone/bloom
  members). `OnRender` becomes just the debug windows (`RenderUserInterface`'s Scene/Stats), reading
  `GetPrimaryViewport()->GetRenderer()` for the stats and `Configure` for the settings UI; the
  `UI::Image` in the "Scene" window samples `GetPrimaryViewport()->GetOutput()` via an ImGui texture
  the app still creates (re-fetched on `Configure`). `OnDispose` resets only the app's own members;
  the managed viewport/gather/composite are engine-owned and torn down before the context.

## Decisions

1. **Central driving, local ownership, RAII cleanup.** The list holds a non-owning `Viewport*`; the
   caller holds the `Unique` from `Viewport::Create` and registers it. The game's primary viewport is engine-owned
   because the engine offers it as a convenience (the engine holds *its* `Unique`); an editor panel's
   viewport is panel-owned (Plan 03). Either way `~Viewport` removes the engine's pointer, so there is
   no manual unregister contract to violate — dropping the `Unique` is the whole of cleanup. Teardown
   order follows from member destruction: a subclass's panels (and their viewports) are destroyed
   before the base `Application`'s drive-list, so the back-reference is always live in `~Viewport`.

2. **Registration order is render order.** The simplest correct rule for inter-viewport RTT
   (Plan 05): register producers before consumers. Removal preserves order (no swap-and-pop). A
   declared-dependency topo-sort is a named future refinement, not needed now.

3. **The managed primary viewport is opt-in.** A game flips one `ApplicationInfo` field and is
   plug-and-play; the editor leaves it unset and owns its views. The engine never mints a `Scene` —
   the app always pushes the scene through `SetViewState`.

4. **The render phase is engine-owned and uniform; `OnRender` is narrowed, not overridden.** The
   sequence (render viewports → `OnRender` builds ImGui → `ImGuiLayer::Render` → gather + composite)
   lives in `Application::Frame` and runs for *every* app, so the editor stops driving its own present
   path (Plan 3 deletes `EditorHost`'s hand-rolled blit/present graph) and the sample stops recording
   the composite by hand. `OnRender`'s job shrinks to "build the ImGui frame and record extra draws."

5. **The managed tail is engine-managed only when ImGui is on.** Headless smoke has no swapchain and
   no composite — the managed primary viewport renders into its offscreen target and the app reads it
   back (`WriteSceneCapture` reads `GetPrimaryViewport()->GetOutput()->GetImage()`), exactly the
   existing capture path.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Application.h` | `RegisterViewport(Viewport&)`, `GetPrimaryViewport`; the drive-list, managed viewport, managed gather + composite + graph members; `ApplicationInfo::ManagedViewport`. |
| `engine/src/Application.cpp` | Construct the managed viewport/gather/composite at init; the engine render phase in `Frame` (render viewports → `OnRender` → `ImGuiLayer::Render` → gather + composite); gather the `Presented` placements; on swapchain invalidation call `SetSwapChainTarget` then rebuild the graph; teardown order (viewports/gather/composite reset before context). |
| `examples/hello-triangle/main.cpp` | Adopt the managed primary viewport; delete the hand-wired renderer/composite/graph/sampler/texture and their plumbing; `OnUpdate` `SetViewState`; `OnRender` → debug windows only. |

## Verification

- Clean build; `ctest` green across the bands.
- **`smoke_golden` does not move** — the headless capture renders through the managed primary
  viewport (same `SceneRenderer`, same fixed pose) and reads back the same `RGBA16F` output; the
  byte-for-byte golden is the guard that the engine-driven path matches the hand-wired one.
- `hello_triangle_launcher_smoke` exits 0 — the full `dlopen` → `Run()` chain through the managed
  path writes a correct-sized PPM.
- `validation_gate` green under `build-debug` — the managed gather + composite issues the same
  workload the sample issued by hand (one full-window `Presented` placement); no unbound descriptor,
  no new validation surface.
- The windowed app renders + composites identically (manual run): the "Scene" window samples the
  managed viewport, the settings UI drives `Configure`, the Stats read the renderer through
  `GetRenderer()`. Moving the window between an SDR and an HDR display re-targets the composite (the
  `SetSwapChainTarget` path — not covered by the golden, which is fixed-display).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
