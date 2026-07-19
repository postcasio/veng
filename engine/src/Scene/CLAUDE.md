# Scene & gameplay — ECS, input control flow, systems, levels

`Scene` is the runtime ECS world, and the gameplay layer is built from its primitives — components
marking entities plus systems acting on them. **Simulation logic is components + systems**, never
controller/manager/mode objects; the one narrow exception is **presentation binding**, which may be
a per-instance driver (a `GuiOverlay` names a registered `GuiDriver` the engine instantiates with its
document — see [../Gui/CLAUDE.md](../Gui/CLAUDE.md)). A driver reads scene state and stamps
request/command and `VE_VIEW_OUTPUT`-tagged components; it never advances authoritative simulation or
writes a replicated or Sim-input component. It is shaped for
veng's data-oriented grain and for the networking built on top. Project-wide conventions live in
[the root CLAUDE.md](../../../CLAUDE.md); the runtime overview and the engine-managed world drive
in [engine/CLAUDE.md](../../CLAUDE.md); reflection and the `TypeRegistry` in
[../Reflection/CLAUDE.md](../Reflection/CLAUDE.md); the renderer that consumes scenes in
[../Renderer/CLAUDE.md](../Renderer/CLAUDE.md); networking in [../Net/CLAUDE.md](../Net/CLAUDE.md).
Task-oriented guides:
[writing gameplay systems](../../../docs/guides/writing-gameplay-systems.md) and
[wiring a level](../../../docs/guides/wiring-a-level.md).

## The ECS world

A `Scene` is a runtime **ECS world**: a generational `Entity` handle
(`{ u32 Index; u32 Generation; }`, `Entity::Null` empty) over a `TypeId`-keyed, type-erased
**sparse-set** per-component storage, with templated `Add`/`Remove`/`Get`/`TryGet`/`Has` and the
multi-component queries `View<Ts...>` (range-for, yields `(Entity, Ts&...)`, supports `break`) and
`Each<Ts...>`. A query drives off the smallest participating pool. **Structural changes during
iteration are illegal** — adding/removing components or destroying entities mid-`View`/`Each` is
API misuse; the single-threaded model offers no re-entrancy guard. A stale `Entity` (its slot
recycled, generation bumped) accessed through the API is a fatal `VE_ASSERT`, not silent UB.
`DestroyEntity` is **recursive** — it walks the `Hierarchy` `FirstChild` → `NextSibling` links to
destroy the entity's whole subtree in **O(subtree)**, detaching the destroyed root from any
surviving parent's child list first so no sibling is left dangling. A **component is just a
reflected type a `Scene` pools** — see [../Reflection/CLAUDE.md](../Reflection/CLAUDE.md) for
`TypeId` and registration; pools are made lazily on first `Add` of a type, and there is no
separate component-id space.

`Scene::ForEachComponent(Entity, const function<void(TypeId, void*)>&)` iterates every pool that
holds the entity, calling the visitor with each component's `TypeId` and an erased pointer — the
type-agnostic enumeration the editor inspector walks (templated `Get`/`Has` need the type at
compile time; this does not).

## Hierarchy

