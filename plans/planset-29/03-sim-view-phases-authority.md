# Plan 03 — Sim/View tick phases + Authority + camera rig

**Goal:** establish the two structural seams every networking model needs, while there is real
code to justify them. `SceneSystem` declares a **`Phase { Sim, View }`**, and `SceneSimulation`
runs all Sim systems then all View systems each tick — separating the deterministic, replicable
simulation from client-local view derivation. An **`Authority { Tier, Owner }`** annotation marks
ownership ahead of the net layer. And the first **View-phase** system lands: a client-local
**camera rig** that trails the possessed pawn, giving the split a concrete consumer.

## Why it is its own plan

By now there is a Sim system (movement, Plan 02) and a reason for a View system (a camera that
follows the pawn) — so the Sim/View split is justified by real systems rather than asserted in the
abstract. The split is the single highest-value networking-prep structural item, and the camera
rig is what makes it tangible; bundling the `Authority` annotation here keeps all the
"net-anticipation seams" in one reviewable plan, separate from the feature work they organize.

## What lands

- **`SceneSystem` declares a phase.** Add `enum class Phase { Sim, View };` and a
  `virtual Phase GetPhase() const { return Phase::Sim; }` to `SceneSystem`
  ([SceneSystem.h:31](../../engine/include/Veng/Scene/SceneSystem.h:31)) — default `Sim`, so every
  existing system (the spinner, control, movement) keeps its behavior. A system overrides it to
  `View` to run in the view pass.

- **`SceneSimulation::Update` runs Sim then View.** Update iterates the registered systems in two
  passes: all `Phase::Sim` systems in registration order, then all `Phase::View` systems in
  registration order ([SceneSimulation.h:16](../../engine/include/Veng/Scene/SceneSimulation.h:16)).
  `Start`/`Stop` are unchanged (they bracket the session). The two-pass split is the whole
  mechanism — **not** a dependency graph or parallel scheduler; intra-phase ordering stays
  registration order (a richer scheduler is a named future-area-7 increment).

- **`Authority` — the ownership annotation.** A reflected component
  `Authority { Tier Tier; u32 Owner; }` with `enum class Tier { Server, Local };` (the minimal
  set; `Remote`/`Predicted` arrive with the net layer). It is threaded onto entities with sensible
  defaults (authored entities default `Server`; client-local view entities like cameras default
  `Local`) and is **read by nothing in this planset** — its consumer is the future net layer. It is
  cheap to add now precisely because there are only a handful of spawn sites today; the value is
  locking the defaulting discipline in before that count grows, since the cost is per-spawn-site and
  the net layer will not revisit every one. It commits to no replication strategy.

- **The camera rig — the first View-phase system.** A `SceneSystem` with `GetPhase() == View`
  that, for a configured follow relationship (a `CameraFollow { Entity Target; vec3 Offset; f32
  Damping; }` component on the camera entity, or a field on the `Viewer`), reads the **target's**
  world `Transform` and writes the **camera entity's** `Transform` to trail it (offset, optional
  smoothing). Because it runs in the View phase, it reads pawn state the Sim phase already
  finalized this tick and produces purely local view state — never authoritative, never on the
  wire. This is where camera blend/shake would later live (the one place view interpolation
  composes), all View-phase.

## Why the split is justified ahead of networking — and Authority is flagged

The Sim/View split has **today value** independent of networking: it cleanly orders "finish the
simulation, then derive the view," which is exactly the right order for a camera that must follow
the post-update pawn position, and it documents which systems are safe to pause/skip (View can run
on a paused Sim; Sim must not depend on View). That today-value, plus its model-agnostic
net-readiness and high retrofit cost, earns it now.

**`Authority` is the one purely-anticipatory piece** — inert until the net layer reads it. It is
included on the explicit intent to anticipate networking: the honest cost argument is that today's
few spawn sites make the defaulting trivial to add, whereas a populated codebase makes it tedious —
not that the *component* is hard to add later (the ECS adds a component to existing entities
freely). The two-tier `{Server, Local}` guess may also not survive contact with the real net
model. So it is the natural thing to **defer** if a reviewer judges an unread, possibly-wrong-shaped
annotation too speculative: dropping it removes a component and its spawn-site defaults and changes
nothing else in the planset. The Sim/View split does **not** depend on it.

## Decisions

1. **Two phases, two passes — minimal.** `Sim` then `View`, registration order within each. No
   scheduler, no inter-system dependencies; that is a separate future-area-7 increment. The split
   is structural, not a perf feature.

2. **Default `Phase::Sim`** so the change is non-breaking — every existing system runs exactly as
   before until it opts into `View`.

3. **Camera behavior is View-phase, always.** A follow/blend/shake system reads finalized Sim
   state and writes local view; putting it in Sim would make the camera part of the replicated
   simulation, which principle 4 forbids.

4. **`Authority` is a lean annotation with only `Server`/`Local` now.** No `Remote`/`Predicted`
   until a net layer defines their semantics; over-specifying the enum ahead of its consumer would
   be the premature-taxonomy mistake this planset exists to avoid.

5. **Smoke safety for View systems.** The smoke path does not tick `Update`, so the camera rig
   (a per-tick `Update` behavior) never runs in the captured frame: **in smoke the camera's pose is
   exactly the authored or spawned `Transform`, rig-independent.** The invariant the golden rests on
   is therefore that the authored/spawned camera transform alone produces the capture pose — which
   Plan 04 must hold when the seat moves onto a spawned player. If a future smoke variant ticks the
   simulation, the rig must be deterministic (no wall-clock smoothing in the fixed-pose path).

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/SceneSystem.h` | Add `Phase` enum + `GetPhase()` (default `Sim`). |
| `engine/include/Veng/Scene/SceneSimulation.{h}` / `engine/src/Scene/SceneSimulation.cpp` | `Update` runs Sim then View passes; `Start`/`Stop` unchanged. |
| `engine/include/Veng/Scene/Components.h` | `Authority` (+ `Tier`) and `CameraFollow` components + reflect blocks. |
| `engine/include/Veng/Scene/BuiltinTypes.h` / `.cpp` | Register `Authority`, `CameraFollow`. |
| `engine/src/Scene/…` (or example src) | The camera-rig View-phase system. |
| `examples/hello-triangle/…` | The camera follows the Plan 02 pawn via `CameraFollow`; entities carry `Authority` defaults. |
| `tests/unit/…` | Phase-ordering + rig tests (see below). |

## Tests

- **Phase ordering (unit):** systems in both phases recording call order assert all `Sim` run
  before all `View` within a tick, registration order preserved within each phase; a `Stop`/`Start`
  cycle is unaffected.
- **Camera rig (unit, pure):** with a target `Transform` and a `CameraFollow` offset, the rig
  writes the expected camera `Transform`; it runs **after** a Sim movement step in the same tick
  (proving it reads finalized state). Deterministic with a fixed `delta`.
- **Non-breaking default:** an existing `Sim`-default system still ticks as before.

## Verification

- Clean build; `ctest` green across the bands, including the phase-ordering + rig tests.
- The no-device cooker test stays green (`Authority`, `CameraFollow` are GPU-free).
- `hello_triangle_launcher_smoke` exits 0 and writes a correct-sized PPM.
- `smoke_golden` does **not** move (View systems do not run in the pinned smoke frame; the camera
  pose is the authored one).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
