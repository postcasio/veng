# Writing gameplay systems

This guide takes you from an empty `SceneSystem` to a system running inside a
level. Gameplay in veng is **not** a hierarchy of actor objects — it is plain
reflected **components** marking entities plus **systems** that act on them.
A player, a camera rig, a game mode: each is a component (the data) and a system
(the logic), never a `PlayerController` / `CameraManager` / `GameMode` class.

The live reference for everything below is the
[hello-triangle](../../examples/hello-triangle/) module — `main.cpp` defines a
control system, a spawn rule, and a spinner, and the engine ships the movement
and camera-rig systems they feed. Open it beside this guide; every snippet here
is taken from real, compiling code.

---

## 1. What a system is

A gameplay system is a subclass of `SceneSystem`
([`engine/include/Veng/Scene/SceneSystem.h`](../../engine/include/Veng/Scene/SceneSystem.h)).
Its surface is three virtuals:

```cpp
class SceneSystem
{
public:
    virtual ~SceneSystem() = default;

    [[nodiscard]] virtual Phase GetPhase() const { return Phase::Sim; }

    virtual void OnStart(Scene& scene, const SystemContext& context) {}
    virtual void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) = 0;
    virtual void OnStop(Scene& scene, const SystemContext& context) {}
};
```

- **`OnStart`** runs once when play begins, before the first `OnUpdate`. Spawn
  entities, seed state, cache lookups here. The default does nothing.
- **`OnUpdate`** runs once per tick with the `delta` (seconds since the previous
  tick). This is the only pure-virtual — every system advances *something*.
- **`OnStop`** runs once when play ends, after the last `OnUpdate`. Tear down what
  `OnStart` built. The default does nothing.

A `SceneSimulation` owns the registered systems and drives this lifecycle:
`Start` → repeated `Update` → `Stop`. The runtime app and the editor's Play mode
tick the *same* systems through the *same* driver, so a system you write behaves
identically in both.

### The SystemContext

Every lifecycle call receives a `SystemContext` — the per-tick services a system
is allowed to reach for:

```cpp
struct SystemContext
{
    AssetManager& Assets;   // load or build resources
    const Input&  Input;    // frame-coherent input, always present
    u64           Tick;     // the fixed sim tick number (the wire's unit of time)
    f32           Alpha;    // interpolation fraction into the next tick; View phase only
    NetRole       Role;     // which peer's authority this tick runs under; Server standalone
    // ... view, pointer, and debug-draw services, elided ...
};
```

`Input` is **always present** — never null. In a headless run (the CI/smoke path,
which has no window) the input service reports the neutral *all-zeros* state:
nothing pressed, zero mouse delta. So a system reads `context.Input`
unconditionally, with no null-guard, and in headless it naturally produces zero
output — a stationary pawn, an idle session. This is deliberate: it makes a
headless tick a clean function of scene state, which is what keeps the smoke
golden reproducible.

You operate on the world through the `Scene&`: the templated `Add`/`Get`/`Has`
accessors and the `View<Ts...>` / `Each<Ts...>` queries that iterate every entity
carrying a given component set. **Write transforms (and any spatial component)
through the scene accessor each tick** — a `Transform&` retained across frames and
mutated bypasses the scene's change tracking and the broadphase will read it as
static.

---

## 2. Choosing a phase — Sim or View

Every system declares the **phase** it runs in, by overriding `GetPhase()`:

```cpp
enum class Phase { Sim, View };
```

Each tick, `SceneSimulation::Update` runs **all `Phase::Sim` systems first, then
all `Phase::View` systems** (each phase in registration order). A View system
therefore reads the state the Sim phase finalized *this* tick.

