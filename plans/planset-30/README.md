# planset-30 — event-routed input with a focus stack

**Phase goal:** give input a single, explicit **owner** per frame. Before this, the engine
`Input` was a flat global poller every consumer read at once, and ImGui ingested all GLFW
events through its own chained callbacks — so while a game ran in the editor viewport,
keystrokes and clicks reached the play simulation, the editor camera, *and* the ImGui panels
simultaneously. Capturing the mouse for Play hid the cursor but did not make input exclusive.

This planset takes up the first half of
[future area 4 (event & input)](../future/README.md#4-event--input-systems): the **event-routed
input core**. The `Window` becomes the single event source (a typed `Event` queue), an
**`InputRouter`** routes each event to consumers by a **focus stack**, and `Input` becomes an
event-fed snapshot rather than a poller. ImGui is **one routed consumer**: the engine owns the
GLFW callbacks (`install_callbacks=false`) and forwards to the ImGui backend only under UI
focus. Pushing **gameplay focus** (the editor's Play, the shipped sample) makes the running game
the exclusive input owner and captures the cursor; **Shift+Esc** (or window-focus loss) releases
it. **Multi-seat input routing and the networking layer remain** the named next increments behind
this seam.

## The shape

```
GLFW callbacks ──► Window event queue ──► InputRouter (focus stack) ──┬─► ImGui sink (forward when UI-focused)
   (engine-owned)     (per-frame)           drain + dispatch          └─► Input snapshot sink
```

- **Window = event source.** All GLFW input + window callbacks enqueue a typed `Event`; the app
  drains the queue each frame through the router. `EventType`/`Event` gain
  key/char/mouse-button/move/scroll/enter + window-focus variants
  ([`Event.h`](../../engine/include/Veng/Event.h),
  [`InputEvents.h`](../../engine/include/Veng/InputEvents.h),
  [`WindowEvents.h`](../../engine/include/Veng/WindowEvents.h)).
- **`InputRouter` = the owner seam.** A focus stack (`InputFocus::UI` / `Gameplay`) decides
  routing: under UI focus an input event is forwarded to ImGui *and* folded into the `Input`
  snapshot; under gameplay focus it is folded into the snapshot only and **swallowed** from ImGui.
  Gameplay focus captures the OS cursor; the Shift+Esc release chord and window-focus loss pop it
  ([`InputRouter.h`](../../engine/include/Veng/InputRouter.h)).
- **`Input` = event-fed snapshot.** Same query API; `BeginFrame` rolls the snapshot and the router
  applies routed events via `ApplyEvent`. No window / no routed events ⇒ neutral all-zeros, so the
  headless contract every `SystemContext.Input` reader relies on is preserved
  ([`Input.h`](../../engine/include/Veng/Input.h)).
- **ImGui = a routed consumer.** Backend initialized with callbacks off; the router forwards each
  event through the backend's chain-callbacks under UI focus, so ImGui never sees input the game
  swallowed and keymap/mods/char handling stay the backend's job
  ([`ImGuiLayer.cpp`](../../engine/src/ImGui/ImGuiLayer.cpp)).
- **Editor + sample.** The prefab/level document pushes gameplay focus on Play/Resume and pops it
  on Stop/Pause; the viewport shows an accent border + a "Shift+Esc to release" banner while
  captured, skips the editor camera, and re-grabs on a click. The shipped sample pushes gameplay
  focus on start.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Event vocabulary | `EventType` + `Event` subclasses for key (press/release/typed), mouse (button/move/scroll/enter), and window focus, carrying engine `Key`/`MouseButton` vocab plus GLFW-native scancode/mods for the ImGui sink. Reuses the existing `EVENT` macro + `EventDispatcher`. | done |
| 01 | Window event source | Window installs all GLFW input callbacks beside the existing ones, enqueues typed events into a per-frame queue, and drains them via `DrainEvents`. The unused `WindowInfo::EventCallback` is removed. | done |
| 02 | InputRouter + Input refactor | New `InputRouter` (focus stack, cursor capture on gameplay focus, Shift+Esc release chord, window-focus-loss pop). `Input` refactored to `BeginFrame` + `ApplyEvent`, dropping the direct GLFW polling. | done |
| 03 | ImGui as a routed consumer | `ImGui_ImplGlfw_InitForVulkan(handle, false)` + `ImGuiLayer::ForwardEvent` translating an `Event` to the backend chain-callbacks, called by the router under UI focus. | done |
| 04 | Application wiring + frame order | `Application` owns the router (`GetInputRouter`), drains+dispatches events before `ImGuiLayer::BeginFrame` so ImGui's `NewFrame` consumes them. | done |
| 05 | Editor + sample wiring | `PrefabEditorPanel`/`LevelEditorPanel` push/pop gameplay focus around Play; the viewport border/banner + camera-skip + re-grab read the router; the shipped sample pushes gameplay focus on start. | done |
| 06 | Tests, verification, roadmap | `InputRouter` unit suite (focus, snapshot routing, release chord, focus-loss pop); full `ctest` + `smoke_golden` + `validation_gate` green; this record + `future/README.md` area 4 status. | done |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## What remains (future area 4)

**Multi-seat input routing** (split-screen, AI-vs-player — fanning one or more devices into the
right `PlayerInput` per `Viewer`, building directly on this router) and the **networking layer**
(serialize/predict/rollback `Intent`, replicate by `Authority`, derive View locally) stay the
named next increments, now unblocked by the delivered event-routing seam.
