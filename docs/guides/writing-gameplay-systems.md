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

| | `Phase::Sim` (the default) | `Phase::View` |
|---|---|---|
| What it is | Deterministic, replicable game simulation | Client-local view derivation |
| Examples | control, movement, game-mode rules | camera rig, blends, screen shake |
| On the wire | Replicated; authoritative | Never replicated; derived per client |

**Why the line matters.** The Sim/View split is the structural seam a networking
layer and a paused-sim editor both rely on. Replicate the *pawn* (Sim); each
client derives its *own* camera (View). A future net layer simulates the Sim
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

- **`PlayerInput`** — a per-player snapshot of *this tick's raw control state*:
  `vec3 Move`, `vec2 Look`, `u32 Buttons`. For a local player it is filled from
  `Veng::Input`; a remote player's would be filled from the wire.
- **`Intent`** — an *abstract, source-agnostic command*: `vec3 Move` (in the
  pawn's local frame), `vec2 Look`, `u32 Actions`. It answers "what does this pawn
  want to do this tick," divorced from who decided it.
- **`Possesses`** — a seat-to-pawn link: `Entity Pawn`, the pawn this seat
  controls.

### Stage 1 + 2: a control system reads input, writes intent

hello-triangle's `ControlSystem` (in
[`main.cpp`](../../examples/hello-triangle/main.cpp)) is `Phase::Sim` and does
exactly this — reads the always-present `Input` into the local player's
`PlayerInput`, then maps that snapshot onto the possessed pawn's `Intent`:

```cpp
class ControlSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32, const SystemContext& context) override
    {
        const Input& input = context.Input;

        vec3 move{0.0f};
        if (input.IsKeyDown(Key::W)) { move.z += 1.0f; }
        if (input.IsKeyDown(Key::S)) { move.z -= 1.0f; }
        if (input.IsKeyDown(Key::D)) { move.x += 1.0f; }
        if (input.IsKeyDown(Key::A)) { move.x -= 1.0f; }

        u32 buttons = 0;
        if (input.IsKeyDown(Key::Space)) { buttons |= static_cast<u32>(PlayerButton::Jump); }

        const vec2 look = input.GetMouseDelta();

        scene.Each<PlayerInput, Possesses>(
            [&](Entity, PlayerInput& player, Possesses& possesses)
            {
                player.Move = move;
                player.Look = look;
                player.Buttons = buttons;

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

The button bit layout (`PlayerButton::Jump`) is game policy — the engine treats
the bitset as opaque. The mapping itself is factored into a pure free function so
it is testable without an `Input` or a scene:

```cpp
Intent MapInputToIntent(const PlayerInput& input)
{
    Intent intent;
    intent.Move = input.Move;
    intent.Look = input.Look;
    intent.Actions = input.Buttons;
    return intent;
}
```

Note this Sim system *does* read `context.Input` — and that is the **one
sanctioned place**. The control system is the edge that translates the device into
the `Intent` snapshot; it is the boundary the determinism contract draws. Treat
the look value here as a captured snapshot, not a continuous device poll consulted
again downstream.

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
  producer."
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
([`player.prefab.json`](../../examples/hello-triangle/assets/player.prefab.json))
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

Let's build a small but complete Sim-phase gameplay system from scratch: a
**patrol mover** that drives an entity back and forth between two points by
*producing `Intent`*, so it rides the existing `MovementSystem` exactly as a
player does. This shows the Input → Intent → Movement pattern with an AI producer
substituted for the control system — the "AI is just another `Intent` producer"
payoff made concrete.

### The settings component

```cpp
// A patrol path and pace; tuned in the inspector, read by the patrol system.
struct Patrol
{
    vec3 PointA{0.0f};
    vec3 PointB{0.0f};
    f32  Speed = 1.0f;   // fraction of the leg traversed per second
};

