# Plan 02 — Input → Intent → Movement

**Goal:** build the control pipeline as data, not an object. Device input is captured into a
per-player **`PlayerInput`** snapshot; a **control** system translates it into an abstract
**`Intent`** command; a **movement** system consumes intent and moves the possessed pawn.
Possession is a **`Possesses`** reference. There is no controller class — and crucially, the
control system writes *intent*, never motion, so AI and (later) remote players are drop-in intent
producers and the simulation becomes a pure function of state + intents.

## Why it is its own plan

The intent pipeline is the net-critical seam (the chokepoint a net layer serializes, predicts,
and rolls back) and is orthogonal to the camera work — it drives a `Transform` through `Intent`,
needing no `Viewer`. It runs in parallel with Plan 01 (it touches no camera path), relying only on
the delivered scene-systems framework and Plan 00's always-present `Input` (so the control system
reads input with no null-guard); Plan 00 lands first regardless, so this adds no reordering. It is
reviewable on its own before Plan 03 sorts it into the Sim phase and Plan 04 spawns a player
through it.

## What lands

- **`PlayerInput` — the per-player input snapshot (the chokepoint).** A reflected component
  holding this tick's control state: movement axes (`vec2`/`vec3`), look axes (`vec2`), and a
  button bitset. For the **local** player it is filled each tick from the engine `Veng::Input`
  service the `SystemContext` carries
  ([SceneSystem.h:21](../../engine/include/Veng/Scene/SceneSystem.h:21)); a future **remote**
  player's is filled from the wire. It is deliberately a *component snapshot*, not a direct read
  of the device, so the same downstream systems run identically regardless of where the input
  came from — this is what a net layer serializes and replays.

- **`Intent` — the abstract command.** A reflected component carrying *what the pawn wants to do*
  this tick, device- and source-agnostic: a desired move vector (pawn-local), a desired look
  delta, action flags (jump/fire/...). It is the interface between "who decides" (player, AI,
  remote) and "what happens" (movement/gameplay). Cleared/overwritten each tick by its producer.

- **`Possesses` — the seat→pawn link.** A reflected reference `Possesses { Entity Pawn; }` on the
  player/seat entity (chosen over an inverse `ControlledBy { Entity Player }` on the pawn so the
  player stays the active subject). Possession is just this reference; nothing inherits or owns
  through it.

- **The control system (`PlayerInput` + `Possesses` → `Intent`).** A `SceneSystem` that, per
  player with input + a possessed pawn, maps the input snapshot to the pawn's `Intent`
  (movement axes → desired move in the pawn's local frame, look axes → desired look delta, buttons
  → action flags). It reads `Veng::Input` unconditionally — in headless the always-present service
  (Plan 00) reports the all-zeros state, so it naturally produces a zero `Intent` and the pawn
  stays put, with no null to guard. It writes through the scene `Intent` accessor — never a
  retained reference — so the spatial/version bookkeeping is correct.

- **The movement system (`Intent` → `Transform`).** A `SceneSystem` that integrates each pawn's
  `Intent` into its `Transform` (translate by the desired move × speed × `delta`, rotate by the
  look delta), scaled by per-pawn tuning carried on a **`Mover { f32 MoveSpeed; f32 TurnSpeed; }`**
  component on the pawn (authored data, so feel is tunable; defaults apply if absent). This is the
  authoritative gameplay step; it reads intent and pawn state only, so it is deterministic.

- **AI is a drop-in producer.** Document (and optionally demonstrate) that an AI system writing
  the same `Intent` component drives the same movement system with **no** player, `PlayerInput`,
  or possession involved — the uniformity the intent split buys.

- **Sample demonstration (windowed only).** The windowed app gets a player-driven pawn: a
  `PlayerInput`-fed seat possessing a movable primitive the player drives with WASD/look, the
  authored scene camera (Plan 01) watching the scene. Windowed-only: the smoke path does not tick
  `Update` ([main.cpp:192](../../examples/hello-triangle/main.cpp:192)), and even if it did the
  headless `Input` reads all-zeros, so the pawn never moves in the capture.

