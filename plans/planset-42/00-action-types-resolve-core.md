# Plan 00 — action types + the pure resolve core

**Goal:** land the vocabulary of the action-mapping layer and the one piece of real logic in it — a
**device-free, pure** function that turns a set of active bindings plus a raw input snapshot into a
resolved set of named-action values. No `Scene`, no `Window`, no asset, no GPU: just types and a
function, fully unit-tested with no ICD, the way `DecideBarrier` / `ComputeCascades` foundation-first
work always begins in veng. Nothing consumes it yet — Plan 01 wires it into a system.

## The starting point

- `Veng::Input` (`Veng/Input.h`) is an event-fed **snapshot** (planset-30): `IsKeyDown(Key)`,
  `WasKeyPressed(Key)`, mouse position/delta, and mouse buttons — **keyboard and mouse only; there is
  no gamepad surface yet**. It reports neutral all-zeros in headless. This plan does **not** read it;
  it defines the pure function that a *later* system feeds a snapshot into. The snapshot's read
  surface is the raw-state input to `ResolveActions`.
- The reflection layer supplies the authoring macros this plan uses: `VE_LEAF(Type, 0x…ULL,
  FieldClass::Kind)` for a leaf/enum identity, `VE_ENUM` for `{name, value}` enum reflection, and
  `VE_REFLECT`/`VE_FIELD` for a reflected struct (`Veng/Reflection/`, engine/CLAUDE.md). The types
  below are reflected so Plan 02's asset cooks/loads and Plan 03's editor draws them for free.
- Ids are minted `u64`s authored like `AssetId`/`TypeId`/`SystemId` — hex in C++ (`0x…ULL`), decimal
  in JSON, minted with `vengc generate-id`. `ActionId` is the newest member of that family.

## What lands

A new engine home, `Veng/Input/Actions.h` (+ `engine/src/Input/Actions.cpp` for the resolve walk).
All types are `Veng`-namespaced and reflected.

### 1. The id and the action declaration

```cpp
/// @brief Stable identity of a named input action, authored like an AssetId.
enum class ActionId : u64 { Null = 0 };   // VE_LEAF(FieldClass::Scalar) — minted per game action

/// @brief The value shape a resolved action carries.
enum class ActionKind : u32 { Button, Axis1D, Axis2D };   // VE_ENUM

/// @brief One action a context declares: its id, display name, and value shape.
struct InputAction            // VE_REFLECT
{
    ActionId Id = ActionId::Null;
    string   Name;            // display/authoring label; on-disk identity is Id
    ActionKind Kind = ActionKind::Button;
};
```

