# Multi-seat input and split-screen

This guide covers **routing input per seat**: giving each local player its own
devices so two seats on one machine resolve to two distinct `PlayerInput`s. It is
the natural extension of [Authoring input actions](authoring-input-actions.md):
that guide turned raw device state into named actions *per seat*; this one turns
"per seat" into **one device per seat** — a keyboard-and-mouse player beside a
controller player, each driving its own pawn, each rendered into its own quadrant.

The seat authoring is in hello-triangle's
[`player.prefab.json`](../../examples/hello-triangle/assets/prefabs/player.prefab.json)
— a `Viewer` seat carrying a `SeatInput`. The runtime reconfigure — spawning a second
seat and reconfiguring the managed viewport list into quadrants — is exercised end to
end by [`tests/gpu/splitscreen.cpp`](../../tests/gpu/splitscreen.cpp). Open them beside
this guide.

---

## The shape of it

```
 devices                    routing (this guide)               per seat (unchanged resolver)
 ───────                    ────────────────────               ─────────────────────────────
 keyboard ─┐  Veng::Input   ┌──────────────────────────┐        ┌─────────────────────┐
 mouse  ───┤  snapshot      │ SeatInput (per Viewer):   │  seat- │ InputMappingSystem   │
 pad 0  ───┤  (keyboard,    │  · UsesKeyboardMouse      │  scoped│  ResolveActions(...)  │─► PlayerInput
 pad 1  ───┘   mouse, and   │  · Gamepad = id | None    │──raw──►│  (once per local seat)│    (per seat)
               gamepads)    │ pointer → seat by region  │  view  └──────────┬──────────┘
                            │  + UsesKeyboardMouse gate  │                   │ read by name
                            └──────────────────────────┘        ┌──────────▼──────────┐
 render: Application manages a LIST of viewports; split-screen is  │ control → Intent    │ UNCHANGED
 ReconfigureManagedViewports({leftHalf → A, rightHalf → B}).       └─────────────────────┘
```

The resolver is exactly the one the input-mapping guide describes:
`InputMappingSystem` runs the pure `ResolveActions(activeContexts, raw, previous)`
once per locally-owned seat into that seat's `PlayerInput`. What changes here is
only the `raw` each seat reads — every seat used to be handed the *same* view over
the shared `Veng::Input`; now each seat gets a **filtered view scoped to its own
assigned devices**. Two seats, two device sets, two `PlayerInput`s.

Three pieces make that happen: a **`SeatInput`** component naming each seat's
devices, the per-seat **`SeatInputView`** the mapping system builds from it, and the
**managed viewport list** that lets you reconfigure the window into quadrants at
runtime. Everything downstream — the control system, `Intent`, movement — is
untouched.

---

## 1. A seat names its devices with `SeatInput`

A **`SeatInput`** component (`Veng/Scene/Components.h`) lives on the `Viewer` seat
entity beside `Possesses` and the `InputContextStack`, and names the physical
devices that feed that seat:

```json
"::Veng::Viewer": { "Camera": 0 },
"::Veng::PlayerInput": {},
"::Veng::InputContextStack": { "Active": [ 16596091148679838649 ] },
"::Veng::Possesses": { "Pawn": 2 },
"::Veng::SeatInput": {
  "UsesKeyboardMouse": true,
  "Gamepad": "None",
  "WantsGamepad": true
}
```

- **`UsesKeyboardMouse`** — whether this seat reads the keyboard (and, region-gated,
  the pointer). There is one keyboard, held by whichever seat sets this.
- **`Gamepad`** — the pad slot this seat's gamepad bindings read, or `"None"` for no
  pad. A `GamepadId` is the **GLFW joystick slot** (0..15), stable while a pad stays
  connected — so a persisted assignment never silently re-points at a different
  physical pad. A level can author a fixed slot (`"Gamepad": 0`) to pin seat 1 to pad
  0.
- **`WantsGamepad`** — whether the `DeviceAssignmentSystem` (below) auto-fills an
  empty `Gamepad` slot when a pad connects. Independent of `UsesKeyboardMouse`: the
  ordinary single-player seat opts into both a keyboard *and* an auto-assigned pad.

> **A seat with no `SeatInput` takes no local input.** `InputMappingSystem` now
> queries `(Viewer, InputContextStack, PlayerInput, SeatInput)`, so a seat lacking
> `SeatInput` is skipped — its `PlayerInput` is left for another producer to
> synthesize (an AI seat, or a future replicated remote seat). **Every local human
> seat must author a `SeatInput`.** This is a behavioral change, not merely additive:
> a seat that previously resolved against the shared snapshot now resolves nothing
> until it carries the component.