**The Sim phase steps at a fixed rate; the View phase runs per frame.** The world
drive is an accumulator: Sim systems advance at a fixed `SimTickRate` (a
`GameWorldInfo` knob, default 60 Hz) with a monotonic `u64` tick number — a frame
runs 0, 1, or several Sim ticks depending on the elapsed time, and pressed-edge
input latches until a tick consumes it. View systems then run **once** per frame
with the frame delta and an interpolation `Alpha` (the residual fraction into the
coming tick), which the render gather uses to blend transforms between the last two
ticks so motion stays smooth between fixed steps. `context.Tick` is that tick number
and `context.Alpha` the fraction (see the `SystemContext` above). The fixed tick is
what a client and server agree on — "tick 4812" is the wire's unit of time — so a
Sim system's cadence is framerate-independent and replayable, while a View system
(a camera rig) is free to run at the display rate. Networking builds directly on
this: [Networking](networking.md) covers the full model.

| | `Phase::Sim` (the default) | `Phase::View` |
|---|---|---|
| What it is | Deterministic, replicable game simulation | Client-local view derivation |
| Examples | control, movement, game-mode rules | camera rig, blends, screen shake |
| On the wire | Replicated; authoritative | Never replicated; derived per client |

**Why the line matters.** The Sim/View split is the structural seam a networking
layer and a paused-sim editor both rely on. Replicate the *pawn* (Sim); each
client derives its *own* camera (View). The net layer simulates the Sim
phase authoritatively and lets every client run the View phase locally. The
editor can pause the Sim phase while still running View systems to keep the
camera responsive. Single-threaded today it is one extra partitioned pass over
the system list — and it is agony to introduce after the fact, which is why it is
set now.

### The Sim determinism contract

`Phase::Sim` is where replicable gameplay lives, and that imposes a discipline a
Sim system **must** keep:

> A `Phase::Sim` system reads only **entity state** and **`Intent`** — never
> `context.Input` directly, never wall-clock time, never async asset-load state.

Those inputs make a system non-replayable: re-running it on identical state would
produce a different result, which silently breaks lockstep. The device is read
*once*, at the edge, by the control system that produces `Intent` (section 3) —
everything downstream consumes the `Intent` snapshot, so the same intents over
the same state always yield the same world.

This is a **discipline the phase enables, not a rule the engine enforces.** The
compiler will not stop a Sim system from reading the mouse; the contract is yours
to keep. `Phase::View` carries no such restriction — a camera rig reading the
mouse is fine, because its output is local and never on the wire.

A View system must also be **safe to run (or skip) in the pinned smoke frame**:
the smoke path renders one fixed pose and never ticks `Update`, so a View system
that would move the camera simply does not run there.

### The authority filter

A Sim system that **advances authoritative state** — moves a pawn, integrates a
prop, resolves a game rule — should act only on the entities **this peer owns**.
`HasAuthority(context, scene, entity)` is the one-line query that decides it:

```cpp
scene.Each<Transform, Intent>(
    [&](Entity entity, Transform& transform, Intent& intent)
    {
        if (!HasAuthority(context, scene, entity)) { return; }
        // ... advance the entity's state ...
    });
```

It reads the entity's `Authority` tier against the tick's `NetRole` (on
`SystemContext`): a `Server`-tier entity is simulated only by a `Server`-role peer,
a `Local`-tier entity is always simulated locally (client-local view/UI state), and
a `Remote`-tier entity — the client-side mirror of a server-owned entity — is never
simulated (the interpolation system displays it). An entity with no `Authority`
component defaults to `Server`-tier.

- **Standalone and the server are unchanged.** A standalone or listen/dedicated
  server runs as `Role::Server`, where every `Server`-tier entity passes — so
  single-player behaviour is identical whether or not a system consults the filter.
- **A client stops fighting the snapshot stream.** On `Role::Client` the filter
  skips the `Server`/`Remote`-tier pawns whose truth arrives over the wire, so the
  client's Sim phase never overwrites a snapshot. Its `Local`-tier entities keep
  simulating.
- **AI and server producers are unaffected.** An AI or server-authoritative system
  that writes `Intent` directly still runs and still advances the state it owns —
  the filter gates the *advancing* systems, not the `Intent` producers.