## Decisions

1. **Control produces intent; movement consumes it — never fused.** This is the spine (principle
   2). It looks like indirection in single-player, but it is the seam that makes prediction/rollback,
   AI, and replay tractable, and it is cheap now (one extra component, one extra system boundary).

2. **`PlayerInput` is a component snapshot, not a device read inside the system.** A system that
   reaches straight into `Veng::Input` cannot be driven by a recorded or networked input; routing
   through a component makes the source swappable. The local capture from `Veng::Input` is the one
   place the device is read.

3. **Possession is a reference, independent of the camera.** `Possesses` links a seat to a pawn;
   it has nothing to do with `Viewer.Camera` (scope decision 5). A first-person setup that wants
   the pawn's camera to be the seat's view is an opt-in wiring (Plan 03's rig or a direct
   `Viewer.Camera = pawnCamera`), not a coupling baked into possession.

4. **Input is always present; headless is its neutral reading, not an absence.** The control system
   reads `Input` unconditionally — there is no null to guard (Plan 00). In headless the service
   reports all-zeros, identical to an idle windowed frame, so the system produces no motion and the
   tick stays a pure function of state. Determinism follows because headless input is the zero
   element, not because of a special-case branch.

5. **Single local player.** One `PlayerInput`, populated from the one `Veng::Input`. Routing
   distinct inputs to distinct seats is out of scope (scope decision 8) — the future-area-4 seam.

6. **The generic movement system ships with the engine; the game-specific control mapping stays in
   the example.** Movement (`Intent` → `Transform`) is reusable across games, so it is engine-side
   and gets a core-pack `SystemId` (Plan 05); the WASD/look → `Intent` mapping is game policy, so it
   lives in the example. This split decides where each system's `SystemId` is minted and what the
   guide (Plan 08) references.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Scene/Components.h` (or new headers) | `PlayerInput`, `Intent`, `Possesses`, `Mover { MoveSpeed; TurnSpeed }` components + reflect blocks. |
| `engine/include/Veng/Scene/BuiltinTypes.h` / `.cpp` | Register the new components. |
| `engine/src/Scene/…` | The **generic movement** system (`Intent`→`Transform`), engine-provided (decision 6). |
| `examples/hello-triangle/…` (control mapping) | The **game-specific control** system (`PlayerInput`→`Intent`, WASD/look mapping), example-side (decision 6). |
| `examples/hello-triangle/…` | A local-player seat with `PlayerInput` + `Possesses`, a movable pawn, the control + movement systems registered; windowed-only behavior. |
| `tests/unit/…` | Control + movement math tests (see below). |

## Tests

- **Movement (unit, pure):** drive the movement system over a pawn with a synthetic `Intent`,
  asserting the `Transform` integrates as expected (move scales by `delta` × speed; look by turn
  speed). A zero `Intent` leaves the pawn still.
- **Control (unit, pure):** map a synthetic `PlayerInput` snapshot through the control system and
  assert the produced `Intent`; assert an **all-zeros (headless) `Veng::Input`** produces a zero
  `Intent` and no motion.
- **AI uniformity:** a synthetic system writing `Intent` directly (no `PlayerInput`/possession)
  drives the same movement result — proving the producer is interchangeable.
- **Independence:** moving a possessed pawn does not change any `Viewer`'s resolved camera.

## Verification

- Clean build; `ctest` green across the bands, including the new control/movement tests.
- The no-device cooker test stays green (all new components are GPU-free).
- `hello_triangle_launcher_smoke` exits 0 and writes a correct-sized PPM — **the pawn does not
  move in smoke** (no `Update` tick, and the headless `Input` reads all-zeros regardless).
- `smoke_golden` does **not** move (the demonstration is windowed-only; the smoke pose is pinned).
- Windowed manual check: the pawn responds to WASD/look at the authored speed.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