Because `SeatInput` is a reflected builtin type, **authoring one into a prefab or
level requires re-cooking against an engine that registers it** — an unknown `TypeId`
is a cook-time validation error against an older `RegisterBuiltinTypes`.

---

## 2. Auto-assigning pads: `DeviceAssignmentSystem`

The builtin **`DeviceAssignmentSystem`** (a Sim system, registered to run
immediately *before* `InputMappingSystem`) reconciles each seat's `SeatInput.Gamepad`
against the pads actually connected this tick (`Veng::Input::ConnectedGamepads`):

- A pad **connected but held by no seat** is assigned to the first seat with
  `WantsGamepad` and a `None` slot, in deterministic seat-iteration order.
- A seat whose assigned slot is **no longer connected** is cleared back to `None`.
- A **level-authored slot is respected** — the policy fills only `None` slots, and
  only for `WantsGamepad` seats.

So "controller connected → first opted-in seat gets it" is automatic, and in
headless (where the connected set is empty) it is a no-op. A controller-disconnect
"press start to rejoin" flow is a game-side policy over the same connect/disconnect
events, not engine mechanism.

The gamepad state it reconciles against is the device surface on **`Veng::Input`**
(`Veng/Input.h`): pads are tracked by `GamepadId`, polled once per frame into the
same event-fed snapshot as keyboard and mouse, and queried with
`IsGamepadButtonDown` / `GetGamepadAxis` / `ConnectedGamepads`. The
`GamepadButton` / `GamepadAxis` binding sources a `*.inputmap.json` can name — inert
before the device layer existed — are now live.

---

## 3. How each seat reads only its own devices

`InputMappingSystem` builds each seat a **`SeatInputView`** (`Veng/Input/RawInput.h`)
from its `SeatInput`, and resolves against that instead of the shared adapter:

- **Gamepad arms read *only* the seat's assigned pad** (`SeatInput.Gamepad`). A
  different seat's pad, or an unassigned seat, reads neutral.
- **Keyboard arms read the shared keyboard only when the seat sets
  `UsesKeyboardMouse`** — otherwise neutral. Keyboard is *not* region-gated: there is
  one keyboard, held by whichever seat opts in.
- **Pointer arms are gated twice**, described next.

So a pad-only guest seat (`UsesKeyboardMouse = false`, `Gamepad = 0`) reads only pad
0; the keyboard/mouse host seat reads the keyboard, the mouse, and no pad. `Move` and
`Look` resolve independently for each.

### The pointer routes by region *and* the keyboard/mouse gate

A **gamepad** routes by *id* (which seat holds the pad). A **pointer** is a single
positional device shared across the whole window, so it routes by *region*: the
`InputRouter` hit-tests the cursor's window point against each `Presented` viewport's
region (`Viewport::WindowToViewport`) and resolves it to the seat that owns that
region. A seat reads the pointer only when it **both** sets `UsesKeyboardMouse`
**and** owns the region the cursor is over. A mouse-position source reports the
pointer's **viewport-local** coordinate (so an action reading position stays inside
the seat's own quadrant); a look-**delta** axis reports the raw window pixel delta
unchanged, which keeps look sensitivity invariant across region size.

> **The pointer is inert under cursor capture.** Gameplay focus captures the OS
> cursor (hidden and locked), and mouse-look reads *raw delta*, not position — so
> "which quadrant is the cursor over" is undefined in the captured mode a
> first-person demo plays in. The rule: **while the cursor is captured, region
> routing is off and the pointer belongs wholly to the single `UsesKeyboardMouse`
> seat** (delta-look needs no position). Region routing applies only when the cursor
> is free — menus, UI focus, click-to-point. This is why a live gameplay split-screen
> demo proves **device-id** routing (keyboard → seat A, pad → seat B), while
> **pointer-region** routing is exercised by a headless test with a scripted free
> cursor.

The router computes this pointer routing once per frame; the app associates a
viewport's region with its seat when it registers the viewport (see below), so no
game code touches the hit-test.

---

## 4. Split-screen: reconfiguring the managed viewport list

The engine's plug-and-play render path already manages one full-window viewport (the
one a game pushes its scene through). For split-screen it manages a **list** of
viewports, and split-screen is a *runtime reconfigure* of that list — not a bespoke
render path.