The builtin `MovementSystem` and the motion systems (`ConstantMotionSystem`,
`RootMotionDriveSystem`) consult it; a game's own authoritative Sim systems adopt
the same line.

---

## 3. The Input → Intent → Movement pattern

This is the spine of player control, and the single most important pattern in
this guide. It is three stages, never collapsed into one:

```
device / wire  →  PlayerInput  →  Intent  →  Transform
                  (capture)       (control)   (movement)
```

The components live in
[`engine/include/Veng/Scene/Components.h`](../../engine/include/Veng/Scene/Components.h):

- **`PlayerInput`** — a per-seat snapshot of *this tick's resolved actions*: a set
  of `{ ActionId, vec2 Value, ActionPhase }` samples read by action id
  (`input.GetValue(Actions::Move)`, `input.WasTriggered(Actions::Jump)`). The engine's
  builtin `InputMappingSystem` fills it for a local seat by resolving that seat's
  active bindings against `Veng::Input`; a remote seat's arrives from the wire.
- **`Intent`** — an *abstract, source-agnostic command*: `vec3 Move` (in the
  pawn's local frame), `vec2 Look`, `u32 Actions`. It answers "what does this pawn
  want to do this tick," divorced from who decided it.
- **`Possesses`** — a seat-to-pawn link: `Entity Pawn`, the pawn this seat
  controls.

### Stage 1: the engine resolves raw input into actions

Stage 1 — device → `PlayerInput` — is done for you. Raw device state is bound to
**named actions** through cooked, remappable data (an `InputMappingContext` asset,
active per-seat via an `InputContextStack`), and the builtin **`InputMappingSystem`**
resolves those bindings each tick into the seat's `PlayerInput`. It is the **only**
system that reads raw device state, and it must be ordered **before** your control
system in the level (registration order does not reorder an explicit `systems`
list). Authoring the actions, the `*.inputmap.json`, and the seat's context stack is
the subject of its own guide — **[Authoring input actions](authoring-input-actions.md)**;
here we pick up at the resolved snapshot.

### Stage 2: a control system reads actions, writes intent

hello-triangle's `ControlSystem` (in
[`main.cpp`](../../examples/hello-triangle/main.cpp)) is `Phase::Sim` and maps the
resolved `PlayerInput` onto the possessed pawn's `Intent`. It reads actions **by
name** — never the device — so it consults only the snapshot `InputMappingSystem`
already filled:

```cpp
class ControlSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32, const SystemContext&) override
    {
        scene.Each<PlayerInput, Possesses>(
            [&](Entity, PlayerInput& player, Possesses& possesses)
            {
                // An unwired seat is inert: skip rather than fault.
                if (possesses.Pawn == Entity::Null || !scene.IsAlive(possesses.Pawn) ||
                    !scene.Has<Intent>(possesses.Pawn))
                {
                    return;
                }

                scene.Get<Intent>(possesses.Pawn) = MapInputToIntent(player);
            });
    }
};
```

The `Actions::Move` / `Look` / `Jump` constants are game policy — minted `ActionId`s
the bindings target. The mapping itself is factored into a pure free function so it
is testable without an `Input` or a scene:

```cpp
Intent MapInputToIntent(const PlayerInput& input)
{
    const vec2 move = input.GetValue(Actions::Move);
    const vec2 look = input.GetValue(Actions::Look);

    Intent intent;
    intent.Move = vec3(move.x, 0.0f, -move.y);
    intent.Look = vec2(-look.x, 0.0f);
    intent.Actions = input.IsHeld(Actions::Jump) ? 1u : 0u;
    return intent;
}
```

Note this Sim system reads **no raw device state** — it reads the resolved
`PlayerInput`, the deterministic snapshot. The one device read for the whole seat
happened upstream, in `InputMappingSystem`, at the edge; everything from
`PlayerInput` onward is a pure function of that snapshot. That is exactly the
boundary the determinism contract draws.

### Stage 3: a movement system consumes intent, mutates state

