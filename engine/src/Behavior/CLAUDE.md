# Veng/Behavior — the behaviour runtime

The engine's decision-making arm: a **behaviour tree** built in code, a **`BehaviorAgent`** component
that gives an entity one plus the state for running it, a **`BehaviorTreeRegistry`** catalog and the
**`BehaviorTreeRef`** component that select a registered tree from authored data, and a
**`BehaviorSystem`** that ticks every agent's tree each Sim step. It is the AI producer the control
pipeline documents but never shipped — `Intent` is written for three deciders (a player, an AI, a
remote), and this is the AI one. A leaf that writes a pawn's `Intent` drives it through the same
`MovementSystem` a player's control system feeds; nothing about the runtime is game-specific.

The public surface is `engine/include/Veng/Behavior/` (`BehaviorTree.h`, `BehaviorAgent.h`,
`BehaviorTreeRegistry.h`, `BehaviorSystem.h`); the tick walk, the builder and the catalog live in
`engine/src/Behavior/`.

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
succeeding on all and failing on any; `ReactiveSelector` and `ReactiveSequence`, which re-evaluate
rather than resume — see [Priority and aborts](#priority-and-aborts)), **decorators** wrapping one child (`Inverter`, `Succeeder`,
`Repeat(n | forever)`, `Until` — repeat while the child returns a given status —, `Cooldown`), and
**leaves** (a consumer `Task`, a `Wait`/`WaitRandom` dwell timer, a `Condition` predicate over the
ECS — the perception this phase needs). A `Task` is the one kind a consumer subclasses: `OnEnter`,
`Tick → Status`, `OnExit(Status)`, `OnAbort`. One task instance is shared by every agent, so it holds no
per-agent state.

## Seeded, so an agent replays

`BehaviorAgent::Seed` makes an agent reproducible. Each node draws from `Rng(HashCombine(Seed,
nodeIndex))` (the `Random.h` idiom), so a `WaitRandom` draws the same delay on a reconciliation
replay and two agents with the same seed make the same choices — independent of what any other node
drew, because the stream is keyed by position rather than by draw order.

## The catalog and the authored reference

A tree's *structure* is always code; which tree an entity runs can be data. A module registers each
tree it builds under a minted **`BehaviorTreeId`** (a `u64` leaf in the `SystemId`/`GuiDriverId`
family, authored as a hex-id string) with a display name and a build function:
`host->Systems.GetBehaviorTrees().Register(id, name, build)`. The catalog rides the
**`SystemRegistry`** rather than a new `VengModuleHost` field, because trees are, like systems,
code-registered gameplay logic an authored id selects, and the system catalog already reaches every
host that loads a module — the launcher, the editor, the cooker and the tests. Registration builds
nothing (the cooker holds a catalog with no device); **`Resolve(id)`** builds a tree on its first call
and returns the same shared tree after, so every agent naming one id runs one immutable tree. A
duplicate id is a fatal collision assert, as for systems. Systems reach it as the required
**`SystemContext::BehaviorTrees`**, filled by the context factory from the host's system registry.

**`BehaviorTreeRef { Tree, Seed }`** is the authored half of an agent: a reflected component a prefab
or level carries, which the editor's inspector draws as a combo over the catalog's names.
`BehaviorAgent` stays the runtime half and records the id it was resolved from in **`Source`** —
`Null` for an agent a spawner built in code. Choosing a tree from run-time state (a pad, a heading, a
destination) is still a code-built agent; the reference selects a fixed tree.

## What the system does, and the ordering it takes

`BehaviorSystem` (`Phase::Sim`) is registered in `RegisterBuiltinSystems` **after `InputMappingSystem`
and before `MovementSystem`** — the same slot a control system takes, so the `Intent` a tree writes is
consumed the same tick. It:

- **resolves authored references first** — at `OnStart`, and ahead of each tick's gather, so no
  structural change happens inside a view — for every entity it simulates under `HasAuthority`: a
  `BehaviorTreeRef` with no agent gets one (`Tree` resolved, `Seed` and `Source` from the reference;
  an id no tree claims is warned once per id and adds nothing); an agent whose `Source` differs from
  its reference's `Tree` is **aborted, removed and re-added** on the new tree with clean slots, so
  the old run's `OnAbort` lands before the new leaf's `OnEnter`; and an agent with a valid `Source`
  whose reference is gone is aborted and removed. An agent with a `Null` `Source` is never touched,
  so code-built agents behave as they always have. `Seed` is read at resolve, so editing it alone
  does not restart a running agent;
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
  **leaf's own** to guard on `IsReplay`: the engine owns the tick, the leaf owns its effects;
