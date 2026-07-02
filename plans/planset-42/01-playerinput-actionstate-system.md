# Plan 01 — `PlayerInput` becomes `ActionState` + the `InputMappingSystem`

**Goal:** wire the pure core into the running world. Repurpose the `PlayerInput` component to hold
the resolved `ActionState`, add the per-seat `InputContextStack` component, add the builtin
**`InputMappingSystem`** that resolves a seat's active contexts against the raw input snapshot each
tick, and migrate hello-triangle off `Key::` polling — its control system keeps only the
game-specific `PlayerInput → Intent` policy, now reading actions by name. After this plan the sample
runs on named actions with an **in-C++** context (Plan 02 makes the context a cooked asset). Depends
on Plan 00.

## The starting point

- `PlayerInput` (`Veng/Scene/Components.h`) is today a fixed `{ vec3 Move; vec2 Look; u32 Buttons }`
  snapshot filled by a game control system. hello-triangle's control system
  ([examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp)) polls
  `input.IsKeyDown(Key::W/S/A/D/Space)` into move/look/buttons, writes each seat's `PlayerInput` via
  `scene.Each<PlayerInput, Possesses>`, then maps `PlayerInput → Intent` through a pure
  `MapInputToIntent` and writes the possessed pawn's `Intent`.
- `SceneSystem` declares a `Phase { Sim, View }`; `SceneSimulation` runs all Sim systems then all
  View systems each tick. `SystemContext.Input` is the always-present raw snapshot (neutral in
  headless). Systems are a catalog: `VE_SYSTEM(Type, 0x…ULL, "Name")` + `RegisterBuiltinSystems`
  (`MovementSystem`, `CameraRigSystem`, `RootMotionDriveSystem`, `AnimationSystem`,
  `ConstantMotionSystem`).
- `Viewer` (seat → camera) and `Possesses` (seat → pawn) mark seats; `Intent` is the pawn-space
  chokepoint `MovementSystem` consumes.

## What lands

### 1. `PlayerInput` becomes the `ActionState`

```cpp
/// @brief Per-seat resolved input for this tick — the serializable action snapshot.
///
/// The resolved values of the seat's active actions this tick (see Veng/Input/Actions.h),
/// produced by InputMappingSystem from the seat's InputContextStack. It is the control
/// chokepoint a net layer replicates and the uniform surface a control system reads by
/// action id — filled locally for an owned seat, from the wire for a remote one.
struct PlayerInput            // VE_REFLECT (its ActionState field is FieldClass::Array)
{
    ActionState State;
    // read-through convenience so a control system writes input.WasTriggered(Actions::Jump):
    vec2 GetValue(ActionId id) const { return State.GetValue(id); }
    f32  GetAxis(ActionId id) const  { return State.GetAxis(id); }
    bool IsHeld(ActionId id) const   { return State.IsHeld(id); }
    bool WasTriggered(ActionId id) const { return State.WasTriggered(id); }
    bool WasReleased(ActionId id) const  { return State.WasReleased(id); }
};
```

The fixed `Move`/`Look`/`Buttons` fields are gone. `PlayerInput` **serializes through the reflection
serializer's name-keyed `FieldClass::Array` encoding** for its `ActionState` — each `ActionSample`
self-describing by its `ActionId` — the same cook/load/replicate path every reflected component
takes, **no bespoke format**. The `Actions` set is kept in the deterministic stack-declared order
Plan 00 defines, so two runs of the same active stack produce the same sample layout — but that
ordering is a **resolution invariant for stable diffing/preview**, not a wire-width optimization. A
tighter positional/bit-packed net encoding (schema keyed on the active context) is deferred to the
networking planset behind this stable shape. The five `Get*`/`Was*` methods delegate to `State` — a
convenience wrapper, not a second snapshot type.

### 2. `InputContextStack` — the per-seat active contexts

```cpp
/// @brief The ordered active input contexts for a seat, highest priority last.
///
/// InputMappingSystem resolves these against the raw snapshot into the seat's PlayerInput.
/// Gameplay systems push/pop entries to switch schemes (enter a vehicle, open a modal). The
/// fine-grained, per-seat sibling of the InputRouter's coarse focus stack.
struct InputContextStack      // Plan 01: runtime-only scratch; Plan 02 makes it a reflected asset array
{
    vector<ResolvedContext> Active;   // Plan 02 → vector<AssetHandle<InputMappingContext>>
};
```

