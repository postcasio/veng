# Authoring and playing rumble clips

Rumble in veng is authored, not hand-driven. A **rumble clip** is a cooked asset — a short
keyframed animation of a gamepad's motors. Continuous rumble is a **`RumbleSource`** component that
the View-phase `HapticsSystem` plays, the way an `AudioSource` plays a sound; a pulse is a
**one-shot** a system fires through its context. Either way the rumble belongs to the scene that
played it: it holds while the world is paused, is silent while nothing presents the scene, and ends
when the scene goes. The haptics mixer layers everything playing on a pad and is the only thing that
ever writes a pad's motors. The architecture is in
[engine/src/Haptics/CLAUDE.md](../../engine/src/Haptics/CLAUDE.md).

## 1. Write the clip

A clip is a `*.rumble.json` file. Its keys are the reflected field names of `RumbleClipData`, bound
strictly — a misspelt key is a cook error, not a silent default.

```json
{
  "Duration": 0.25,
  "Loop": false,
  "LowFrequency": {
    "Keys": [
      { "Time": 0.0, "Value": 0.8, "Interp": "Smooth" },
      { "Time": 0.25, "Value": 0.0 }
    ]
  },
  "HighFrequency": {
    "Keys": [
      { "Time": 0.0, "Value": 1.0 },
      { "Time": 0.1, "Value": 0.0 }
    ]
  }
}
```

- **`Duration`** is in seconds and must be greater than zero.
- **Four channels**, each a curve over `[0, Duration]` with values in `[0, 1]`:
  - `LowFrequency` — the heavy grip motor, the deep rumble;
  - `HighFrequency` — the light grip motor, the buzz;
  - `LeftTrigger`, `RightTrigger` — trigger motors, ignored on a pad without them.

  Leave a channel out and it is silent. At least one channel needs a key.
- **A curve** is a list of `Keys`, sorted by `Time`. Each key's `Interp` shapes the segment that
  leaves it: `Linear` (the default), `Step` (hold, then jump) or `Smooth` (an ease with flat ends).
  Before the first key the curve holds its value, and after the last key likewise, so a single key
  is a constant level. Two keys at one time make an instant jump.
- **`Loop`** wraps the clip at `Duration` — a continuous hum. A `RumbleSource` can override it; a
  one-shot always plays once through.

The cook rejects a clip that would not play — unsorted keys, a time past the duration, a value
outside `[0, 1]`, a zero duration, no keyed channel — naming the asset, the channel and the key.

## 2. Add it to the pack

A pack entry like any other asset, with an id minted by `vengc generate-id`:

```json
{ "id": "0x64A87127B0361E70", "type": "RumbleClip", "source": "haptics/jump.rumble.json" }
```

## 3. A pulse: fire a one-shot from a system

Load the clip once, then fire it through `SystemContext::Haptics`:

```cpp
void OnStart(Scene&, const SystemContext& context) override
{
    m_Jump = context.Assets.Load<Haptics::RumbleClip>(JumpClipId);
}

void OnUpdate(Scene& scene, f32, const SystemContext& context) override
{
    scene.Each<PlayerInput>([&](const Entity seat, PlayerInput& player)
    {
        if (player.WasTriggered(Actions::Jump))
        {
            context.Haptics.PlayOneShot(Haptics::RumbleTarget::ForSeat(seat), m_Jump);
        }
    });
}
```

- **Fire and forget.** There is no handle and nothing to stop: the clip plays once through and is
  gone. It belongs to this scene, so a pause holds it and closing the world ends it.
- **Target a seat of this scene.** `ForSeat(entity)` resolves the seat's assigned pad
  (`SeatInput::Gamepad`) when you play it; `ForSeat(Entity::Null)` is the implicit seat, the first
  connected pad. An application without seats targets a pad slot with `RumbleTarget::ForGamepad`.
- **Do not gate on `IsReplay`.** During a client's reconciliation replay the context's facade starts
  nothing, so a Sim system replayed to re-derive predicted state cannot re-trigger a pulse.
- **A clip still loading plays nothing** (and logs once), like an unresident sound.

## 4. A hum: put a `RumbleSource` on an entity

Continuous rumble is data. Author a `RumbleSource` on an entity — in a prefab, or added at runtime —
and list `HapticsSystem` in the level's systems:

```cpp
scene.Add<RumbleSource>(engine, RumbleSource{
    .Clip = context.Assets.Load<Haptics::RumbleClip>(HumClipId),
    .Seat = driverSeat,                    // a seat of this scene; Null is the implicit seat
    .Loop = Haptics::RumbleLoop::Always,   // FromClip | Always | Once
    .Playing = false,                      // authored stopped
    .FadeOutSeconds = 0.3f,
});
```

Then drive it from your own system:

```cpp
RumbleSource& hum = scene.Get<RumbleSource>(engine);
hum.Playing = driving;                     // a rise restarts the clip, a fall fades it out
hum.Intensity = speed / maxSpeed;          // live: set it every tick
```

- **`Playing` is the one control.** Authored `true`, the source plays from the first frame it is
  seen; set it `false` and it fades to silence over `FadeOutSeconds`; set it `true` again and the clip
  restarts. A `Once` clip clears `Playing` itself when it ends.
- **The seat is resolved every frame**, so the rumble follows a reassignment and is silent while the
  seat has no pad.
- **There is no cleanup.** Destroy the entity or remove the component and it is silent the next
  frame; nothing needs an `OnStop`.

## How layers combine

Each pad mixes its one-shots and sources **by maximum, per channel**, then applies the master intensity. Two
overlapping effects never add up to a saturated motor: a strong pulse rises over a hum, and the hum
is back the moment the pulse ends, with no priorities to manage. Author each clip at the level it
should feel on its own.

While the window is unfocused the output is silenced, but clips keep their time — a hum resumes on
refocus, and a pulse that ended meanwhile does not replay. `SetMasterIntensity` on the engine
(`Application::GetHaptics()`) scales every pad — the hook for a player's vibration setting.

## Checking it without a pad

The engine runs headless, so a unit test fires a one-shot or ticks a `RumbleSource` and asserts what
`GetOneShots()`, `GetLayers()` and `GetOutput(pad)` report. In a running game, the gamepad debug
panel (`UI::GamepadPanel`) shows each pad's mix with the one-shots and layers feeding it — each with
its scope's state, `live`, `muted` or `held` — and plays an offered clip; the MCP tool
`haptics.state` reports the same, so with `input.send`'s `pad_connect` a driven session confirms
rumble on a virtual pad with no hardware. hello-triangle fires its `jump` clip on the pad's A button
and hums while a seat drives the socket slab.
