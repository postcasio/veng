# Authoring input actions

This guide covers the **action-mapping layer**: how a game stops reading raw keys
and instead binds device inputs to **named actions** through cooked, remappable
data. It follows the [Input → Intent → Movement](writing-gameplay-systems.md#3-the-input--intent--movement-pattern)
pattern — this is the story of the *first* stage, where raw device state becomes a
game's `PlayerInput`.

The live reference is the [hello-triangle](../../examples/hello-triangle/) module:
its `Actions` constants and `ControlSystem` are in
[`main.cpp`](../../examples/hello-triangle/main.cpp), its bindings in
[`assets/input/gameplay.inputmap.json`](../../examples/hello-triangle/assets/input/gameplay.inputmap.json),
and the seat that activates them in
[`assets/prefabs/player.prefab.json`](../../examples/hello-triangle/assets/prefabs/player.prefab.json).
Open them beside this guide.

---

## The shape of it

```
raw device (Veng::Input) ─► InputMappingSystem ─► PlayerInput ─► control system ─► Intent ─► gameplay
                            (resolves bindings,     (resolved       (reads actions   (abstract  (movement,
                             the ONLY raw reader)    actions)         by name)         command)   rules)
```

Four things you author, one thing the engine does:

1. **Action-id constants** — a C++ constant per action (`Actions::Jump`), the stable
   identity a control system references.
2. **A `*.inputmap.json`** — the actions a scheme declares plus the raw-source →
   action bindings, cooked into an `InputMappingContext` asset.
3. **An `InputContextStack` on the player seat** — the ordered active contexts,
   referenced by `AssetId` from the player prefab.
4. **A control system** — reads the resolved actions by name off `PlayerInput` and
   writes an `Intent`.

The engine's builtin **`InputMappingSystem`** does the resolving: it is the *only*
reader of raw device state, and each tick it resolves every seat's active contexts
against the raw snapshot into that seat's `PlayerInput`.

**Gameplay never reads actions.** Movement and rule systems read `Intent`; only the
control system reads actions. That is what keeps AI and remote players drop-in
`Intent` producers that never touch the action layer.

---

## 1. Declare the action-id constants

An action's *meaning* is a C++ constant a control system references; an action
*exists* by being declared in a context (there is no registry). Mint one `ActionId`
per action with `vengc generate-id` and hardcode it as an uppercase-hex `0x…ULL`
literal, exactly as you would an `AssetId` or a `TypeId`:

```cpp
// The game's named input actions. Each is a minted ActionId a control system
// references and a binding context targets. These constants match the ids the
// cooked gameplay.inputmap asset declares.
namespace Actions
{
    constexpr ActionId Move{0x74080D78CF763EC4ULL};   // strafe (x) / advance (y)
    constexpr ActionId Look{0x6DB6F4088653942DULL};   // mouse delta: x yaw, y pitch
    constexpr ActionId Jump{0xB64A2DFE34C4E523ULL};
}
```

An `ActionId` is a `u64` leaf (`ActionId::Null` is the reserved empty id). The
constant is C++; the *bindings* for these actions are data (step 2), and the two
sides agree only by the id.

---

## 2. Write the `*.inputmap.json`

An `InputMappingContext` (`AssetType::InputMap`) declares its **actions** (id +
name + kind) and its **bindings** (raw source → action). It is an ordinary cooked
asset: a `*.inputmap.json` source the `InputMapImporter` validates and cooks, loaded
at runtime by `AssetId`. hello-triangle's
[`gameplay.inputmap.json`](../../examples/hello-triangle/assets/input/gameplay.inputmap.json):

```json
{
  "actions": [
    { "id": "0x74080D78CF763EC4", "name": "Move", "kind": "Axis2D" },
    { "id": "0x6DB6F4088653942D", "name": "Look", "kind": "Axis2D" },
    { "id": "0xB64A2DFE34C4E523", "name": "Jump", "kind": "Button" }
  ],
  "bindings": [
    { "source": { "device": "Keyboard",  "control": 68 }, "action": "0x74080D78CF763EC4", "axis": "X", "scale":  1.0 },
    { "source": { "device": "Keyboard",  "control": 65 }, "action": "0x74080D78CF763EC4", "axis": "X", "scale": -1.0 },
    { "source": { "device": "Keyboard",  "control": 87 }, "action": "0x74080D78CF763EC4", "axis": "Y", "scale":  1.0 },
    { "source": { "device": "Keyboard",  "control": 83 }, "action": "0x74080D78CF763EC4", "axis": "Y", "scale": -1.0 },
    { "source": { "device": "MouseAxis", "control": 0  }, "action": "0x6DB6F4088653942D", "axis": "X", "scale":  1.0 },
    { "source": { "device": "MouseAxis", "control": 1  }, "action": "0x6DB6F4088653942D", "axis": "Y", "scale":  1.0 },
    { "source": { "device": "Keyboard",  "control": 32 }, "action": "0xB64A2DFE34C4E523", "axis": "Whole" }
  ]
}
```

- **`kind`** is the action's value shape: `Button` (value x ∈ {0,1}), `Axis1D`
  (value x), or `Axis2D` (value xy).
- **`device`** is `Keyboard` / `MouseButton` / `MouseAxis` (live now), or
  `GamepadButton` / `GamepadAxis` (see [Gamepad](#gamepad-sources-are-inert-for-now)).
  `control` is the code interpreted per device — a `Key` value for a keyboard
  (`68` is `D`, `65` is `A`, `87` is `W`, `83` is `S`, `32` is `Space`), the mouse
  delta axis for `MouseAxis` (`0` = horizontal, `1` = vertical).
- **`axis`** picks which component of a vector action a scalar source drives:
  `X`, `Y`, or `Whole` (a native axis or a button drives the action directly).
  Four scalar keyboard bindings — two on `X`, two on `Y` — combine into the one 2D
  `Move` action.
- **`scale`** is a signed multiplier applied before accumulation; a negative
  `scale` inverts (so `A` on `X` with `-1.0` opposes `D` with `+1.0`). There is no
  separate invert flag.

The cook **validates every binding against the context's declared actions**: a
binding naming an action the context does not declare is a located cook error (the
typo-catch a global registry would otherwise miss), as is a duplicate action id or
an unknown device/axis/kind name. Add the source to the asset pack manifest like
any other asset:

```json
{ "id": "0xE65128F84910FBB9", "type": "InputMap", "source": "input/gameplay.inputmap.json" }
```

The bindings are **data**, so retargeting `Jump` from Space to Enter, or adding a
gamepad binding later, is a JSON edit and a recook — no C++ change.

---

## 3. Reference the context from the player seat

A seat carries an **`InputContextStack`** component holding the ordered active
contexts (highest priority last), each a cooked `InputMappingContext` referenced by
`AssetId`. Author it on the **player prefab**, on the same entity that carries the
`Viewer` seat and its `PlayerInput`. From hello-triangle's
[`player.prefab.json`](../../examples/hello-triangle/assets/prefabs/player.prefab.json):

```json
"::Veng::Viewer": { "Camera": 0 },
"::Veng::PlayerInput": {},
"::Veng::InputContextStack": {
  "Active": [ 16596091148679838649 ]
},
"::Veng::Possesses": { "Pawn": 2 }
```

The `Active` list references the `gameplay.inputmap` asset by id — it resolves as
an ordinary load-time prefab dependency, so a seat's base scheme is authored data.
A not-yet-resident context contributes no actions until it streams in.

**The stack switches schemes and gates focus.** Gameplay systems push and pop
entries to change the active scheme — enter a vehicle → push a `vehicle` context,
open a modal → push a UI context — and a higher-priority context that binds an
action shadows a lower one's bindings of that same action entirely. Popping the
gameplay context down to empty **neutralizes input**: with no active context the
seat resolves to all-`None` actions. hello-triangle uses exactly that to release
control when the window loses gameplay focus, suspending the seat's contexts and
restoring them when focus returns:

```cpp
world->Each<Viewer, InputContextStack>(
    [&](const Entity seat, Viewer&, InputContextStack& stack)
    {
        if (!focused && !stack.Active.empty())
        {
            m_SuspendedContexts[seat] = std::move(stack.Active);
            stack.Active.clear();          // no active context → neutral input
        }
        else if (focused && stack.Active.empty())
        {
            // restore the suspended contexts on regaining focus
        }
    });
```

`InputContextStack` is the fine-grained, per-seat sibling of the `InputRouter`'s
coarse focus stack: the router decides *whether the game owns input at all*, the
context stack decides *which scheme* an owning seat resolves.

---

## 4. Read actions in a control system

The control system reads the resolved actions **by name** off the seat's
`PlayerInput` and maps them to the pawn's `Intent`. `PlayerInput` *is* the resolved
action snapshot — its `Get*`/`Was*` helpers read an action by id:

- `GetValue(id)` → the resolved `vec2` value (button x ∈ {0,1}, `Axis1D` x,
  `Axis2D` xy).
- `GetAxis(id)` → the x component, the 1D convenience.
- `IsHeld(id)` → active this tick (`Started` or `Ongoing`).
- `WasTriggered(id)` → became active this tick (`Started`).
- `WasReleased(id)` → released this tick (`Completed`).

The mapping is a pure function — the same action state always yields the same
`Intent`, whether the actions came from the device, a recording, or the wire — so it
is unit-testable without an `Input` or a scene:

```cpp
Intent MapInputToIntent(const PlayerInput& input)
{
    constexpr f32 YawSensitivity = 0.05f;
    const vec2 move = input.GetValue(Actions::Move);
    const vec2 look = input.GetValue(Actions::Look);

    Intent intent;
    intent.Move = vec3(move.x, 0.0f, -move.y);
    intent.Look = vec2(-look.x * YawSensitivity, 0.0f);
    intent.Actions = input.IsHeld(Actions::Jump) ? 1u : 0u;
    return intent;
}
```

The control system itself is `Phase::Sim`. It reads **no raw device state** — it
consults only the resolved `PlayerInput` the engine already filled — so in headless
the resolved actions are all-`None`, it produces a zero `Intent`, and the pawn stays
put, with no null to guard:

```cpp
class ControlSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32, const SystemContext&) override
    {
        scene.Each<PlayerInput, Possesses>(
            [&](const Entity seat, PlayerInput& player, Possesses& possesses)
            {
                // ... reads player.GetValue(Actions::Look) for the camera pitch ...

                if (possesses.Pawn == Entity::Null || !scene.IsAlive(possesses.Pawn) ||
                    !scene.Has<Intent>(possesses.Pawn))
                {
                    return;   // an unwired seat is inert
                }
                scene.Get<Intent>(possesses.Pawn) = MapInputToIntent(player);
            });
    }
};

VE_SYSTEM(ControlSystem, 0x1C2F5C03357C19B2ULL, "Control");
```

Because the control system reads a resolved snapshot rather than the device, it is
the only place actions enter gameplay — everything downstream reads the `Intent` it
writes.

---

## 5. Order `InputMappingSystem` before the control system

This is the one ordering rule that will bite you if you miss it.

`InputMappingSystem` is a **builtin Sim system**; the host pre-registers it (a game
names it, never re-declares it). It fills `PlayerInput`, so it **must run before**
the control system that reads `PlayerInput`. A level's `systems` list is an
**explicit order — registration order does not reorder it** — so you must place
`InputMappingSystem` ahead of your control system *in the level's `systems` array*.
hello-triangle's [level](../../examples/hello-triangle/assets/levels/sample.level.json)
lists it (id `8866966906423916917`) at position 1, before its `ControlSystem`
(id `2030943125819365810`) at position 2:

```json
"systems": [
  8128120177403478945,    // SpawnPlayerRule
  8866966906423916917,    // InputMappingSystem  ← fills PlayerInput
  2030943125819365810,    // ControlSystem       ← reads PlayerInput
  ...
]
```

In the level editor, enable **Input Mapping** and drag it above your control system
in the systems panel. Get the order wrong and the control system reads *last tick's*
`PlayerInput` — input lags a frame, or (before the first tick) reads empty.

`InputMappingSystem` runs for **locally-owned seats only** (every seat today; the
seam the net layer will key on) and is device-driven, so it never runs in a bare
`Scene` with no `(Viewer, InputContextStack, PlayerInput)` seat — a world that takes
no player input (the minimal template's spinning cube) resolves nothing and needs
none of this.

---

## Gamepad sources are inert for now

The binding vocabulary carries `GamepadButton` / `GamepadAxis` source arms so a
`*.inputmap.json` can already name them, but **`Veng::Input` has no gamepad state
yet**: the resolver reads a gamepad source as neutral (zero), so a gamepad binding
is inert until the device layer lands. That layer — filling `Veng::Input` with pad
state and fanning devices per seat — is a future direction, alongside multi-seat input
routing. Author keyboard/mouse bindings today; gamepad bindings are forward-ready
but do nothing yet.

---

## The editor

The **`InputMappingEditorPanel`** (registered for `AssetType::InputMap`) opens a
`*.inputmap.json` and draws its actions + bindings through the reflection inspector,
so the binding table is add/remove/edit-able with no bespoke widget code. It shows
each binding's action by name (an `ActionId` combo scoped to the document's declared
actions), recooks live behind a stable handle, and resolves the document against the
editor's own input each frame so a binding's effect is observable without launching
the game. It is deliberately **basic** — no press-a-key-to-bind capture, no
drag-reorder — because there is no data consumer of actions yet (everything is C++);
that investment waits until a runtime remapping screen or visual scripting earns it.

---

## Where to go next

- **[Writing gameplay systems](writing-gameplay-systems.md)** — the full
  Input → Intent → Movement pattern this guide's first stage feeds, plus phases,
  config-via-components, and registering + wiring systems.
- **[Wiring a level](wiring-a-level.md)** — the `Level` asset, the ordered system
  set, and the load-to-play flow.
- The generated API reference (`cmake --build build --target docs`) documents every
  type named here — `ActionId`, `InputMappingContext`, `InputContextStack`,
  `PlayerInput`, `InputMappingSystem`, `ResolveActions` — in full.
