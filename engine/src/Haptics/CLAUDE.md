# Veng/Haptics — rumble clips and the haptics engine

Authored rumble an application plays, layers, scales, loops and stops on gamepads the way it plays
sound: a cooked **rumble clip** asset (a keyframed animation of a pad's motors) and the
**`HapticsEngine`** every system reaches, which mixes every playing clip per pad and is **the only
writer of the motors**. Project-wide conventions live in [the root CLAUDE.md](../../../CLAUDE.md),
the runtime overview in [engine/CLAUDE.md](../../CLAUDE.md), the pad device layer and seats in
[../Scene/CLAUDE.md](../Scene/CLAUDE.md), and the asset tier in [../Asset/CLAUDE.md](../Asset/CLAUDE.md).

## The pieces

- **`Curve1D`** (`Veng/Math/Curve.h`, reflected) — a sorted list of `CurveKey { Time, Value, Interp }`,
  `CurveInterp` being `Linear`, `Step` or `Smooth` (a Hermite with flat tangents). A key's
  interpolation shapes the segment leaving it; `Evaluate` holds the end keys' values outside them and
  an empty curve reads zero. Two keys at one time author an instant jump. It is general on purpose:
  the next keyframed scalar (a particle envelope, an audio fade) uses it rather than growing its own.
- **`Haptics::RumbleClip`** (`Veng/Haptics/RumbleClip.h`, `AssetTypes::RumbleClip`, CPU-only) — a
  `RumbleClipData { Duration, Loop, LowFrequency, HighFrequency, LeftTrigger, RightTrigger }`, each
  channel a `Curve1D` over `[0, Duration]` with values in `[0, 1]`; an empty channel is silent and
  the trigger channels are ignored on a pad without trigger motors. Cooked from a `*.rumble.json`
  whose keys are the reflected field names, bound strictly (an unknown key is a cook error), and
  validated by **`CheckRumbleClip`** — the one statement of a playable clip, shared by the cook and
  anything else that builds one. Loaded by id through the ordinary path (`RumbleClipLoader`), so it
  hot-reloads through `MountMemory` like every asset, and has an `AssetHandle<RumbleClip>` leaf, so a
  component can name one.
- **`Haptics::HapticsEngine`** (`Veng/Haptics/Haptics.h`) — reached as `SystemContext::Haptics` and
  `Application::GetHaptics()`, following `SystemContext::Audio`.

## Playing

`Play(RumbleTarget, AssetHandle<RumbleClip>, RumbleParams) → RumbleHandle`:

- **`RumbleTarget`** is a seat (`ForSeat(SeatRef)`) or a pad slot (`ForGamepad(GamepadId)`). A seat
  target resolves the seat's `SeatInput::Gamepad` **every frame**, so it follows a reassignment and is
  silent while the seat has no pad; the implicit seat (a null Viewer) resolves to the first connected
  pad, as its input does. A pad target is for an application without seats.
- **`RumbleParams { Intensity = 1; optional<bool> Loop; WorldInstanceId World; }`** — `Loop`
  overrides the clip's own, `World` names the world the instance belongs to (a scene system passes
  `context.World`); an invalid world means application-owned.
- **A play inside a `ReplayScope` starts nothing and returns a null handle.** The Application holds
  one around every client reconciliation replay step, so a Sim system re-run to re-derive predicted
  state never re-triggers a clip — the engine enforces it, so no caller gates on `IsReplay`. Every
  other call (`IsPlaying`, `SetIntensity`, `Stop`) works as usual during a replay, so a system that
  keeps a hum's handle sees it still playing.
- **An unloaded clip returns a null handle and logs once per clip.** The inert engine starts nothing.

Controls: `SetIntensity`, `Stop(handle, fadeSeconds = 0)` (a fade ramps the instance linearly to
zero and retires it; a later, shorter fade takes over from the current level), `IsPlaying`,
`StopAll(target)` (matched exactly), `SetMasterIntensity` (the hook for a player's vibration setting,
clamped to `[0, 1]`, default 1). Inspection: `GetInstances(target)` and `GetAllInstances()` report
each live instance's clip, time, duration, intensity, fade, flags, world and the pad it resolved to;
`GetOutput(pad)` is a pad's mix. These are what the debug window, the MCP read and the tests use.

## The frame

`Application::Frame` calls `Update` once, in its **presentation step** (`Frame/Presentation`) **after
`OnUpdate`** and after the presentation scopes are resolved — every play this frame (the worlds' systems
and `OnUpdate`) has landed. Update:

1. **Advances** every instance by the frame's unscaled delta. A freshly played instance holds at time
   zero on its first Update, so its clip's start is what sounds first. An instance whose world is
   **paused** holds its time (and its fade) and contributes nothing, and resumes when the world does;
   one whose world **closed** stops. An application-owned instance is never paused.
2. **Retires** finished non-looping instances and completed fades.
3. **Mixes** each pad: per channel, the **maximum** over the instances resolving to it (each the clip
   channel × its intensity × its fade), then × the master intensity, clamped to `[0, 1]`. Maximum
   rather than sum, so layered rumble never saturates: a pulse rises above a hum and the hum returns
   after it, with no priority system.
4. **Writes** every slot's levels to the pad backend's `GamepadBackend::SetMotors` — the primitive's
   only caller, so a pad has exactly one writer. While the window is unfocused (and background input
   is not retained) the written levels are zero, but instances keep their time: a hum returns on
   refocus and a pulse that ended meanwhile does not replay. `GetOutput` still reports the mix.

**Headless and dedicated runs** keep the engine running — it is deterministic and testable there —
and only the device write is absent. The backend sends a pad its levels only on change and renews
them before its short duration lapses, so a stalled frame loop lets a pad fall silent.

## Seams

- **`HapticsEngineInfo`** carries the three host hooks — `ResolveSeat`, `WorldState`, `WriteMotors` —
  each optional, so a test builds an engine over exactly what it exercises. The Application fills them
  from its `WorldRunner`, `Input` and pad backend.
- **`SystemContext::Haptics` is a required reference**, like every service on the context, so a
  context that omits it does not compile; every context the Application builds binds its live engine.
  `Haptics::GetInertEngine()` is a function-static engine on which every play starts nothing, for a
  caller that must hand one over and wants no rumble.
- **The pure core** is `EvaluateClip(clip, t, loop)` (a looping clip wraps, a non-looping one reads
  zero from its duration on) and `MixRumble(layers, master)`, unit-tested directly beside the engine
  (`tests/unit/haptics.cpp`; the curve in `tests/unit/curve.cpp`; the cook in
  `tests/cooker/rumble_clip_cook.cpp`).
- **Tooling** — `UI::GamepadPanel` shows each pad's mix and instances and plays a host-offered clip
  through `Play`; MCP's `haptics.state` reports the same, so a driven session with a virtual pad
  verifies rumble with no hardware (see [mcp/CLAUDE.md](../../../mcp/CLAUDE.md)).

**Ownership is by world id, not by scene.** An instance's owner is the `WorldInstanceId` in its
params, judged through the `WorldState` hook, so an open world reads open whether or not anything
presents it. The scene-owned alternative — a scope dying with its scene, held by the View phase's lease
and muted when unpresented — is the presentation scope ([../Scene/CLAUDE.md](../Scene/CLAUDE.md),
"Presentation scopes"), whose states the haptics update already runs after.

The authoring walkthrough is [docs/guides/authoring-rumble-clips.md](../../../docs/guides/authoring-rumble-clips.md).
