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

The fixed `Move`/`Look`/`Buttons` fields are gone. **The wire/serialized form is a fixed positional
vector in the active context's declared action order** — the `ActionState.Actions` vector is kept in
that schema order (Plan 02's context declares the order; a single active context is the common case),
so a game-defined action set costs no more serialized width than the old fixed struct and the net
layer replicates `[value, value, …]` positionally. The reflection serializer already handles the
`vector<ActionSample>` as a `FieldClass::Array`; the positional-schema guarantee is a resolution
invariant, documented on the component.

### 2. `InputContextStack` — the per-seat active contexts

```cpp
/// @brief The ordered active input-mapping contexts for a seat, highest priority last.
///
/// InputMappingSystem resolves these against the raw snapshot into the seat's PlayerInput.
/// Gameplay systems push/pop entries to switch schemes (enter a vehicle, open a modal). The
/// fine-grained, per-seat sibling of the InputRouter's coarse focus stack.
struct InputContextStack      // VE_REFLECT (FieldClass::Array of AssetHandle<InputMappingContext>)
{
    vector<AssetHandle<InputMappingContext>> Active;
};
```

In Plan 01 the handle type is a forward-declared placeholder resolved to a real `AssetType::InputMap`
asset in Plan 02; the sample seeds this stack from an in-C++ `ResolvedContext` this plan, then from a
loaded asset in Plan 02. (Concretely: Plan 01 lets a seat carry a resolver-ready context directly so
the system is exercised before the asset exists; Plan 02 swaps the source to `AssetHandle`.)

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
            input.State = ResolveActions(ResolveStack(stack), raw);
        });
}
```

- It is the **sole reader of raw device state** — no other system or game code polls `Veng::Input`
  for gameplay. The thin `RawInput` adapter implements Plan 00's `RawInputView` over
  `SystemContext.Input` (the previous-tick state it needs for phase detection is tracked by the
  adapter/system across ticks).
- It runs for **locally-owned** seats only. `IsLocallyOwned` is a small helper over the seat's
  `Authority` (a `Local`/`Server`-owned-by-this-client seat) — until the net layer exists every seat
  is local, so the guard is a no-op passthrough today, threaded now because it is the seam the net
  layer keys on (like `Authority` itself in planset-29).
- **Headless:** `ctx.Input` is neutral, so `ResolveActions` yields all-`None` and the pawn stays put
  with no guard — the same contract the old control system relied on.

### 4. hello-triangle migration

- **Delete** the `Key::W/S/A/D/Space` polling and the `PlayerInput.{Move,Look,Buttons}` fill in the
  game's control system.
- Declare the game's action ids (`namespace Actions { constexpr ActionId Move{…}, Look{…}, Jump{…}; }`).
- Seed each seat with an `InputContextStack` carrying an in-C++ context (WASD → `Move`, mouse →
  `Look`, Space → `Jump`) — Plan 02 replaces this with a cooked-asset handle.
- The game's control system shrinks to the **`PlayerInput → Intent` policy only**: read named actions
  (`input.GetAxis` / `input.WasTriggered`) and write the possessed pawn's `Intent` (the existing
  `MapInputToIntent` body, its input side now action-named). `MovementSystem` and everything below are
  untouched.
- The gameplay-focus toggle (`InputRouter::PushFocus(Gameplay)`, the Escape release) is unchanged —
  it is router-level focus, orthogonal to the action layer.

## Notes & constraints

- **`Intent` and `MovementSystem` are untouched** — the migration is entirely above the `Intent`
  write. The proof that the layering is right: the diff touches only how `PlayerInput` is *filled*,
  not what reads `Intent`.
- **Ordering.** `InputMappingSystem` must precede the control system in the level's system set. As a
  builtin registered first it leads the default order; a level naming an explicit ordered set lists
  it before its control system (documented in Plan 04's guide).
- **Reflection round-trip.** `PlayerInput` now serializes its `ActionState`; the existing prefab
  round-trip covers it (a seat's `PlayerInput` is runtime state, usually not authored, but the
  serializer must handle the new shape). Confirm a prefab carrying a `PlayerInput`/`InputContextStack`
  round-trips.

## Files (sketch)

- `engine/include/Veng/Scene/Components.h` — `PlayerInput` reshaped; `InputContextStack` added.
- `engine/include/Veng/Scene/InputMappingSystem.h` + `engine/src/Scene/InputMappingSystem.cpp` — the
  system + the `RawInput` adapter + `IsLocallyOwned`.
- `engine/src/Scene/BuiltinSystems.cpp` — register `InputMappingSystem` first.
- `examples/hello-triangle/main.cpp` — the migration above.

## Verification

- **`hello_triangle-launcher` under `HT_SMOKE`** writes a correct-sized PPM and the launcher smoke
  passes — the fixed smoke pose does not depend on live input (headless → neutral → no motion), so
  `smoke_golden` does not move.
- **Manual windowed run** (`/run` or a dev launch): WASD moves the pawn and Space acts, driven
  through named actions with zero `Key::` in game gameplay code.
- **A `unit` prefab round-trip** over a `PlayerInput` + `InputContextStack` entity.
- Clean build (`-j 8` on the main thread), full `ctest` green, plus the validation gate in
  `build-debug` (a reshaped component + new system must not trip a non-exhaustive switch under
  `-Werror`).

## Dependencies

Plan 00 (the types + resolve core). Independently verifiable before Plan 02 via the in-C++ context.