VE_REFLECT(Patrol, 0x…ULL)   // mint with `vengc generate-id`
VE_FIELD(PointA, .DisplayName = "Point A")
VE_FIELD(PointB, .DisplayName = "Point B")
VE_FIELD(Speed,  .DisplayName = "Speed", .Min = 0.0)
VE_REFLECT_END();
```

### The system — an Intent producer

```cpp
// Drives each (Transform, Intent, Patrol) pawn toward its current goal by writing
// Intent — never by moving the Transform itself. The engine's MovementSystem
// consumes that Intent, so the patrol pawn moves through the identical path a
// player-controlled pawn does. Phase::Sim (the default): deterministic, reads only
// entity state — no device, no wall clock.
class PatrolSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32, const SystemContext&) override
    {
        scene.Each<Transform, Intent, Patrol>(
            [&](Entity entity, Transform& transform, Intent& intent, Patrol& patrol)
            {
                const vec3 goal = m_TowardB[entity] ? patrol.PointB : patrol.PointA;
                const vec3 toGoal = goal - transform.Position;

                if (glm::length(toGoal) < 0.05f)
                {
                    m_TowardB[entity] = !m_TowardB[entity];   // reached the goal; flip
                    intent.Move = vec3(0.0f);
                    return;
                }

                // Express the goal direction in the pawn's local frame and request it
                // as movement; the Mover's MoveSpeed and the MovementSystem do the rest.
                const vec3 localDir = glm::inverse(transform.Rotation) * glm::normalize(toGoal);
                intent.Move = localDir * patrol.Speed;
            });
    }

private:
    map<Entity, bool> m_TowardB;
};

VE_SYSTEM(PatrolSystem, 0x…ULL, "Patrol");   // mint with `vengc generate-id`
```

`Intent` is overwritten each tick by its producer, so writing `intent.Move` every
tick (zeroing it at the goal) is correct — a zero `Intent` is a pawn at rest.

### Register it

```cpp
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<Patrol>();

    // PatrolSystem produces Intent; MovementSystem consumes it. Register the
    // producer first so the Intent is fresh when movement runs this same tick.
    host->Systems.Register<PatrolSystem>();
    host->Systems.Register<MovementSystem>();
    // ...
}
```

### Wire it into the level

In the level editor, enable `PatrolSystem` and `MovementSystem` (in that order),
add a patrolling entity to the world carrying `Transform`, `Intent`, `Mover`, and
`Patrol`, set its two points and speed in the inspector, and Play. The pawn
shuttles between the points — and because it drives `Intent`, it obeys the same
`Mover` tuning and the same movement integration a player does.

### What it cross-references

This example reuses the real shipped pieces:

- **`Intent`** and **`Mover`** — the components from
  [`Components.h`](../../engine/include/Veng/Scene/Components.h).
- **`MovementSystem`** — the engine's `Intent` consumer from
  [`Movement.h`](../../engine/include/Veng/Scene/Movement.h), the exact system
  hello-triangle's `ControlSystem` feeds.
- **`ControlSystem`** in
  [`main.cpp`](../../examples/hello-triangle/main.cpp) is the player-driven
  sibling of `PatrolSystem`: both are `Phase::Sim` `Intent` producers feeding one
  movement system. Swapping the device-reading producer for an AI one is the whole
  difference — which is the point of the pattern.
- **`SpawnPlayerRule`** in the same file shows the `OnStart`/`OnUpdate`/`OnStop`
  lifecycle in full: it spawns the configured player prefab at `OnStart` and tears
  it down at `OnStop`, reading the game-mode `Session` state.
- **`CameraRigSystem`** in [`CameraRig.h`](../../engine/include/Veng/Scene/CameraRig.h)
  is the `Phase::View` counterpart — it trails the moved pawn after the Sim phase
  finalizes, deriving a purely local camera pose.

---

## Where to go next

- **[Wiring a level](wiring-a-level.md)** — the `Level` asset, world prefab versus
  level-scoped data, and the load-to-play flow.
- The generated API reference (`cmake --build build --target docs`) documents
  every symbol named here in full.
- The hello-triangle module is the canonical, compiling reference — read it
  end to end.