The engine ships the consumer:
[`MovementSystem`](../../engine/include/Veng/Scene/Movement.h), also `Phase::Sim`.
It reads **only** `Intent` and pawn state — never the device — and integrates each
`(Transform, Intent)` pawn's intent into its transform, scaled by the pawn's
`Mover` (or a default when absent):

```cpp
void MovementSystem::OnUpdate(Scene& scene, f32 delta, const SystemContext&);
// per (Transform, Intent): IntegrateMovement(transform, intent, mover, delta)
```

`IntegrateMovement` is pure math (no scene, no device), so it is the deterministic
core both the system and its unit tests drive.

### Why split it three ways

Single-player makes this look like indirection — why not just move the pawn where
the key points? Because the seam pays off the moment you do anything beyond one
local player:

- **AI is a drop-in.** An AI system that writes the same `Intent` component drives
  the same `MovementSystem` with zero changes — AI is "just another `Intent`
  producer." The engine ships that producer: `BehaviorSystem` ticks a behaviour
  tree whose leaves write `Intent` (see [Writing AI behaviours](writing-ai-behaviors.md)).
- **Remote players are a drop-in.** A net layer fills `PlayerInput` from the wire
  instead of the device; nothing downstream changes.
- **The simulation is replayable.** Because movement is a pure function of
  `(state, intents)`, a net layer can predict it on the client and roll it back on
  a server correction. `Intent` is the serializable chokepoint every networking
  model — lockstep, rollback, snapshot — sends per player and replays.

**The anti-pattern**, deliberately avoided:

```cpp
// DON'T: one system that reads the device and moves the pawn directly.
void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override
{
    if (context.Input.IsKeyDown(Key::W)) { pawn.Position.z += speed * delta; }
}
```

This collapses three roles into one. There is now no command an AI or a net layer
can write through, no replayable snapshot, and a Sim system reading the device —
breaking the determinism contract. The indirection through `Intent` *is* the
feature.

---

## 4. Configuring a system — config via components

A system carries **logic, not constants**. Its tunable parameters live in a
**settings component** the system reads off the entity, defaulting when the
component is absent. This reuses the entire reflection inspector for tuning — a
designer edits the values in the editor, no recompile — and keeps the system pure.

`MovementSystem` is the model: it reads each pawn's `Mover` for speed, falling
back to a default-constructed `Mover` when the pawn has none. `Mover` is a plain
reflected component:

```cpp
struct Mover
{
    f32 MoveSpeed = 4.0f;   // local-space units per second
    f32 TurnSpeed = 2.0f;   // radians per look unit
};
```

In the player prefab
([`player.prefab.json`](../../examples/hello-triangle/assets/prefabs/player.prefab.json))
the pawn carries `"::Veng::Mover": { "MoveSpeed": 5.0, "TurnSpeed": 2.0 }`. Change
those numbers in the inspector and the pawn's feel changes — the system never
moves.

Your own system follows the same shape: query `(SettingsComponent, ...)`, or
`scene.Has<Settings>(e) ? scene.Get<Settings>(e) : Settings{}` for the
default-if-absent case. Reach for a hardcoded constant only for a true engine
invariant, never for a value a designer would want to tune.

---

## 5. Registering a system

A system declares a stable identity through the `VE_SYSTEM` trait macro, and the
host registers it during `VengModuleRegister`. The identity is what lets the
editor catalog list it and a level name it.

### Declare the identity

```cpp
VE_SYSTEM(SpinnerSystem, 0xB5BB5153EC6ACDDEULL, "Spinner");
```

`VE_SYSTEM(Type, IdLiteral, NameLiteral)` specialises the `VengSystem<Type>` trait
with a stable `SystemId` and a display name. The `SystemId` is a new id space
alongside `AssetId` / `TypeId`. **Mint it with `vengc generate-id`** — never write
one by hand — and hardcode the value as an uppercase-hex `0x…ULL` literal in C++.
Two systems claiming one id is a fatal collision assert at registration. A system
registered without a `VE_SYSTEM` fails to compile.

