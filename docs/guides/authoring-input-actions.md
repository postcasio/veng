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

An `InputMappingContext` (`AssetTypes::InputMap`) declares its **actions** (id +
name + kind) and its **bindings** (raw source → action). It is an ordinary cooked
asset: a `*.inputmap.json` source the `InputMapImporter` validates and cooks, loaded
at runtime by `AssetId`. hello-triangle's
[`gameplay.inputmap.json`](../../examples/hello-triangle/assets/input/gameplay.inputmap.json):

```json
{
  "Actions": [
    { "Id": "0x74080D78CF763EC4", "Name": "Move", "Kind": "Axis2D" },
    { "Id": "0x6DB6F4088653942D", "Name": "Look", "Kind": "Axis2D" },
    { "Id": "0xB64A2DFE34C4E523", "Name": "Jump", "Kind": "Button" }
  ],
  "Bindings": [
    { "Source": { "Device": "Keyboard",  "Control": 68 }, "Action": "0x74080D78CF763EC4", "Axis": "X", "Scale":  1.0 },
    { "Source": { "Device": "Keyboard",  "Control": 65 }, "Action": "0x74080D78CF763EC4", "Axis": "X", "Scale": -1.0 },
    { "Source": { "Device": "Keyboard",  "Control": 87 }, "Action": "0x74080D78CF763EC4", "Axis": "Y", "Scale":  1.0 },
    { "Source": { "Device": "Keyboard",  "Control": 83 }, "Action": "0x74080D78CF763EC4", "Axis": "Y", "Scale": -1.0 },
    { "Source": { "Device": "MouseAxis", "Control": 0  }, "Action": "0x6DB6F4088653942D", "Axis": "X", "Scale":  1.0 },
    { "Source": { "Device": "MouseAxis", "Control": 1  }, "Action": "0x6DB6F4088653942D", "Axis": "Y", "Scale":  1.0 },
    { "Source": { "Device": "Keyboard",  "Control": 32 }, "Action": "0xB64A2DFE34C4E523", "Axis": "Whole" }
  ]
}
```

The document *is* the reflected `InputMapData` (`Veng/Asset/InputMappingContext.h`): every key is a
field name of `InputMapData`, `InputAction`, `Binding` or `InputSource`, read through the shared
JSON walker, so an absent field takes its default and a key naming no field is a cook error.

- **`Kind`** is the action's value shape: `Button` (value x ∈ {0,1}), `Axis1D`
  (value x), or `Axis2D` (value xy).