The scene hierarchy is an intrusive, sibling-linked **`Hierarchy`** component: a `Parent` up-edge
plus a doubly-linked, ordered child list (`FirstChild`/`PrevSibling`/`NextSibling`). Topology is
mutated **only** through `Scene` operations — `SetParent(child, parent)` (detach from the old
list, append under the new in O(1); `Entity::Null` parent re-parents to root), `Detach(child)`,
and `MoveBefore(child, sibling)` (the editor's drag-reorder / insert-at) — which maintain all four
links as a set and bump the spatial version. `GetParent(entity)` and `ForEachChild(entity, fn)`
(forward, insertion order) are the read side. A cycle (a descendant adopting an ancestor) is API
misuse and a fatal `VE_ASSERT`. Only `Parent` is a reflected, persisted field; the three list
links are derived and rebuilt on prefab spawn, so the serializer and cooker never touch them.

## Spatial version

A `Scene` carries a monotonic **spatial version counter** (`GetSpatialVersion()`): it bumps on any
change to a **spatial pool** (`Transform`/`Hierarchy`/`MeshRenderer`) — a structural
`Add`/`Remove`, a `DestroyEntity` touching one, a **non-`const`** access (the mutable
`Get`/`View`/`Each` path, a potential in-place edit), or a `ForEachComponent` visit (the editor
inspector's erased-`void*` edit path). A **`const`** `View`/`Each` does **not** bump it, so a
read-only consumer iterates without forcing a version move. This is the access-as-write
change-tick a consumer (the `SceneBroadphase`) gates its tree rebuild on: it caches the version it
last built against and rebuilds only when the version moved. One constraint: a `Transform&`
retained across frames and written without re-acquiring it bypasses the bump — write transforms
through the scene accessors each frame, as all engine and sample code does.

## Ownership & the owned simulation

A `Scene` is **`Unique`, single-owner** — nothing holds a `Ref` to it; the app (or the
engine-managed world) owns it and a renderer reads it per frame as a `const Scene&`. The
`TypeRegistry` it was created with (`Scene::Create(TypeRegistry&)`) must outlive it and must
already have every component type registered.

**A `Scene` optionally owns the `SceneSimulation` that drives it.** `Scene::SetSimulation`
attaches one and `GetSimulation` returns it; `StartSimulation` / `TickSimulation` /
`StopSimulation` forward `*this` to the held simulation (no-ops when none). `Level::LoadInto`
builds the level's simulation and attaches it here, so the running scene is a self-contained
bundle. The simulation is optional — the editor's Play mode owns its own `SceneSimulation` driving
a play-clone scene rather than attaching one, and a bare `Scene` (a static render source, a test
world) has none. `Clone()` does **not** copy the simulation.

## Builtin components

The builtins are plain reflected components, pre-registered identically to a game's own: `Name` (a
display label), `Transform` (**local** TRS — `Position`/`Rotation`/`Scale`, never a world matrix),
`Hierarchy` (the intrusive scene-graph link — a `Parent` up-edge plus the ordered child list,
mutated through `SetParent`/`Detach`/`MoveBefore`; `WorldMatrix`/`ComputeWorldMatrices` walk the
`Parent` edge as `parent.world * local`, recomputed on demand with no dirty-flag cache), `Camera`
(the component whose FovY/Near/Far and world transform build a `CameraView`, the value type
carrying the view/projection — Y flipped for Vulkan clip space), `MeshRenderer` (holds the
`AssetHandle<Mesh>` a draw queries — the mesh owns its materials, so a renderer queries
`(Transform, MeshRenderer)` and draws each submesh with its material), `Animator` (plays an
`AssetHandle<Animation>` on a skinned-mesh entity; the `AnimationSystem` writes the result into a
transient `SkinnedPose` the renderer uploads), `Light` (a directional light —
`Direction`/`Color`/`Intensity`; `SceneRenderer::Execute` selects the first `Light` entity into
the `SceneView`, or a zero-intensity default → flat ambient when the scene has none), and
`ViewPose` (a fieldless runtime-only tag marking an entity whose `Transform` is authored per
frame in the View phase — a camera-anchored impostor, a billboard: the render gather resolves
its own local transform live instead of blending the scene's two-tick history, which holds
earlier frames' writes and would render it a frame stale; ancestor levels keep their own
interpolation).

## Bounds & broadphase inputs

A `Scene` reduces to a world-space bound on demand: `SceneBounds(scene)`
(`Veng/Scene/Transforms.h`) unions every resident `(Transform, MeshRenderer)` entity's world-space
mesh bound, reusing `ComputeWorldMatrices`' one amortized pass — recompute-on-demand, no
dirty-flag cache. `GatherMeshes` (`Veng/Scene/Visibility.h`) is the pure one-shot candidate gather
over the same pass (world matrix + world-space `AABB` + resident mesh per entity); the
`SceneBroadphase` caches that gather and builds the BVH from it, re-gathering only when the
scene's spatial version moves (or a still-loading mesh becomes resident). The broadphase is a BVH
with **per-submesh leaves**, the granularity the renderer's GPU-driven hi-Z occlusion culling
operates at. Each `Mesh` carries a local-space `GetBounds()` derived from its canonical vertex
positions at load, and each `SubMesh` a local-space `AABB` folded over its index range. Both build
on `AABB` (`Veng/Math/AABB.h`), the engine's glm-only bounds primitive — a min/max `vec3` pair
with the union/expand/center/extents/corners/transform algebra and an empty sentinel. `Frustum`
(`Veng/Math/Frustum.h`) is its visibility companion — six bounding planes extracted
Gribb-Hartmann from a view-projection matrix (Vulkan ZO clip), with a conservative
`Intersects(Frustum, AABB)` p-vertex test (never a false cull).

## Cameras & seats

**Camera is selected per seat and resolved to a `CameraView`.** A camera is `(Transform, Camera)`
data; a **`Viewer { Entity Camera }`** component is a *seat* (a local player, a render target, the
editor viewport) naming the camera entity it renders through, separating seat from camera. Two
pure helpers beside `MakeCameraView` (`Veng/Scene/Camera.h`) resolve a seat to the view the
renderer consumes: **`ResolveCameraView(const Scene&, Entity viewer, f32 aspect) →
optional<CameraView>`** reads the seat's `Viewer`, looks up its `Camera` entity, and projects
through its `WorldMatrix` (so a camera parented under a rig resolves correctly);
**`ResolvePrimaryCameraView(const Scene&, f32 aspect)`** is the one-seat convenience — first
`Viewer`, else the first bare `(Transform, Camera)` entity. **Aspect is a render-target property,
never a `Camera` field** — the caller passes it (output extent in the runtime, panel extent in the
editor). The renderer consumes only a resolved `CameraView` through `SceneView`, so the runtime's
in-scene camera and the editor's external orbit `EditorCamera` (its own non-ECS camera,
`editor/src/EditorCamera.h`) coexist with no renderer involvement. The prefab editor's Play mode
can preview the scene's authored `Viewer` camera through `ResolvePrimaryCameraView` instead of its
orbit camera.