In Plan 01 the stack holds resolver-ready `ResolvedContext`s **directly**, seeded in C++ — it is
**runtime-only seat scratch, not serialized or reflected** — so the system is exercised before the
`AssetType::InputMap` asset exists. Plan 02 reshapes `Active` to a reflected
`vector<AssetHandle<InputMappingContext>>` and moves the reflected/prefab round-trip there; the
resolver is unaffected (it consumes `std::span<const ResolvedContext>` either way, via
`InputMappingContext::GetResolved()` in Plan 02).

### 3. `InputMappingSystem` — the builtin resolver

A new builtin `SceneSystem` (`Veng/Scene/InputMappingSystem.h`, `VE_SYSTEM(…, "Input Mapping")`),
`Phase::Sim`, **registered first** in `RegisterBuiltinSystems` so it runs ahead of any control system:

```cpp
void InputMappingSystem::OnUpdate(Scene& scene, f32 delta, const SystemContext& ctx)
{
    const RawInput raw{ctx.Input};                    // the thin Veng::Input → RawInputView adapter
    scene.Each<Viewer, InputContextStack, PlayerInput>(
        [&](Entity seat, Viewer&, InputContextStack& stack, PlayerInput& input)
        {
            if (!IsLocallyOwned(scene, seat)) { return; }   // remote/AI seats: PlayerInput is externally supplied
            input.State = ResolveActions(ResolveStack(stack), raw, input.State);   // prev = last tick
        });
}
```

- It is the **sole reader of raw device state** — no other system or game code polls `Veng::Input`
  for gameplay. The thin `RawInput` adapter implements Plan 00's `RawInputView` over
  `SystemContext.Input`, reading **only this tick's state** (a gamepad source reads neutral until the
  device layer lands); phase comes from the previous `PlayerInput.State` threaded into
  `ResolveActions`, so the system holds no cross-tick state of its own. The adapter lives in a
  **public `Veng/Input/` header** (not private to the .cpp) so Plan 03's editor preview reuses it.
- It runs for **locally-owned** seats only. `IsLocallyOwned(scene, seat)` is the seam the net layer
  keys on; **today it returns true for every seat** (no remote ownership exists yet), threaded now —
  like `Authority` in planset-29 — so the resolve-per-seat shape is right when replication lands. It
  does **not** gate on `Authority.Tier` yet (an authored seat is `Server`-tier; mapping tier/owner to
  "locally owned" is the net layer's call), so the guard changes no behavior in this planset.
- **Headless:** `ctx.Input` is neutral, so `ResolveActions` yields all-`None` and the pawn stays put
  with no guard — the same contract the old control system relied on.

### 4. hello-triangle migration

- **Delete** the `Key::W/S/A/D/Space` polling and the `PlayerInput.{Move,Look,Buttons}` fill in the
  game's control system.
- Declare the game's action ids (`namespace Actions { constexpr ActionId Move{…}, Look{…}, Jump{…}; }`).
- Seed each seat with an `InputContextStack` carrying an in-C++ `ResolvedContext` (WASD → `Move`,
  mouse → `Look`, Space → `Jump`) — Plan 02 replaces this with a cooked-asset handle.
- **Add `InputMappingSystem` to the level's run order.** `SceneSimulation` runs an explicit
  `SystemId` set *in the order it names* (registration order in `RegisterBuiltinSystems` does not
  reorder it), so insert `InputMappingSystem`'s minted `SystemId` into
  `assets/levels/sample.level.json`'s `systems` array **immediately before `Control`'s id**.
  Otherwise the resolver never runs and every action reads empty — silently.
- The game's control system shrinks to the **`PlayerInput → Intent` policy only**: read named actions
  (`input.GetValue` / `input.WasTriggered`) and write the possessed pawn's `Intent` (the existing
  `MapInputToIntent` body, its input side now action-named). `MovementSystem` and everything below are
  untouched.