- **`Device`** is `Keyboard` / `MouseButton` / `MouseAxis`, or
  `GamepadButton` / `GamepadAxis` (see [Gamepad sources](#gamepad-sources-and-shaping)).
  `Control` is the code interpreted per device — a `Key` value for a keyboard
  (`68` is `D`, `65` is `A`, `87` is `W`, `83` is `S`, `32` is `Space`), or a
  `MouseAxis` code: the pointer delta (`0` = horizontal, `1` = vertical), the
  viewport-local pointer position (`2` / `3`, seat-resolved only), or the
  scroll-wheel delta (`4` = horizontal, `5` = vertical). The named constants are
  `RawInput::MouseAxisX/Y`, `SeatInputView::MousePositionX/Y`, and
  `RawInput::MouseScrollX/Y`. A gamepad source's `Control` is a `GamepadButton` /
  `GamepadAxis` index.
- **`Axis`** picks which component of a vector action a scalar source drives:
  `X`, `Y`, or `Whole` (a native axis or a button drives the action directly).
  Four scalar keyboard bindings — two on `X`, two on `Y` — combine into the one 2D
  `Move` action.
- **`Scale`** is a signed multiplier applied first; a negative `Scale` inverts (so
  `A` on `X` with `-1.0` opposes `D` with `+1.0`). There is no separate invert flag.
- **`Threshold`** (default `0`) is the scaled value a source must reach. On a
  `Button` action the source is a **half-axis**: it presses when the scaled value is
  positive and at least `Threshold`, so a trigger becomes a button with a deliberate
  pull point, and a stick axis bound with `Scale -1` to one button and `+1` to another
  fires each only on its own half. A source at rest never presses, whatever the
  threshold. On an axis action, a source whose scaled magnitude is under `Threshold`
  contributes nothing, and above it the value passes through unrescaled.
- **`Exponent`** (default `1`) is an axis action's response curve,
  `sign(s)·|s|^Exponent` on the scaled value `s`, applied after the threshold: `2`
  maps a half deflection to a quarter, for fine aiming near centre. A `Button` action
  ignores it.
- **`Modifier`** (default `{ "Device": "None" }`, no modifier) makes the binding a
  **chord**, and **`ModifierThreshold`** (default `0.5`) is the value the modifier must
  reach; see [Chorded bindings](#chorded-bindings).

The cook **validates every binding against the context's declared actions**: a
binding naming an action the context does not declare is a located cook error (the
typo-catch a global registry would otherwise miss), as is a duplicate or null action
id, an unknown device/axis/kind name, a negative `Threshold`, an `Exponent` that is
not greater than 0, a `Source` with device `None`, a gamepad `Source` or `Modifier`
indexing past the last button or axis, a negative `ModifierThreshold`, or a key or
button modifier whose `ModifierThreshold` is over 1 (it would never be down). Add the source to the asset pack manifest like
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
seat resolves to all-`None` actions. Going quiet while the seat lacks gameplay focus
needs no stack surgery, though: a context authored `"RequiresGameplayFocus": true`
drops out of resolution whenever the seat is not gameplay-focused, which is how
hello-triangle's gameplay map stops reading the mouse while its debug UI has the cursor.

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

## Gamepad sources and shaping

A `GamepadButton` / `GamepadAxis` source reads the seat's assigned pad (see
[Multi-seat input](multi-seat-input.md)). A pad's sticks and triggers arrive already
**shaped by the device layer**: `Veng::Input` passes each stick's two axes through one
**radial deadzone** and each trigger through its own as it takes the pad in, rescaling
what is left so the value is continuous at the zone's edge and full at full
deflection. A resting stick therefore reads exactly zero — so a stick bound to a
button never holds it from noise, and a stick bound to an axis action completes
when released. The zone is radial because a per-axis zone would bend a diagonal onto
whichever axis cleared first; only the device layer sees both axes of a stick together.

The zones are engine defaults (`GamepadDeadzones::DefaultStick` 0.15 and
`DefaultTrigger` 0.05, conventional starting points rather than measurements), set per
pad with `Input::SetGamepadDeadzones(id, stick, trigger)` and reverting to the defaults
when the pad disconnects. Virtual pads are shaped the same way. The unshaped values stay
readable through `Input::GetRawGamepadAxis`, and the gamepad debug panel
(`Veng::UI::GamepadPanel`) shows them beside the shaped ones with live zone sliders. A
binding's own `Threshold` and `Exponent` then shape the zoned value per binding, so an
input map authors the pull point and the response curve and never a deadzone.

---

## Chorded bindings

A pad has too few buttons for a control scheme with many verbs, so a binding can name a
**`Modifier`**: a control that must be held for the binding to contribute. A modifier
held plus a control is a **chord**, and it works on axes as well as buttons — "hold the
left stick click and the right stick's X axis rolls instead of yawing":

```json
{ "Source": { "Device": "GamepadAxis", "Control": 2 }, "Action": "<Yaw>" },
{ "Source": { "Device": "GamepadAxis", "Control": 2 }, "Action": "<Roll>",
  "Modifier": { "Device": "GamepadButton", "Control": 7 } }
```

- **The modifier reads as a button.** A key, mouse button or pad button is down while
  held; an axis (a trigger, a stick half) is down when its value is positive and at
  least `ModifierThreshold`. That is separate from the binding's own `Threshold`, which
  shapes the controlled source. Only the positive half of an axis can be a modifier.
- **A chord replaces the plain meaning of its control.** While a chord is **live**, every
  plain binding on the **same `Source`** contributes nothing, in any active context —
  so above, holding the stick click stops the stick driving Yaw. A chord is live while
  its modifier is down **and** its action resolves from the chord's own context; a chord
  whose action a higher context rebinds is dead and silences nothing. Suppression is
  computed before any value accumulates, so it depends on neither binding nor context
  order, and pressing the modifier before or after the control resolves the same. A chord
  never silences another chord.
- **A modifier need not be an action.** A control used only as a modifier carries no
  binding of its own. If it does carry one, that binding fires as usual while it is held.
- **Keyboard chords work the same** (Shift + key). A binding has one modifier, so "either
  Shift" is two bindings, one on `LeftShift` (`340`) and one on `RightShift` (`344`).
- **Phase stays with the action.** An action held through a plain binding and then
  through a chord stays `Ongoing` as long as its value never reaches zero.

---

## Roles: actions the engine acts on

Most actions mean something only to your control system. A few mean something to the
**engine**, and you say so by tagging the action with a **`Role`**. The engine binds no
key itself: which control navigates a menu is your input map's decision, made in the
same file and the same vocabulary as every other binding.

The navigation roles are `NavigateUp`, `NavigateDown`, `NavigateLeft`, `NavigateRight`,
`NavigateNext`, `NavigatePrevious`, `Confirm` and `Cancel`. A press of one drives focus in
the interactive Gui documents of the seat that pressed it, while that seat holds UI focus:

```json
{ "Id": "0x…", "Name": "UiDown", "Kind": "Button", "Role": "NavigateDown",
  "RepeatDelay": 0.4, "RepeatRate": 0.1 },
{ "Id": "0x…", "Name": "UiConfirm", "Kind": "Button", "Role": "Confirm" }
```

```json
{ "Source": { "Device": "Keyboard", "Control": 264 }, "Action": "<UiDown>" },
{ "Source": { "Device": "GamepadButton", "Control": 13 }, "Action": "<UiDown>" },
{ "Source": { "Device": "GamepadAxis", "Control": 1 }, "Action": "<UiDown>",
  "Scale": 1.0, "Threshold": 0.5 },
{ "Source": { "Device": "Keyboard", "Control": 257 }, "Action": "<UiConfirm>" },
{ "Source": { "Device": "GamepadButton", "Control": 0 }, "Action": "<UiConfirm>" }
```

- **A role action is a `Button`.** A stick drives one through a binding's `Threshold` on
  one half-axis, a direction per half. Shift+Tab for `NavigatePrevious` is a pair of
  chords — `Tab` modified by `LeftShift` (`340`) and by `RightShift` (`344`) — beside a
  plain `Tab` on `NavigateNext`, which the live chord silences.
- **A role fires on its press.** Holding it fires nothing more unless it **repeats**:
  with a `RepeatRate` (seconds between repeats) set, a held role fires again once held
  its `RepeatDelay` (0 waits one `RepeatRate`), then at the rate — at most once a frame,
  from a timer the engine keeps per action per seat, so a pad repeats exactly as a key
  does. Both default to 0, no repeat. The cook rejects a negative value, a repeat on an
  action with no role, a delay with no rate, and a role on a non-`Button` action.
- **The modifiers held on the keyboard travel with the press**, so Shift with a direction
  extends an `Extended` list's range and Control moves focus alone.
- **Where the map lives.** Name it in `project.veng` as the project's default UI context:

  ```json
  "defaultUiContext": "0x…"
  ```

  The cook writes it into the cooked project, the game takes it from there at boot, and
  the editor reads the same key, so menus navigate the same under the editor's Play as in
  the shipped game. `Application::SetDefaultUiContext` overrides it at runtime. With none,
  documents navigate by pointer alone.

**Releasing the cursor is a role too.** The engine releases no captured cursor on a key of
its own. Tag a `Button` action **`ReleaseFocus`** and bind it in a map the seat resolves
while it plays; its press hands the seat's gameplay focus back to the UI:

```json
{ "Id": "0x…", "Name": "ReleaseCursor", "Kind": "Button", "Role": "ReleaseFocus" }
```

```json
{ "Source": { "Device": "Keyboard", "Control": 256 }, "Action": "<ReleaseCursor>" }
```

`ReleaseFocus` fires only under gameplay focus and the navigation roles only under UI
focus, so the same key can carry this and a `Cancel`: the press that frees the cursor is
still held when the menu appears, so it never also cancels there. Bind no release and the
cursor still cannot be trapped — losing window focus suspends the capture, and returning to
the window takes it back with no click.

**How seats resolve roles.** The engine resolves every role action once a frame, before
any world ticks — so a paused world's menu still navigates — for each local `SeatInput`
seat and for the implicit all-devices seat that drives viewports bound to no seat. A seat
resolves its own `InputContextStack` with the default UI context beneath it: re-declare
and re-bind a role action's **id** in a seat context to shadow the default's controls for
that seat. A `SeatFocusScope` that swaps in a context makes it the seat's whole scheme, so a
seat suspended under an overlay navigates nothing. Resolution continues whatever the
seat's focus and only the dispatch is gated, so the press that closes one screen is never
also a fresh press in the screen it uncovers. A key a text field or the immediate-mode
overlay has taken reads as released until you let it go, so typing never navigates; a pad
is never claimed.

---

## The editor

The **`InputMappingEditorPanel`** (registered for `AssetTypes::InputMap`) opens a
`*.inputmap.json` and draws its actions + bindings through the reflection inspector,
so the binding table is add/remove/edit-able with no bespoke widget code. It reads and
saves the file through the same reflection walker the cook binds it with, so every
action and binding field round-trips — an action's `Role`, `RepeatDelay` and
`RepeatRate` included, in its Role category. It shows
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