A game declares its action ids as C++ constants (`namespace Actions { constexpr ActionId Move{0x…};
constexpr ActionId Jump{0x…}; }`) and references those ids from both its control system and its
binding JSON — the `AssetId` pattern exactly. There is **no** registry; an action "exists" by being
declared in a context (Plan 02 validates bindings against a context's `InputAction` list).

### 2. The binding

```cpp
/// @brief Which raw control a binding reads.
///
/// The Gamepad* arms are **forward vocabulary**: Veng::Input carries no gamepad state yet, so the
/// resolver reads them as neutral (Plan 01's adapter returns zero for a gamepad source) until the
/// device layer lands next planset. Keyboard/mouse are live now.
enum class InputDeviceType : u32 { Keyboard, MouseButton, MouseAxis, GamepadButton, GamepadAxis }; // VE_ENUM

/// @brief A raw control reference: a device kind + a control code interpreted per device.
struct InputSource            // VE_REFLECT
{
    InputDeviceType Device = InputDeviceType::Keyboard;
    u32 Control = 0;          // Key code / mouse-button / mouse-axis / gamepad button|axis index
};

/// @brief Which component of a vector action a scalar source drives.
enum class AxisComponent : u32 { Whole, X, Y };   // VE_ENUM — Whole = a native axis drives the action directly

/// @brief One raw-source → action mapping with the minimal modifiers.
struct Binding                // VE_REFLECT
{
    InputSource  Source;
    ActionId     Action = ActionId::Null;
    AxisComponent Axis = AxisComponent::Whole;   // a button → an axis component (WASD → Move.xy)
    f32          Scale = 1.0f;                    // sign/scale (S = −Y via Scale −1 on Axis Y)
};
```

This is deliberately the *minimal* modifier set — a scalar source contributes to one action
component with a signed scale (a negative `Scale` covers inversion, so there is no separate
`Invert` flag); a native axis (a stick, `AxisComponent::Whole`) drives its action directly. It
covers WASD → a 2D `Move` action and a stick → the same action without the full Unreal-style
swizzle/curve/dead-zone-shape modifier zoo (deferred; see the README).

### 3. The resolved snapshot and its phase

```cpp
/// @brief How an action's activation changed this tick.
enum class ActionPhase : u32 { None, Started, Ongoing, Completed };   // VE_ENUM
// Started = became active this tick; Ongoing = still active; Completed = released this tick.

/// @brief One resolved action this tick.
struct ActionSample           // VE_REFLECT
{
    ActionId    Id = ActionId::Null;
    vec2        Value{0.0f};   // button → x∈{0,1}; Axis1D → x; Axis2D → xy
    ActionPhase Phase = ActionPhase::None;
};

/// @brief The resolved action set for one seat this tick.
///
/// Plan 01 makes this the storage of the PlayerInput component. Carries one sample per action
/// declared across the active contexts, in the deterministic stack-declared order the resolution
/// contract below defines (so the same active stack yields the same layout run to run).
struct ActionState            // VE_REFLECT (FieldClass::Array of ActionSample)
{
    vector<ActionSample> Actions;

    /// @brief Resolved value of an action, or zero if the action is not present.
    vec2 GetValue(ActionId) const;
    f32  GetAxis(ActionId) const;          // .x convenience
    bool IsHeld(ActionId) const;           // Started || Ongoing
    bool WasTriggered(ActionId) const;     // Started
    bool WasReleased(ActionId) const;      // Completed
};
```

The `Get*`/`Was*` helpers are the surface a control system reads (`state.WasTriggered(Actions::Jump)`).

### 4. The pure resolve function

```cpp
/// @brief The raw-input read surface the resolver needs, satisfied by Veng::Input.
///
/// A tiny read-only interface so the resolver is testable with a fake and never links the
/// windowing snapshot. It reports only *this tick's* state — phase comes from the previous
/// ActionState threaded into ResolveActions, not from a previous-raw query — so there is no
/// *Prev accessor. Veng::Input satisfies it (Plan 01 adapts it).
struct RawInputView
{
    virtual bool  IsKeyDown(u32 code) const = 0;
    virtual bool  IsButtonDown(InputDeviceType, u32 code) const = 0;
    virtual f32   GetAxis(InputDeviceType, u32 code) const = 0;
};

/// @brief Resolve active bindings against a raw snapshot into the seat's action state.
///
/// Pure: same inputs → same output, no device/GPU/scene access. Higher-priority contexts
/// (later in `active`) override a lower one's binding of the same action. Combines a 2D
/// action's component bindings, applies each binding's signed scale, and derives each action's
/// phase by comparing this tick's activation against `previous` — the seat's ActionState from
/// last tick — so phase needs no stateful adapter and works for axis actions (whose activation
/// is the resolved value, not any single raw source).
ActionState ResolveActions(std::span<const ResolvedContext> active, const RawInputView& raw,
                           const ActionState& previous);
```

`ResolvedContext` is the in-memory, load-resolved form of an `InputMappingContext` (the declared
`InputAction`s + `Binding`s already validated) — Plan 02 produces it from the cooked asset; this
plan defines it and a hand-built constructor for tests.

**Resolution contract** (the unit-tested rules):
- **Action set & order:** the result carries **one sample per action declared across the active
  contexts**, in stack order — each context's declared actions in declaration order, an action
  declared in more than one context keeping its *first* position. An action with no active binding
  still gets a sample (zero value, `None` phase). So the sample set and its order are a deterministic
  function of the active stack, fixed until the stack changes.
- An action's value is accumulated from every binding targeting it: a `Whole` axis source sets the
  value directly; an `X`/`Y` button/axis source adds `Scale · sourceValue` into that component. A
  digital source contributes `1.0` while down (a negative `Scale` inverts).
- **Context priority:** contexts later in `active` win — a higher context that binds an action
  shadows a lower context's bindings of the *same* action entirely (not per-binding-merged), so a
  `vehicle` context can fully rebind `Move` while leaving unrelated actions falling through to the
  base context.
- **Phase (from `previous`):** an action is *active* this tick when its resolved `|value| > 0`
  (for an axis action, the dead-zone threshold is a later modifier). Comparing against the same
  action's activation in `previous`: `Started` when active now and not in `previous`, `Completed`
  when the reverse, `Ongoing` while continuously active, `None` while continuously inactive. An
  action absent from `previous` (a newly declared/pushed context's action) is treated as inactive
  last tick.
- A neutral raw snapshot (headless) yields every action `None` with zero value.

## Notes & constraints

- **No dependency added.** The types are reflected (existing macros) and the function is glm-only;
  `Veng/Input/Actions.h` pulls in nothing a public header could not already. It sits in `libveng`.
- **`RawInputView` keeps the core device-free.** The resolver takes an abstract read surface, so the
  unit test drives it with a scripted fake and never opens a window; Plan 01 writes the thin
  `Veng::Input` adapter.
- **Placeholder ids while implementing.** The types carry placeholder `ActionId`/leaf `TypeId`
  literals during the build; mint the real ones with `vengc generate-id` once green and replace, per
  the working norms.

## Files (sketch)

- `engine/include/Veng/Input/Actions.h` — the types above + `ResolveActions` declaration + the
  reflect-macro blocks.
- `engine/src/Input/Actions.cpp` — `ResolveActions` + the `ActionState` accessors.
- `tests/unit/…` — a new `action_resolve` suite.

## Verification

- **`ctest -R action_resolve`** — the pure resolve tests, no ICD: WASD → a `Move` 2D action (each
  key drives the right component with the right sign; diagonals sum), a whole-axis source → a 1D axis
  action (the `AxisComponent::Whole` path), a button → `Jump` with `Started`/`Ongoing`/`Completed`
  across scripted tick sequences (threading
  each tick's result back as `previous`), context priority (a higher context rebinds `Move` and
  leaves `Jump` falling through), the deterministic stack-declared sample order, an unbound declared
  action still producing a `None` sample, and the neutral snapshot → all-`None`.
- **`include_hygiene`** compiles the new public header while linking only PUBLIC deps — proves
  `Veng/Input/Actions.h` leaks no backend include.
- Clean build, full `ctest` green. No render change — no golden touched.

## Dependencies

None beyond the reflection macros. Foundation for Plans 01–03.