### Register it

In the module's `VengModuleRegister`, register both the system and any component
types it reads:

```cpp
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<Spinner>();
    host->Systems.Register<SpinnerSystem>();

    // Registration order is run order within a phase.
    host->Systems.Register<SpawnPlayerRule>();
    host->Systems.Register<ControlSystem>();
    host->Systems.Register<MovementSystem>();
    host->Systems.Register<CameraRigSystem>();
    // ...
}
```

`SystemRegistry::Register<T>()` reads the `SystemId` and name off the trait and
stores `{ id, name, factory }` — it never instantiates the system. The host
(launcher or editor) owns the `SystemRegistry` and threads it through
`VengModuleHost::Systems`. Once registered, the system appears in the editor's
system catalog, ready to be enabled and ordered in a level.

**Registration order is run order within a phase.** hello-triangle registers
`ControlSystem` before `MovementSystem` so intent is produced before it is
consumed in the same tick; all three control systems are `Phase::Sim`, so they
finish before the `Phase::View` `CameraRigSystem` trails the moved pawn.

---

## 6. Wiring a system into a level

Registration makes a system *available*; a **`Level`** decides which systems are
*active*, in what order. A level is the unit of assembled game (see
[Wiring a level](wiring-a-level.md) for the concept in full). From the editor:

1. Open the `Level` asset — the `LevelEditorPanel`
   ([`editor/src/panels/LevelEditorPanel.h`](../../editor/src/panels/LevelEditorPanel.h))
   composes the prefab scene surface for the world plus two level-scoped children.
2. In the **systems panel**, toggle your system on and drag-reorder it into the
   active set. The panel lists the whole `SystemRegistry` catalog with phase
   labels; enabling and ordering writes the level's ordered `systems` list.
3. In the **world**, add your system's settings component to the relevant entity
   and tune it through the inspector (section 4).
4. In the **settings panel**, edit the game-mode and render config through the
   same reflection inspector.
5. Hit **Play** — the editor builds its simulation from exactly the level's
   ordered system set and ticks it.

That is the whole assembly loop: a game is composed as authored data, not
hardcoded in `main.cpp`.

---

## 7. Worked example, end to end

Let's make the "AI is just another `Intent` producer" payoff concrete: a **patrol**
that drives an entity back and forth between two points by *producing `Intent`*, so it
rides the existing `MovementSystem` exactly as a player does. The `Intent` producer for
an AI is the engine's shipped **`BehaviorSystem`**, so we do not write a bespoke system
at all — we write the *leaf* that carries the logic and build a tree around it. This is
the whole shape of AI work in veng: the composites, the per-agent state, and the tick are
the engine's; the leaf is yours. The behaviour runtime has its own guide,
[Writing AI behaviours](writing-ai-behaviors.md); this is the movement-pattern slice of it.

### The leaf — an Intent producer

```cpp
// Drives the pawn toward a world point by writing Intent — never by moving the Transform
// itself. The engine's MovementSystem consumes that Intent, so the patrol pawn moves
// through the identical path a player-controlled pawn does. Succeeds once it arrives.
class MoveToWaypoint final : public BehaviorTask
{
public:
    explicit MoveToWaypoint(vec3 goal) : m_Goal(goal) {}

    Status Tick(BehaviorContext& context) override
    {
        Transform& transform = context.Scene.Get<Transform>(context.Pawn);
        const vec3 toGoal = m_Goal - transform.Position;
        if (glm::length(toGoal) < 0.05f)
        {
            context.Scene.Get<Intent>(context.Pawn).Move = vec3(0.0f);
            return Status::Success;   // arrived; the Sequence advances to the next leaf
        }

        // Express the goal direction in the pawn's local frame and request it as movement;
        // the Mover's MoveSpeed and the MovementSystem do the rest.
        const vec3 localDir = glm::inverse(transform.Rotation) * glm::normalize(toGoal);
        context.Scene.Get<Intent>(context.Pawn).Move = localDir;
        return Status::Running;
    }

private:
    vec3 m_Goal;
};
```

