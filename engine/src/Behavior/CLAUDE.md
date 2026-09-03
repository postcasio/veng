# Veng/Behavior — the behaviour runtime

The engine's decision-making arm: a **behaviour tree** built in code, a **`BehaviorAgent`** component
that gives an entity one plus the state for running it, and a **`BehaviorSystem`** that ticks every
agent's tree each Sim step. It is the AI producer the control pipeline documents but never shipped —
`Intent` is written for three deciders (a player, an AI, a remote), and this is the AI one. A leaf
that writes a pawn's `Intent` drives it through the same `MovementSystem` a player's control system
feeds; nothing about the runtime is game-specific.

The public surface is `engine/include/Veng/Behavior/` (`BehaviorTree.h`, `BehaviorAgent.h`,
`BehaviorSystem.h`); the tick walk and the builder live in `engine/src/Behavior/`.

## The ECS is the blackboard

There is deliberately **no `Blackboard` type**. A task reads and writes components on the agent and
its pawn through the `BehaviorContext`'s `Scene&`, and cross-tick memory it needs lives in its own
component on the agent entity — which already has reflection, an inspector, serialisation, and
replication. A second data model would duplicate all of that, so the runtime introduces none: the
house rule is that a system's data is components, and a behaviour is no exception.

## The tree is immutable; the state is per agent

A `BehaviorTree` is a **flat array of nodes** carrying only structure and authored parameters — no
running state. It is built once by a `BehaviorTreeBuilder` (nesting spelled as chained calls, a
composite closed by `End()`, a decorator closing itself once its one child is complete) and shared by
every agent through a `Ref<BehaviorTree>`. The running state — a node's status, a `Wait`'s remaining
time, a `Repeat`'s count, a composite's resumed-child index — lives in the agent's **`vector<NodeSlot>`**,
one slot per node, indexed by the node's position. Two agents on one tree never share a slot, so their
running positions never collide. `NodeSlot` is plain copyable data, which keeps `BehaviorAgent`
poolable.

The node families: **composites** (`Sequence` stops at the first `Failure`, `Selector` at the first
`Success`, both resuming a `Running` child next tick; `Parallel` ticks every child each tick,
succeeding on all and failing on any), **decorators** wrapping one child (`Inverter`, `Succeeder`,
`Repeat(n | forever)`, `Until` — repeat while the child returns a given status —, `Cooldown`), and
**leaves** (a consumer `Task`, a `Wait`/`WaitRandom` dwell timer, a `Condition` predicate over the
ECS — the perception this phase needs). A `Task` is the one kind a consumer subclasses: `OnEnter`,
`Tick → Status`, `OnExit(Status)`. One task instance is shared by every agent, so it holds no
per-agent state.

## Seeded, so an agent replays

`BehaviorAgent::Seed` makes an agent reproducible. Each node draws from `Rng(HashCombine(Seed,
nodeIndex))` (the `Random.h` idiom), so a `WaitRandom` draws the same delay on a reconciliation
replay and two agents with the same seed make the same choices — independent of what any other node
drew, because the stream is keyed by position rather than by draw order.

## What the system does, and the ordering it takes

`BehaviorSystem` (`Phase::Sim`) is registered in `RegisterBuiltinSystems` **after `InputMappingSystem`
and before `MovementSystem`** — the same slot a control system takes, so the `Intent` a tree writes is
consumed the same tick. It:

- **gathers agents into a member vector before ticking any**, so a task that spawns or destroys an
  entity mid-tick does not invalidate the iteration (the same hazard any `View` has), and re-checks
  `IsAlive` per agent since a prior agent's task may have destroyed a later one;
- **skips an agent failing `HasAuthority`**, exactly as the other authoritative Sim advancers do, so a
  client never ticks a tree for an entity it only mirrors — an agent is authority-side by construction
  and its state is never replicated;
- **resolves the pawn through `Possesses`**: the agent's `Possesses.Pawn` when it carries one (and it
  is alive), else the agent itself — so an agent is its own body or the mind behind a possessed one;
- **still ticks on a replay** (`SystemContext::IsReplay`), because intent must be re-derived as
  prediction re-runs control. **A leaf's *external* side effect** (a spawn, an audio one-shot) is the
  **leaf's own** to guard on `IsReplay`: the engine owns the tick, the leaf owns its effects.

`BehaviorAgent` is `VE_TYPE` (runtime-only) and `BehaviorSystem` is `VE_SYSTEM`; both register through
the ordinary `RegisterBuiltinTypes` / `RegisterBuiltinSystems` path, so the module ABI is untouched.

## Two behaviours worth knowing

- **A `Parallel` that completes abandons a still-`Running` sibling without an `OnExit`.** The status
  has no "aborted" value, so the abandoned subtree is reset silently. A leaf that must clean up on
  abandonment does it from its own component lifecycle, not from `OnExit`.
- **The one debug affordance is a marker, not a label.** `SystemContext::Debug`, when present, marks
  the pawn of an agent whose tree is `Running` — the debug-draw surface carries no world-space text, so
  it marks rather than names the running leaf.

## The guide

`docs/guides/writing-ai-behaviors.md` is the consumer walkthrough — build a tree, write a task, give
an entity an agent, wire the system — and the `writing-gameplay-systems.md` patrol example is written
on this runtime, so the guide's AI exemplar is real, compiled code.
