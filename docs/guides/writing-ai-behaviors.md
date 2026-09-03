# Writing AI behaviours

An entity decides what to do through a **behaviour tree**: a small tree of composites and decorators,
built in code, with your own logic at the leaves. The engine ticks it each simulation step and hands
each leaf the scene, the entity, and the pawn it acts through — so a leaf that writes the pawn's
`Intent` drives it through the same `MovementSystem` a player's control system feeds. An AI is just
another `Intent` producer; this guide is how to write one.

The runtime is `Veng/Behavior/`: `BehaviorTree.h` (the tree, the builder, the node kinds, the `BehaviorTask`
leaf), `BehaviorAgent.h` (the component that gives an entity a tree), and `BehaviorSystem.h` (the
system that ticks them). Nothing here is device-bound, so the whole thing is unit-testable headless.

## The shape

- A **`BehaviorTree`** is immutable and shared: build it once, and every entity running that
  behaviour holds the same `Ref<BehaviorTree>`.
- A **`BehaviorAgent`** component gives one entity that tree plus its own running state (a
  `vector<NodeSlot>`, one slot per node) and a **seed**. Two agents on one tree never share a slot.
- A **`BehaviorSystem`** (already a builtin) ticks every agent it has authority over each Sim step.

Because the tree carries no running state and the agent carries no logic, the same tree scales from
one turret to a thousand without copying anything but a slot vector per agent.

## The ECS is the blackboard

There is no separate blackboard object. A leaf reads and writes **components** on the agent and its
pawn, through the context it is handed:

```cpp
struct BehaviorContext
{
    Scene& Scene;             // the world — the blackboard
    Entity Agent;             // the entity carrying the BehaviorAgent
    Entity Pawn;              // what the agent acts through (its Possesses target, or itself)
    f32    Delta;             // seconds since the previous tick
    u64    Tick;              // the fixed simulation tick number
    Rng&   Random;            // this node's own reproducible random stream
    const SystemContext& System;   // authority, replay, and debug state
};
```

If a task needs to remember something across ticks, it keeps that in its **own component on the agent
entity** — a reflected struct or a `VE_TYPE` — which already has an inspector, serialisation, and
replication. The runtime never introduces a second data model to reflect or replicate.

## Writing a task

A `BehaviorTask` is the leaf you subclass. It has three hooks; only `Tick` is required:

```cpp
#include <Veng/Behavior/BehaviorTree.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

// Drives the pawn toward a world point by writing Intent; succeeds once it arrives.
class MoveToTask final : public Veng::BehaviorTask
{
public:
    explicit MoveToTask(Veng::vec3 goal) : m_Goal(goal) {}

    Veng::Status Tick(Veng::BehaviorContext& context) override
    {
        Veng::Transform& transform = context.Scene.Get<Veng::Transform>(context.Pawn);
        const Veng::vec3 toGoal = m_Goal - transform.Position;
        if (glm::length(toGoal) < 0.1f)
        {
            context.Scene.Get<Veng::Intent>(context.Pawn).Move = Veng::vec3(0.0f);
            return Veng::Status::Success;
        }

        // Express the goal in the pawn's local frame and request it as movement; the pawn's Mover
        // and the MovementSystem do the rest — exactly the path a player's Intent takes.
        const Veng::vec3 localDir =
            glm::inverse(transform.Rotation) * glm::normalize(toGoal);
        context.Scene.Get<Veng::Intent>(context.Pawn).Move = localDir;
        return Veng::Status::Running;
    }

private:
    Veng::vec3 m_Goal;
};
```

`Tick` returns one of three statuses:

- **`Running`** — not done; tick me again next step.
- **`Success`** — finished well.
- **`Failure`** — finished badly.

`OnEnter(context)` runs on the tick the leaf first becomes active, `OnExit(context, status)` on the
tick it finishes — use them to acquire and release whatever the run needs. One task instance is
shared by every agent, so a task holds **no per-agent mutable state** in its own fields; per-agent
memory lives in a component on the agent.

> **Replay and side effects.** The engine re-ticks the tree on a client reconciliation replay to
> re-derive `Intent`, so a leaf whose `Tick` has an *external* side effect — spawning an entity,
> firing a one-shot sound — must guard that effect on `context.System.IsReplay`. Writing a component
> (the ordinary case) needs no guard; the re-derivation is the point.

### Perception is a Condition

A **`Condition`** leaf is a predicate over the ECS — the perception this runtime needs. It takes a
function and returns `Success` when it holds, `Failure` otherwise:

```cpp
builder.Condition([](Veng::BehaviorContext& context)
{
    return context.Scene.Has<Threat>(context.Agent);
});
```

## Building a tree

`BehaviorTreeBuilder` spells the nesting as chained calls. A composite opens a scope you close with
`End()`; a decorator wraps the single node that follows it; a leaf attaches and closes itself:

```cpp
#include <Veng/Behavior/BehaviorTree.h>

using namespace Veng;

Ref<BehaviorTree> tree = BehaviorTreeBuilder()
    .Selector()                                        // first child that succeeds wins
        .Sequence()                                    // flee if threatened
            .Condition([](BehaviorContext& c) { return c.Scene.Has<Threat>(c.Agent); })
            .Leaf(CreateRef<FleeTask>())
        .End()
        .Repeat()                                      // otherwise patrol forever
            .Sequence()
                .Leaf(CreateRef<MoveToTask>(pointA))
                .Wait(2.0f)
                .Leaf(CreateRef<MoveToTask>(pointB))
                .Wait(2.0f)
            .End()
    .End()
    .Build();
```

The vocabulary:

| Kind | Builder call | Behaviour |
|---|---|---|
| **Sequence** | `Sequence() … End()` | Ticks children in order; stops at the first `Failure`; resumes a `Running` child next tick. Succeeds when all do. |
| **Selector** | `Selector() … End()` | Ticks children in order; stops at the first `Success`; resumes a `Running` child next tick. Fails when all do. |
| **Parallel** | `Parallel() … End()` | Ticks every child each tick; fails as soon as any child fails, succeeds once all have. |
| **Inverter** | `Inverter()` | Swaps its child's `Success` and `Failure`; passes `Running` through. |
| **Succeeder** | `Succeeder()` | Maps its child's finish to `Success`; passes `Running` through. |
| **Repeat** | `Repeat(n)` / `Repeat()` | Re-runs its child `n` times, then succeeds; `Repeat()` (or `0`) repeats forever. |
| **Until** | `Until(status)` | Re-runs its child while it returns `status`; any other finish ends it and is returned. |
| **Cooldown** | `Cooldown(seconds)` | Blocks its child (returns `Failure`) for `seconds` after it succeeds. |
| **Wait** | `Wait(seconds)` | A leaf that returns `Running` for `seconds`, then `Success` once. |
| **WaitRandom** | `WaitRandom(min, max)` | Like `Wait`, but the dwell is drawn from the node's seeded stream. |
| **Condition** | `Condition(fn)` | A leaf: `Success` when `fn` holds, `Failure` otherwise. |
| **Leaf** | `Leaf(task)` | Your `BehaviorTask`. |

A builder builds one tree; `Build()` consumes it.

## Giving an entity a behaviour

Add a `BehaviorAgent` carrying the tree and a seed. The seed makes the agent reproducible — each
node's random stream is derived from it and the node's position, so a `WaitRandom` or a chance-taking
task replays identically and two agents with the same seed make the same choices.

```cpp
const Entity pilot = scene.CreateEntity();
scene.Add<Possesses>(pilot, Possesses{.Pawn = ship});      // the pilot flies the ship
scene.Add<BehaviorAgent>(pilot, BehaviorAgent{.Tree = tree, .Seed = 1337});
```

The agent acts through the pawn it **possesses**, or through itself when it possesses nothing — so a
turret is its own body (no `Possesses`), while a pilot is the mind behind a ship it possesses. A leaf
reads `context.Pawn` and never does the lookup itself. Because possession is the engine's one
`Possesses` primitive, an agent whose pawn boards a `Vehicle` is re-pointed at it exactly as a
player's seat is.

`BehaviorAgent` is runtime-only: it is never serialised and never replicated. A behaviour is
re-decided from world state on whichever peer has authority, so a spawner adds the component at
runtime rather than authoring it into a prefab.

## Wiring the system

`BehaviorSystem` is a builtin, registered in `RegisterBuiltinSystems` **after `InputMappingSystem`
and before `MovementSystem`** — the slot a control system takes, so the `Intent` a tree writes is
consumed the same tick. A level simply names it in its `systems` array, before the movement system
that consumes what it produces:

```
"systems": ["InputMappingSystem", "BehaviorSystem", "MovementSystem", ...]
```

The system ticks only agents this peer has authority over (an agent on a `Remote`-tier entity is
skipped, exactly as the other authoritative Sim systems skip one), gathers agents before ticking any
so a task may safely spawn or destroy an entity mid-tick, and — when `SystemContext::Debug` is
present — marks the pawn of any agent whose tree is still running.

## What this is not

- **No cooked tree asset, no editor, no task registry.** Trees are built in code. Authoring one as an
  asset is a [custom asset type](custom-asset-types.md) for the day a designer needs to; the builder
  is that asset's in-memory target when it arrives.
- **No utility scoring or planner.** A tree composes — a new objective is a new subtree — and a
  `Condition` over the ECS is the perception a tree needs.
- **No replicated agent state.** An agent is authority-side by construction.