`Intent` is overwritten each tick by its producer, so writing `Move` every tick (zeroing
it on arrival) is correct — a zero `Intent` is a pawn at rest.

### The tree

A `Sequence` of waypoints, each move followed by a two-second dwell, wrapped in a `Repeat`
so it loops forever:

```cpp
Ref<BehaviorTree> tree = BehaviorTreeBuilder()
    .Repeat()                                  // forever
        .Sequence()
            .Leaf(CreateRef<MoveToWaypoint>(pointA))
            .Wait(2.0f)
            .Leaf(CreateRef<MoveToWaypoint>(pointB))
            .Wait(2.0f)
        .End()
    .Build();
```

### Give the entity a behaviour

Add a `BehaviorAgent` carrying the tree and a seed. The entity needs the same movement
inputs a player's pawn has — `Transform`, `Intent`, `Mover` — because the leaf writes
`Intent` and `MovementSystem` reads it:

```cpp
const Entity pawn = scene.CreateEntity();
scene.Add<Transform>(pawn, Transform{});
scene.Add<Intent>(pawn, Intent{});
scene.Add<Mover>(pawn, Mover{.MoveSpeed = 5.0f});
scene.Add<BehaviorAgent>(pawn, BehaviorAgent{.Tree = tree, .Seed = 1});
```

Here the agent has no `Possesses`, so it acts on itself — the pawn is its own body. An AI
pilot flying a separate ship would instead carry `Possesses{ship}`, and the same leaf
would write the ship's `Intent`.

### Wire it into the level

`BehaviorSystem` and `MovementSystem` are both builtins, so a level just names them in
order — the producer before the consumer:

```
"systems": ["InputMappingSystem", "BehaviorSystem", "MovementSystem", ...]
```

The pawn shuttles between the points — and because it drives `Intent`, it obeys the same
`Mover` tuning and the same movement integration a player does.

### What it cross-references

This example reuses the real shipped pieces:

- **`Intent`** and **`Mover`** — the components from
  [`Components.h`](../../engine/include/Veng/Scene/Components.h).
- **`MovementSystem`** — the engine's `Intent` consumer from
  [`Movement.h`](../../engine/include/Veng/Scene/Movement.h), the exact system
  hello-triangle's `ControlSystem` feeds.
- **`BehaviorSystem`** — the engine's AI `Intent` producer from
  [`BehaviorSystem.h`](../../engine/include/Veng/Behavior/BehaviorSystem.h), registered in
  the control-system slot (after `InputMappingSystem`, before `MovementSystem`). It is the
  sibling of a player's `ControlSystem`: both are `Phase::Sim` `Intent` producers feeding
  one movement system. Swapping the device-reading producer for the behaviour tree is the
  whole difference — which is the point of the pattern. See
  [Writing AI behaviours](writing-ai-behaviors.md) for the tree in full.
- **`ControlSystem`** in
  [`main.cpp`](../../examples/hello-triangle/main.cpp) is the player-driven counterpart —
  a from-scratch `SceneSystem` in exactly the shape sections 1–6 describe, the model to
  copy when you *do* need a bespoke Sim system rather than a behaviour leaf.
- **`SpawnPlayerRule`** in the same file shows the `OnStart`/`OnStop`
  lifecycle in full: it spawns the configured player prefab at `OnStart` (gated on
  the scene carrying a `GameModeConfig`) and tears it down at `OnStop`.
- **`CameraRigSystem`** in [`CameraRig.h`](../../engine/include/Veng/Scene/CameraRig.h)
  is the `Phase::View` counterpart — it resolves each rigged camera after the Sim phase
  finalizes, deriving a purely local camera pose: a `CameraFollow` trails its target
  (third person), a `CameraLook` writes its yaw/pitch heading as the entity's rotation
  (first person). A control system drives both from the look action with its own
  sensitivity.