- **aborts every run in progress at `OnStop`**: each agent it simulates under `HasAuthority` has its
  tree aborted from the root (`BehaviorTree::Abort`) with the stop context's tick and a zero delta,
  while the scene and every service are still live — so a world closed, a scene replaced or an
  application shut down ends each run in exactly one of `OnExit` or `OnAbort`, as the contract
  states. A client mirror never ticked its agents and has nothing to abort.

`BehaviorAgent` is `VE_TYPE` (runtime-only), `BehaviorTreeRef` is reflected, and `BehaviorSystem` is
`VE_SYSTEM`; all register through the ordinary `RegisterBuiltinTypes` / `RegisterBuiltinSystems` path.
The runtime's module-ABI surface is `BehaviorTask`'s vtable — a module subclasses it and the engine's
walk dispatches through it, so a new virtual on it bumps `VENG_MODULE_ABI_VERSION` — plus the catalog
a module registers into through `SystemRegistry`, and `SystemContext::BehaviorTrees`.

## Priority and aborts

`Sequence` and `Selector` **resume** their running child, so a branch before it is never
reconsidered while it runs. Priority needs the other shape, and the two reactive composites
**re-evaluate** instead:

- **`ReactiveSelector`** ticks its children from the first on every tick. The first to return
  `Running` or `Success` wins; a child that was running last tick and is no longer the winner is
  aborted. A condition-guarded branch placed first ("flee when hurt", "stop when told") therefore
  takes over the tick its guard passes. A child returning `Failure` finished, so it is neither
  aborted nor reset — a `Cooldown` under it keeps its timer.
- **`ReactiveSequence`** re-ticks every child before its running one on every tick, each of which
  must return `Success` again. One returning `Failure` aborts the running child and fails the
  sequence; one returning `Running` aborts the running child and becomes the running one. Then the
  running child ticks and the sequence progresses as a `Sequence` does. The children before an
  action are meant to be conditions or idempotent: re-ticking an action that already finished
  restarts it.
- **A failing `Parallel`** aborts the children still running.

**The abort contract.** Aborting a subtree calls `BehaviorTask::OnAbort` on every leaf in it whose
slot is active, with that leaf's own seeded stream, then resets the subtree's slots. `OnAbort` fires
**once per abandoned run, before the reset, and never after `OnExit`** — a leaf that finished is
inactive, so every run ends in exactly one of `OnExit` or `OnAbort`. A reactive selector aborts the
displaced branch **after** the winner has ticked, so the winner's `OnEnter` still sees the state the
displaced branch left. Per-agent state a task keeps in a component is therefore removed in `OnAbort`
and `OnExit` alike; a task that cleans up in only one of them leaks on the other path.

## Two behaviours worth knowing

- **A stop aborts; removing a code-built agent mid-run does not.** Besides the tree walk, aborts come
  from `BehaviorSystem` itself — at `OnStop`, and when an authored agent is retargeted or loses its
  reference. A code-built `BehaviorAgent` removed, or an entity destroyed, while a leaf is running
  calls neither `OnExit` nor `OnAbort`. State kept in a component on the agent entity goes when the
  entity does; a consumer that strips only the `BehaviorAgent` removes the task components with it.
  A task's `OnAbort` therefore also runs during a stop, and must not assume the world goes on.
- **The running-agent marker is a scene gizmo, not a `BehaviorSystem` draw.** A mark at the pawn of
  an agent whose tree is `Running` is the `SceneGizmo::Agents` family
  (`Veng/Renderer/SceneGizmos.h`), drawn only when a consumer selects that family — the debug-draw
  surface carries no world-space text, so it marks the pawn rather than naming the running leaf. It
  reads the root's status through `BehaviorTree::RootStatus` rather than ticking, so it never
  advances an agent. It lives there rather than in `BehaviorSystem::OnUpdate` so it is opt-in like
  every other gizmo family: a system that drew it whenever a debug sink merely existed painted every
  running agent in any scene whose viewport carried a debug pass at all.

## The guide

`docs/guides/writing-ai-behaviors.md` is the consumer walkthrough — build a tree, write a task, give
an entity an agent in code or register the tree and author a `BehaviorTreeRef`, wire the system — and the `writing-gameplay-systems.md` patrol example is written
on this runtime, so the guide's AI exemplar is real, compiled code.