## Input → actions → PlayerInput → Intent

**Control flows raw input → actions → `PlayerInput` → Intent → Movement.** Raw device state is
bound to **named actions** through cooked, remappable data and resolved into a per-seat snapshot,
which a game-specific control system reads to produce the abstract `Intent` gameplay consumes:

- **The action-mapping layer** (`Veng/Input/`). An **`ActionId`** is a minted `u64` leaf (authored
  like `AssetId`/`TypeId`); an action *exists* by being declared in a context, so there is no
  registry. An **`InputMappingContext`** (`AssetTypes::InputMap`) declares its actions
  (id + name + `ActionKind`) and a `vector<Binding>` (raw `InputSource` → action, with a signed
  scale + axis-component modifier). **`ResolveActions(activeContexts, raw, previous) →
  ActionState`** (`Veng/Input/Actions.h`) is the pure, device-free core — bindings × the active
  context stack × the raw snapshot → each action's value + phase, phase derived by comparing
  against the previous `ActionState`. It is unit-tested with no window, mirroring the
  `DecideBarrier`/`ComputeCascades` pure-core pattern; `RawInput` (`Veng/Input/RawInput.h`) is the
  thin adapter from `Veng::Input` to the resolver's `RawInputView`.
- **`PlayerInput` *is* the resolved `ActionState`.** The per-seat serializable snapshot is a
  game-defined set of `ActionSample { ActionId; vec2 Value; ActionPhase }`, read by name
  (`input.GetValue(Actions::Move)`, `input.WasTriggered(Actions::Jump)`) — not a fixed
  `{Move, Look, Buttons}` struct. It serializes through the reflection serializer's name-keyed
  `FieldClass::Array` encoding (each sample self-describing by its `ActionId`), so it rides the
  ordinary cook/load/replicate path with no bespoke wire format.
- **`InputContextStack`** is a per-seat component holding the ordered active
  `AssetHandle<InputMappingContext>` (highest priority last), authored on the player prefab.
  Gameplay systems push/pop it to switch schemes (enter a vehicle → push the `vehicle` context);
  popping to empty neutralizes the seat's input. It is the fine-grained sibling of the
  `InputRouter`'s coarse focus stack. A system drives that **coarse** stack — capturing or releasing
  a seat's gameplay focus — through the builtin **`FocusRequest`** component (`Veng/Scene/Requests.h`):
  it stamps `FocusRequest{ Focus = Gameplay }` (a `Null` seat means the cursor seat) to capture and
  `{ Focus = UI }` to release, and the engine drain owns a single per-seat focus token behind it,
  reconciling idempotently. This lets a stateless system drive focus — which a `FocusToken` held
  across frames otherwise could not — and the request-driven token composes with, and never pops,
  a token pushed by an overlay suspend or a `SeatFocusScope`. It is a local-only request like its
  siblings; see **The system catalog** and the request family in `Veng/Scene/Requests.h`.