---

## 8. Reaching application operations — request components

A `SystemContext` carries no `Application` back-reference by design, so a system
**cannot call** the process-level operations: opening or joining a world, starting
to host, connecting, stopping the net mode, exiting, or holding an input-focus
token across frames. The bridge is a family of builtin, **local-only** request
components in
[`engine/include/Veng/Scene/Requests.h`](../../engine/include/Veng/Scene/Requests.h)
— `TravelRequest`, `HostRequest`, `ConnectRequest`, `StopNetRequest`,
`ExitRequest`, and `FocusRequest`. A system **stamps** one onto any world's scene;
`Application::Frame` **drains** it at its frame-safe point (before the world tick)
and reports the outcome back through the component. So a menu's Host button is a
system that stamps a `HostRequest`, not a call into the app:

```cpp
// A menu system reaching an application operation: stamp, don't call.
scene.Add<HostRequest>(menuEntity);   // the engine drains it and starts hosting
```

None of these is `VE_REPLICATED` — a request never rides the wire, and on a
`Client`-tier world it lowers to the client-side meaning (a `TravelRequest`
becomes a client travel/join). The **consumption semantics are uniform**: a
handled request is **removed** (absence is the acknowledgement — re-stamp freely),
an unhandleable one is left **`Pending`** and retried next frame, and a failed one
is marked `RequestStatus::Failed` with an `Error` string and **held exactly one
frame** so the stamping system can read the outcome. Read `Status` before
re-stamping — a system that re-stamps unconditionally every frame overwrites
`Failed` before it can be seen.

`FocusRequest` is the same idiom for the `InputRouter`'s **coarse** gameplay/UI
focus: stamp `FocusRequest{ Focus = Gameplay }` (a `Null` seat means the cursor
seat) to capture the pointer and `{ Focus = UI }` to release it. The engine owns a
single per-seat token behind it and reconciles idempotently, so a *stateless*
system can drive focus — which a raw `FocusToken` held across frames could not —
and the request-driven token never disturbs a token an overlay suspend or a
`SeatFocusScope` pushed.

**Focus-gated input contexts** are the authored, fine-grained complement to
`FocusRequest`. An `InputMappingContext` can declare `requiresGameplayFocus` in its
`*.inputmap.json`; `InputMappingSystem` then **excludes** that context from a seat's
effective bindings whenever the seat lacks gameplay focus (`SystemContext` carries a
`GameplayFocused` flag for exactly this). It is pure evaluation — the authored
`InputContextStack` is never mutated — so a mouse-look context that must go quiet
while a menu holds focus declares the gate instead of a system lifting it off the
stack and re-inserting it.

### The one exception: presentation binding may be a driver

Everything above keeps the rule that **simulation logic is components + systems**.
The single narrow exception is **presentation binding**: a `GuiOverlay` may name a
registered per-instance `GuiDriver` (see
[Screen-space UI and overlays](screen-space-ui-and-overlays.md)) that the engine
instantiates with the document. A driver *reads* scene state and *stamps*
request/command and `VE_VIEW_OUTPUT`-tagged components — exactly the writes a system
makes — but it **never** advances authoritative simulation and never writes a
`VE_REPLICATED` or Sim-input component. It is an ergonomic home for per-instance HUD
binding, not a controller object; your game logic still lives in systems.

---

## Where to go next

- **[Authoring input actions](authoring-input-actions.md)** — stage 1 of the
  Input → Intent → Movement pattern in full: named actions, the `*.inputmap.json`
  binding data, the seat's `InputContextStack`, and the ordering rule.
- **[Wiring a level](wiring-a-level.md)** — the `Level` asset, world prefab versus
  level-scoped data, and the load-to-play flow.
- The generated API reference (`cmake --build build --target docs`) documents
  every symbol named here in full.
- The hello-triangle module is the canonical, compiling reference — read it
  end to end.