- **Focus gates through the context stack, not a raw read.** Today the control system neutralizes
  input by gating every read on `input.IsMouseCaptured()` — that raw-read gate is gone with the raw
  reads. Instead the gameplay-focus toggle now **pushes the gameplay context onto the seat's
  `InputContextStack` when focus is captured and pops it on release** (Escape): an empty stack
  resolves to all-`None`, so the pawn and follow camera stay still while ImGui owns the mouse. This
  is the `InputContextStack` showcase and the correct home for the gate. (`InputRouter` focus itself
  is unchanged — it drives *whether* the toggle pushes/pops.)

## Notes & constraints

- **`Intent` and `MovementSystem` are untouched** — the migration is entirely above the `Intent`
  write. The proof that the layering is right: the diff touches only how `PlayerInput` is *filled*,
  not what reads `Intent`.
- **Ordering.** `InputMappingSystem` must precede the control system. For a level with an explicit
  `systems` order (hello-triangle) this means listing its id first, as the migration does above;
  registration order in `RegisterBuiltinSystems` only sets the "all registered" convenience order, not
  an authored level's. A mechanism to *force* mandatory-first builtins regardless of the authored
  order is a scheduling-model question recorded in Plan 04's roadmap pass, not built here.
- **Context-stack changes take effect next tick.** `InputMappingSystem` runs once, ordered first, so
  a Sim system that pushes/pops a seat's stack this tick is read on the *following* tick — a
  documented one-frame latency, not a bug.
- **Reflection round-trip.** In Plan 01 `InputContextStack` is runtime-only scratch (not reflected),
  so the round-trip here is over **`PlayerInput` alone** — confirm a prefab carrying a `PlayerInput`
  round-trips through the new `ActionState` shape. The `InputContextStack` round-trip lands in Plan 02,
  where the component becomes a reflected `AssetHandle` array.
- **Mint the ids.** The `Actions::Move/Look/Jump` `ActionId` constants and `InputMappingSystem`'s
  `SystemId` are placeholders during the build; mint them with `vengc generate-id` once green (hex in
  C++, decimal in the level JSON), per the working norms.

## Files (sketch)

- `engine/include/Veng/Scene/Components.h` — `PlayerInput` reshaped; `InputContextStack` added.
- `engine/include/Veng/Scene/InputMappingSystem.h` + `engine/src/Scene/InputMappingSystem.cpp` — the
  system + `IsLocallyOwned`.
- `engine/include/Veng/Input/RawInput.h` (+ `.cpp`) — the public `Veng::Input → RawInputView` adapter
  (public so Plan 03's preview reuses it, not private to the system).
- `engine/src/Scene/BuiltinSystems.cpp` — register `InputMappingSystem`.
- `examples/hello-triangle/main.cpp` — the migration above.
- `examples/hello-triangle/assets/levels/sample.level.json` — insert `InputMappingSystem`'s id into
  the `systems` order, before `Control`.
- `tests/unit/control_movement.cpp` — its hand-built `PlayerInput{.Move,.Look,.Buttons}` fixtures and
  its local `MapInputToIntent` mirror move to the action-named shape (build an `ActionState` of
  samples), matching the migrated control policy — otherwise this test fails to compile.

## Verification

- **`hello_triangle-launcher` under `HT_SMOKE`** writes a correct-sized PPM and the launcher smoke
  passes — the fixed smoke pose does not depend on live input (headless → neutral → no motion), so
  `smoke_golden` does not move.
- **A headless end-to-end `unit` test** drives a scripted `RawInputView` through `InputMappingSystem`
  (a seat carrying an in-C++ context) → `PlayerInput` → `MapInputToIntent`, asserting the produced
  `Intent` for a known input — the joined-up resolve→snapshot→intent path with no window (the pure
  `action_resolve` suite covers the resolver alone; this covers the wiring).
- **Manual windowed run** (`/run` or a dev launch): WASD moves the pawn and Space acts, driven
  through named actions with zero `Key::` in game gameplay code; **releasing focus (Escape) pops the
  context so the pawn stops** even as the mouse moves over the debug UI.
- **A `unit` prefab round-trip** over a `PlayerInput` entity (the reshaped `ActionState` shape).
- **`tests/unit/control_movement.cpp`** compiles and passes under the new shape.
- Clean build (`-j 8` on the main thread), full `ctest` green, plus the validation gate in
  `build-debug` (a reshaped component + new system must not trip a non-exhaustive switch under
  `-Werror`).

## Dependencies

Plan 00 (the types + resolve core). Independently verifiable before Plan 02 via the in-C++ context.
