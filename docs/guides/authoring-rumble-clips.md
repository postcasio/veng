# Authoring and playing rumble clips

Rumble in veng is authored, not hand-driven. A **rumble clip** is a cooked asset — a short
keyframed animation of a gamepad's motors — and a system plays it on a seat's pad through the
**haptics engine**, the way it fires a sound through the audio engine. The engine layers every clip
playing on a pad, scales and fades them, pauses them with their world, and is the only thing that
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
- **`Loop`** wraps the clip at `Duration` — a continuous hum. A play can override it.

The cook rejects a clip that would not play — unsorted keys, a time past the duration, a value
outside `[0, 1]`, a zero duration, no keyed channel — naming the asset, the channel and the key.

## 2. Add it to the pack

A pack entry like any other asset, with an id minted by `vengc generate-id`:

```json
{ "id": "0x64A87127B0361E70", "type": "RumbleClip", "source": "haptics/jump.rumble.json" }
```

## 3. Play it from a system

Load the clip once, then play it through `SystemContext::Haptics`:

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
            context.Haptics.Play(
                Haptics::RumbleTarget::ForSeat(SeatRef{.World = context.World, .Viewer = seat}),
                m_Jump, Haptics::RumbleParams{.World = context.World});
        }
    });
}
```

- **Target a seat.** A seat target follows the seat's assigned pad (`SeatInput::Gamepad`) every
  frame, so it keeps working through a reassignment and is silent while the seat has no pad. An
  application without seats targets a pad slot directly with `RumbleTarget::ForGamepad`.
- **Pass the world.** `RumbleParams::World = context.World` makes the instance the world's: it holds
  while the world is paused and stops when the world closes. Leave it invalid for rumble the
  application owns (a menu), which is never paused.
- **Do not gate on `IsReplay`.** During a client's reconciliation replay the engine starts nothing,
  so a Sim system replayed to re-derive predicted state cannot re-trigger a clip.
- **A clip still loading plays nothing** (and logs once), like an unresident sound.

## 4. Keep, scale and stop it

`Play` returns a `RumbleHandle`. A continuous effect keeps it:

```cpp
if (!context.Haptics.IsPlaying(m_Hum))
{
    m_Hum = context.Haptics.Play(target, m_HumClip, {.Loop = true, .World = context.World});
}
context.Haptics.SetIntensity(m_Hum, speed / maxSpeed);   // strength follows a value
...
context.Haptics.Stop(m_Hum, 0.3f);                       // fade out over 0.3 s
```

`StopAll(target)` stops everything on a target at once. `SetMasterIntensity` scales every pad — the
hook for a player's vibration setting.

## How layers combine

Each pad mixes its instances **by maximum, per channel**, then applies the master intensity. Two
overlapping effects never add up to a saturated motor: a strong pulse rises over a hum, and the hum
is back the moment the pulse ends, with no priorities to manage. Author each clip at the level it
should feel on its own.

While the window is unfocused the output is silenced, but clips keep their time — a hum resumes on
refocus, and a pulse that ended meanwhile does not replay.

## Checking it without a pad

The engine runs headless, so a unit test plays a clip on a padless seat and asserts what
`GetInstances(target)` reports. In a running game, the gamepad debug panel (`UI::GamepadPanel`)
shows each pad's mix and live instances and plays an offered clip, and the MCP tool `haptics.state`
reports the same — with `input.send`'s `pad_connect`, a driven session confirms a clip plays on a
virtual pad with no hardware. hello-triangle plays its `jump` clip on the pad's A button.