- **`InputMappingSystem`** (`Veng/Scene/InputMappingSystem.h`) is the builtin Sim system that
  resolves each locally-owned seat's `InputContextStack` against the raw snapshot into that seat's
  `PlayerInput`. It is the **sole reader of raw device state**, registered in
  `RegisterBuiltinSystems` ahead of any control system — a level's explicit `systems` order must
  place it before the control system that reads `PlayerInput` (registration order does not reorder
  the list). It iterates `(Viewer, InputContextStack, PlayerInput, SeatInput)` seats, so a world
  with none — the input-free minimal template — resolves nothing, a clean no-op with no guard; in
  headless the neutral snapshot resolves to all-`None`. `IsLocallyOwned` returns true for every
  seat; it is the seam the net layer keys on.
    **A context can be gated on gameplay focus as authored data.** An `InputMapData`
  (`Veng/Asset/InputMappingContext.h`) carries a reflected **`RequiresGameplayFocus`** flag
  (authored `"requiresGameplayFocus"`, tolerant-read so existing cooked maps are unchanged); when
  it is set, `InputMappingSystem` **excludes** that context from the seat's effective active list
  whenever the seat lacks gameplay focus (`SystemContext::GameplayFocused`, stamped from
  `InputRouter::IsGameplayFocused()` and `false` headless). This is **pure evaluation at list
  assembly** — the authored `InputContextStack::Active` is never mutated and the order of the
  remaining contexts is unchanged — so a mouse-look context that should not resolve while a menu
  holds focus declares the gate rather than a system lifting and re-inserting it from a saved
  index. It composes orthogonally with the coarse `FocusRequest`/`SeatFocusScope` focus stack (a
  map screen's authored stack *swap* is deliberate state change; this gate is evaluation).
- **`SeatInput` scopes the raw read *per seat*.** A reflected **`SeatInput`** component
  (`Veng/Scene/Components.h`, `UsesKeyboardMouse` + a `Gamepad` id + `WantsGamepad`) on the
  `Viewer` seat names that seat's devices; `InputMappingSystem` builds each seat a filtered
  **`SeatInputView`** (`Veng/Input/RawInput.h`) and resolves against it, so two seats with
  different assignments produce distinct `PlayerInput`s. The view's arms route two ways: a
  **gamepad** arm reads *only* the seat's assigned pad (by `GamepadId`); a **keyboard** arm reads
  the shared keyboard only when the seat sets `UsesKeyboardMouse`; a **pointer** arm is gated
  *both* by `UsesKeyboardMouse` *and* by owning the cursor's viewport region this frame (position
  reported viewport-local, look-delta left raw and sensitivity-invariant). Region routing is
  **inert while the cursor is captured** — a captured pointer belongs wholly to the single
  `UsesKeyboardMouse` seat (delta-look needs no position); it applies only for a free cursor. The
  `InputRouter` computes the per-frame `PointerRouting` (which seat owns the free pointer,
  hit-testing `WindowToViewport` against each `Presented` viewport's region in association order);
  `Application` threads it onto the `SystemContext`. A **`DeviceAssignmentSystem`** (a Sim system
  registered before `InputMappingSystem`) reconciles each seat's `Gamepad` against
  `Veng::Input::ConnectedGamepads`: a connected-but-unheld pad is auto-assigned to the first
  `WantsGamepad` seat with a `None` slot, a disconnected slot is cleared, a level-authored slot is
  respected. **A seat with no `SeatInput` is skipped** — its `PlayerInput` is synthesized or
  replicated (the AI/remote path) — so every local human seat must author one.

A game-specific **control** system reads `PlayerInput` by action id and writes the abstract
**`Intent`** command (local-frame move, look delta, action bitset); a **movement** system
(`MovementSystem`, `Veng/Scene/Movement.h`) and gameplay systems generally consume `Intent` and
mutate state, scaled per pawn by an optional **`Mover`**. **The layering invariant:** actions →
`PlayerInput` → control system → `Intent` → gameplay; **only** the control system reads actions,
and gameplay reads `Intent`. `Intent` is the serializable chokepoint the net layer predicts and
rolls back and the uniform interface AI and remote players write through — both are drop-in
`Intent` producers that never touch an action or a context, no movement change. (The net layer
replicates `PlayerInput` — the action snapshot — for a human seat and re-derives its `Intent`
server-side; AI and server-authoritative producers write `Intent` directly.) `Veng::Input`
(`Veng/Input.h`) carries a gamepad device surface backing the gamepad `InputSource` arms — pads
tracked by `GamepadId` (a stable GLFW joystick slot 0..15), polled once per frame into the same
event-fed snapshot as keyboard/mouse, queried through `IsGamepadButtonDown` / `GetGamepadAxis` /
`ConnectedGamepads`, with connect/disconnect surfaced as events. A `SeatInputView`'s gamepad arm
reads the seat's assigned pad through it. A **`Possesses { Entity Pawn }`** link names the pawn a
seat controls; possession is independent of `Viewer.Camera` (a spectator views without possessing;
a cutscene retargets the camera without un-possessing).

## ConstantMotion — the input-free counterpart

**`ConstantMotion` is the input-free counterpart** (`Veng/Scene/Motion.h`): an authored
**`ConstantMotion { vec3 LinearVelocity; vec3 AngularVelocity; MotionSpace Space }`** is a
constant rate of change of the `Transform` — a drift and/or spin — that the engine
**`ConstantMotionSystem`** integrates each Sim tick. `AngularVelocity` is an axis-angle vector
(direction is the spin axis, magnitude is radians/sec); `Space` applies both velocities in the
entity's `Local` frame or its parent `World` frame. Unlike `MovementSystem` it reads no `Intent` —
the motion is autonomous, authored data, not a command — so a spinning prop carries no controller
and rides no wire. It is a builtin component (`RegisterBuiltinTypes`) selected per level like any
other system; the minimal template uses it to spin its cube as data, naming the host-registered
builtin `ConstantMotionSystem` in its level — its module registers no system of its own.

## Simulation & the Sim/View split

**The tick is split Sim / View, and entities carry `Authority`.** A `SceneSystem`
(`Veng/Scene/SceneSystem.h`) declares a **`Phase { Sim, View }`** (default `Sim`); a
**`SceneSimulation`** (`Veng/Scene/SceneSimulation.h`) runs all Sim systems, then all View
systems, each tick — so a View system reads the state the Sim phase finalized this tick. **Sim**
is the deterministic, replicable simulation (control, movement, rule systems); a headless Sim tick
is a pure function of state + intents (the `SystemContext.Input` service is always present,
reporting the neutral all-zeros state in headless, so an input-reading system needs no guard).
**View** is client-local presentation derived from finalized Sim state — the `CameraRigSystem`
(`Veng/Scene/CameraRig.h`) trails a possessed pawn via a `CameraFollow` component and resolves a
first-person `CameraLook` (a clamped yaw/pitch heading written as the entity's rotation), never
authoritative and never on the wire. An **`Authority { Tier, Owner }`** component marks who
simulates an entity: authored entities default `Server`, client-local view entities are `Local`
(only those two tiers are authored or persisted; `Remote` and `Predicted` are each peer's runtime
stance toward an entity, never replicated). Its consumer is the net layer's authority filter —
**`HasAuthority(context, scene, entity)`**, a `SystemContext` role × `Authority::Tier` query the
builtin authoritative Sim advancers (`MovementSystem`, the motion systems, `RootMotionDriveSystem`)
consult before touching an entity; an entity with no `Authority` defaults to `Server`-tier (see
[../Net/CLAUDE.md](../Net/CLAUDE.md)). The two-pass split is the whole scheduling mechanism: no
dependency graph, no parallelism.

## Game modes & world config

**A game mode is mode-state components + rule systems — no object, no registry.** A
**`GameModeConfig`** on the level's settings entity names the player prefab (its JSON key is
`"gameMode"`); a game authors whatever further mode-state components its own rule systems read
and write, beside it. The "mode" is a *selectable set of rule systems* (spawn-on-start, scoring,
win-condition) over those components; begin/end-play is the systems' `OnStart`/`OnStop`.
Selecting a mode is choosing a config plus a registered rule set — no C++ path picks it, no
`GameModeRegistry`, no ABI bump. The engine ships no mode-state component of its own — mode state
is game vocabulary. (The word "session" means something else entirely: the per-account
`Net::SessionRecord` the host tier keeps — see [../Net/CLAUDE.md](../Net/CLAUDE.md).)

**World-scoped config is a component found by type, not on a designated entity.** A rule system
reads the `GameModeConfig` (and the engine reads `LevelRenderSettings`) through
**`Scene::TryGetFirst<T>()`** — the first component of a type, or `nullptr`. So world/level config
lives on *some* settings entity without any consumer naming a well-known one: a `Level` seeds
level-scoped config onto one (see **Levels**), and genuinely world-scoped config (a hypothetical
`PhysicsSettings` holding gravity, say) is just authored as a component on an entity in the world
prefab. One such component is the expected case; with several the first wins and the rest are
ignored — a loose convention, deliberately **not** an enforced singleton or a side-channel
resource store (the data stays ordinary component data, riding the one
cook/serialize/inspector/replication pipeline). Absent → the consumer falls back to a default, the
same schema-tolerance a missing input gets.

## The system catalog

**Systems are a catalog; selection and order are level data; config is components.** A
`SceneSystem` declares a stable **`SystemId`** (a `u64` id space alongside `AssetId`/`TypeId`,
minted with `vengc generate-id`) + a display name through the **`VE_SYSTEM(Type, 0x…ULL,
"Name")`** trait macro — the system analogue of `VE_REFLECT`'s identity. The host-owned
**`SystemRegistry`** (`Veng/Scene/SystemRegistry.h`, mirroring the `TypeRegistry`: the host
constructs it, **pre-registers the engine's reusable systems with `RegisterBuiltinSystems`**
(`Veng/Scene/BuiltinSystems.h` — the system analogue of `RegisterBuiltinTypes`), the module fills
its own through `VengModuleRegister`, `Application` borrows it) stores `{ SystemId, Name,
factory }`, **enumerates the catalog** without instantiating anything, resolves an id, and fatally
rejects a duplicate id. The builtins register in this order (`BuiltinSystems.cpp`):
`DeviceAssignmentSystem`, `InputMappingSystem`, `MovementSystem`, `RootMotionDriveSystem`,
`CameraRigSystem`, `AnimationSystem`, `ConstantMotionSystem`, `RemoteInterpolationSystem`,
`TimeOfDaySystem`. Registration is GPU-free (building a system touches no `Context`/device), so
`RegisterBuiltinSystems` is callable in the headless cooker with no ICD — the cook reflects a
level's named systems against the same builtins + module catalog the runtime resolves. A
`SceneSimulation` is built either from an **ordered `SystemId` set** selecting catalog entries
(run in that order, honoring the phase split) or from the whole registry as the "all registered"
convenience. A system's **parameters are authored as components** — a settings entity the system
reads — reusing the entire reflection inspector and keeping systems pure logic; there is no
reflected-system-config mechanism.

## Levels

**A `Level` is the authored wiring artifact — a thin wrapper by reference.** A **`Level`** asset
(`AssetTypes::Level`, `Veng/Asset/Level.h`) does not embed world entities: it *references* a
**world prefab** by `AssetId` and adds the data that is not reusable-recipe data — the ordered
active `SystemId` set, the `GameModeConfig`, and a tolerant **`LevelRenderSettings`** subset (the
view-wide post/pipeline knobs — exposure, bloom, shadow/AO toggles the app maps onto its
`SceneRendererSettings`/`SceneView`). The sky/environment is **not** a level field: it is the
scene's one author-opt-in `Sky` component (plus an optional `TimeOfDay`) on the world prefab,
resolved by the renderer itself each `Execute`. The level *reuses* prefab serialization rather
than embedding a second copy: a prefab is a reusable recipe, the `Level` is the once-loaded
playable unit (named `Level`, not `Scene`, to avoid colliding with the runtime `Scene`). It is CPU
data with no GPU resource, loaded through the ordinary `AssetManager::Load`/`LoadSync` path; its
world prefab and that prefab's embedded asset refs resolve as ordinary load-time dependencies.
**`Level::LoadInto(AssetManager&, const SystemRegistry&) → LevelInstance`** is what *starts the
game*: it spawns the world prefab into a fresh `Scene`, builds a `SceneSimulation` from the
level's `SystemId` set against the catalog and **attaches it to the `Scene`**
(`Scene::SetSimulation` — the scene owns its simulation), and **`SeedLevel`s a settings entity**
carrying the level's `GameModeConfig` and `LevelRenderSettings` as components, returning a `LevelInstance { Unique<Scene> World; ResidencyBatch Pending; }` the app
ticks (via `Scene::TickSimulation`) and renders. The level's config (game mode, render settings)
stays **authored on the `Level`** (edited as separate level-editor panels, cooked into the level
blob) but enters the running world as scene components — so rule systems and the engine read it by
`Scene::TryGetFirst<T>()`, the engine resolving `LevelRenderSettings` onto the renderer from the
scene rather than the `Level` object. A game is assembled as authored data, not hand-spawned in
`main.cpp`; the engine-managed game world (see **Application** in
[engine/CLAUDE.md](../../CLAUDE.md)) drives this end to end so a minimal `main.cpp` writes none of
it.