A managed viewport is described by a `ManagedViewportInfo` carrying a **normalized
`Layout`** (an offset + extent in `[0,1]` window fractions the engine resolves to
pixels on every resize, so quadrants stay stable across window resizes) and an
optional bound **`Viewer`** (the seat whose camera the engine resolves and pushes
into that viewport — and whose region the engine associates with the router). To go
split-screen, call **`ReconfigureManagedViewports`** with the new set, applied at a
safe point (the top of the frame, outside system iteration):

```cpp
const std::array quadrants{
    // Left half: the primary seat (unbound — the engine already pushes its camera).
    ManagedViewportInfo{
        .Layout = { .Offset = {0.0f, 0.0f}, .Extent = {0.5f, 1.0f} },
    },
    // Right half: seat B, bound to its Viewer so the engine resolves + pushes its
    // camera and associates the region with the router for pointer routing.
    ManagedViewportInfo{
        .Layout = { .Offset = {0.5f, 0.0f}, .Extent = {0.5f, 1.0f} },
        .Viewer = seatBViewer,
    },
};
ReconfigureManagedViewports(quadrants);
```

`GetManagedViewport(n)` reaches each viewport (index 0 is the primary,
`GetPrimaryViewport()`), and `GetManagedViewportCount()` reports the length. The
gather + composite tail is unchanged: it already assembles every registered
`Presented` viewport into the window, so N quadrant viewports "fall out" as N
placements. Returning to single-seat is the inverse — reconfigure back to one
full-window `Layout`.

Reconfiguring drops the prior viewports (each self-clears its router association) and
builds the new set in order, associating any viewport that names a `Viewer`. A
single default-`Layout` managed viewport is byte-identical to the untracked
full-window case — so the default single-seat path is unchanged, and a world that
never reconfigures pays nothing.

---

## 5. Spawning a second seat

A second seat is an ordinary spawn: instantiate a player prefab (which brings a
`Viewer` + camera + pawn + `Possesses` + `InputContextStack` + `SeatInput`) into the
running world, then retype its `SeatInput` for its device — spawn the copy and retype
the new seat to a pad-only guest:

```cpp
world->Each<Viewer, SeatInput>(
    [&](const Entity seat, Viewer&, SeatInput& devices)
    {
        if (isTheNewlySpawnedSeat(seat))
        {
            devices.UsesKeyboardMouse = false;   // the keyboard stays with seat A
            devices.Gamepad = GamepadId(0);       // this guest reads pad 0
            devices.WantsGamepad = false;
        }
    });
```

Spawn and any component retype must happen at a **safe point outside system
iteration** (structural changes mid-`Each` are illegal). Both seats then drive
through the *existing* action → `Intent` → movement pipeline — no new gameplay
system, no change to the control or movement systems.

---

## The boundary: routing stops at `PlayerInput`

All of this sits **entirely below `PlayerInput`**. The routing changes only *what raw
state each seat's resolve reads*, one layer below `Intent`:

- **Only the local human seat's raw read changes.** AI seats and (future) remote
  seats carry no `SeatInput` device assignment — their `PlayerInput` is synthesized
  or replicated and their `Intent` is written directly, exactly the drop-in-producer
  property the seat model already has.
- **`Intent`, `Authority`, and the Sim/View split are untouched.** Routing is a
  *client-local, device-facing* concern. It does not replicate anything.

Replicating a human seat's input to a server — sending its `PlayerInput`, re-deriving
its `Intent` server-side, and replicating `Session`/pawn state by `Authority` — is
the networking layer, a separate body of work built on this same seat seam. This
guide gets you two local players on one machine; the wire is its own step.

---

## Where to go next

- **[Authoring input actions](authoring-input-actions.md)** — the per-seat resolver
  this guide routes into: action-id constants, `*.inputmap.json` bindings, the
  `InputContextStack`, and reading actions in a control system.
- **[Writing gameplay systems](writing-gameplay-systems.md)** — the
  Input → Intent → Movement pattern both seats drive through unchanged.
- The generated API reference (`cmake --build build --target docs`) documents every
  type named here — `SeatInput`, `SeatInputView`, `DeviceAssignmentSystem`,
  `PointerRouting`, `ManagedViewportInfo`, `ReconfigureManagedViewports`, and the
  `Veng::Input` gamepad surface — in full.
