# Veng/Haptics — rumble clips, rumble sources and the haptics mixer

Authored rumble on gamepads, built the way audio's authored half is: a cooked **rumble clip** asset
(a keyframed animation of a pad's motors), a **`RumbleSource`** component for continuous rumble that
the View-phase **`HapticsSystem`** plays, a **scoped fire-and-forget one-shot** for a pulse, and the
**`HapticsEngine`**, a pure per-pad mixer that is **the only writer of the motors**. Everything a
scene plays belongs to that scene's **presentation scope**, so it holds with a pause, is silent while
the scene is unpresented, and ends when the scene goes — with no world ids, handles or stop calls.
Project-wide conventions live in [the root CLAUDE.md](../../../CLAUDE.md), the runtime overview in
[engine/CLAUDE.md](../../CLAUDE.md), the pad device layer, seats and presentation scopes in
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
- **`RumbleSource`** (`Veng/Haptics/RumbleSource.h`, reflected, not replicated) — continuous rumble.
- **`HapticsSystem`** (`Veng/Haptics/HapticsSystem.h`, a builtin `Phase::View` system) — plays the
  scene's `RumbleSource`s, the peer of `AudioSystem`.
- **`Haptics::ScopedHaptics`** (`Veng/Haptics/ScopedHaptics.h`) — what `SystemContext::Haptics` is:
  the engine as one scene reaches it.
- **`Haptics::HapticsEngine`** (`Veng/Haptics/Haptics.h`) — the per-pad mixer,
  `Application::GetHaptics()`.

## Targets resolve in the calling scene

A **`RumbleTarget { Kind; Entity Seat; GamepadId Gamepad }`** is scene-local: `ForSeat(Entity)` names
a seat's Viewer entity in the scene whose system plays it, `ForGamepad(GamepadId)` a pad slot (an
application without seats). **`ResolveRumbleTarget(target, scene, input)`** is the one resolution: a
seat entity reads its `SeatInput::Gamepad` in that scene (no pad when the entity, its `SeatInput` or
the scene is missing), the implicit seat (`Entity::Null`) resolves to the first pad `input` reports
connected, as its input reads every device, and a pad target to itself. Because the scene is named,
two worlds' seats sharing an entity handle never alias.

## Continuous rumble: `RumbleSource` and `HapticsSystem`

`RumbleSource { Clip, Target, Seat, Gamepad, Intensity, Loop, Playing, FadeOutSeconds }` plus three
runtime fields with no `VE_FIELD` (`Time`, `Fade`, `WasPlaying`). `RumbleLoop` is `FromClip`,
`Always` or `Once`. **`Playing` is the one control**: authored `true`, it plays from the first frame
the system sees it (a spawn, or the component added at runtime); a system sets it `false` to stop,
ramping linearly to silence over `FadeOutSeconds` (0 stops at once), and `true` again to restart
from the clip's start. `Intensity` is live — a system drives it every tick.

Each View pass, for each source, `HapticsSystem`: on a rise of `Playing` resets the time to zero and
the fade to one (that frame sounds the clip's start); otherwise advances the time by the frame delta
while it sounds or fades, and lowers the fade while `Playing` is false; resolves its target in its own
scene **every frame** (so a seat target follows a reassignment and is silent while the seat has no
pad); evaluates the clip (`EvaluateClip`), scales it by `Intensity × Fade`, and submits it through
`context.Haptics.Submit(pad, channels)`; and clears `Playing` when a once-through clip has ended. A
clip still loading holds its time. The system keeps no handles and has **no `OnStop`**:

- a **paused** world runs no View pass, so nothing advances or is submitted, and it resumes from the
  same time;
- an **unpresented** scene's scope is `Muted`, so the mixer drops its layers while its sources keep
  time;
- a **removed component or destroyed entity** simply stops being submitted — silent the next frame;
- a **closed** scene takes its sources with it.

A level that does not list `HapticsSystem` plays no `RumbleSource`; one-shots need no system.

## One-shots: scoped fire and forget

`context.Haptics.PlayOneShot(target, clip, intensity = 1)` plays a clip **once through** (whatever
the clip's own `Loop` — a looping effect is a `RumbleSource`), with no handle. The target resolves to
a pad **at the moment of the play**, and the one-shot keeps that pad; rumble that must follow a seat's
reassignment is a source. It belongs to the calling scene's scope, so it holds with a pause, is silent
while the scene is unpresented, and stops when the scene goes.

**The replay gate is the facade's** (`ScopedHaptics` carries the context's `IsReplay`): inside a
client reconciliation replay `PlayOneShot` starts nothing, so a Sim system re-run to re-derive
predicted state never re-triggers a pulse and needs no `IsReplay` gate of its own. `Submit` is not
gated — a layer lasts one frame and mixes by maximum, so a repeat changes nothing. An unloaded clip
starts nothing and logs once per clip.

**Application-level plays** — a debug panel's test, an editor audition, `OnUpdate` code — go through
**`Application::GetApplicationHaptics()`**, the same facade over the registry's always-`Live`
application scope, with no scene (only the implicit seat and pad targets resolve). Never for a
scene's systems.

## The mixer

`HapticsEngine` is constructed over the application's `PresentationScopes` (which outlives it) and a
`HapticsEngineInfo { WriteMotors }`. It holds two things, each tagged with a `PresentationScopeId`: a
**one-shot table** `{scope, pad, clip, time, intensity}` (`PlayOneShot(scope, pad, clip, intensity)`)
and **this frame's layers** `{scope, pad, channels}` (`SubmitLayer(scope, pad, channels)`).
`StopOneShots(scope, pad)` is for tooling. `Application::Frame` calls `Update` once, in its
**presentation step** (`Frame/Presentation`), after `OnUpdate` and right after
`PresentationScopes::Resolve()` has latched this frame's states:

1. **One-shots by their scope's state.** `Closed` → dropped. `Held` → frozen: no time advance, and a
   fresh one stays fresh. `Muted` / `Live` → advanced by the unscaled frame delta; a fresh one holds
   at time zero on its first advancing update, so its clip's start is what sounds first. One that
   reaches its duration is dropped — a muted one exactly when it would have ended audibly.
2. **Mix.** Each pad takes, per channel, the **maximum** over the one-shots and layers whose scope is
   `Live` (`MixRumble`), × the master intensity, clamped to `[0, 1]`. Maximum rather than sum, so
   layered rumble never saturates: a pulse rises above a hum and the hum returns after it, with no
   priority system.
3. **Write** every slot's levels through `WriteMotors` to the pad backend's
   `GamepadBackend::SetMotors` — the primitive's only caller, so a pad has exactly one writer. While
   the window is unfocused (and background input is not retained) the written levels are zero, but
   one-shots keep their time, so a pulse that ended meanwhile does not replay. `GetOutput` still
   reports the mix. The layers are then cleared for the next frame.

Kept beside it: `EvaluateClip(clip, t, loop)` (a looping clip wraps, a non-looping one reads zero from
its duration on), `ScaleRumble`, `MixRumble`, `SetMasterIntensity` / `GetMasterIntensity` (the hook
for a player's vibration setting, clamped to `[0, 1]`, default 1), `GetOutput(pad)`,
`IsOutputSuspended()`. **Inspection** is `GetOneShots()` and `GetLayers()` (the layers the last update
mixed), each entry carrying its scope id and that scope's current state.

**Headless and dedicated runs** keep the engine running — it is deterministic and testable there —
and only the device write is absent; a dedicated server runs no View phase, so every scope it holds is
`Held` and nothing a scene plays is felt. The backend sends a pad its levels only on change and renews
them before its short duration lapses, so a stalled frame loop lets a pad fall silent.

## Seams

- **`SystemContext::Haptics` is a `ScopedHaptics` held by value**, bound by the context factory
  (`Application::MakeSystemContext`) to the request scene's presentation scope, the scene, the
  input and the request's replay flag. It has no default, so a context that omits it does not
  compile; there is no inert engine. A unit test takes it from `tests/support/TestServices.h`, whose
  `Make(request)` binds the request scene's scope and whose request-less `Make()` binds the
  application scope.
- **`HapticsEngineInfo`** carries only `WriteMotors`, optional, so a test builds an engine over
  exactly what it observes; the Application fills it from its pad backend.
- **The pure core** — `EvaluateClip` and `MixRumble` — is unit-tested directly beside the engine
  (`tests/unit/haptics.cpp`, which also covers each presentation state, the facade's replay gate,
  scene-local seat resolution and `HapticsSystem` through a device-free runner world; the curve in
  `tests/unit/curve.cpp`; the cook in `tests/cooker/rumble_clip_cook.cpp`).
- **Tooling** — `UI::GamepadPanel` shows each pad's mix, one-shots and layers with their scopes, and
  plays and stops a host-offered clip through the application scope; MCP's `haptics.state` reports
  the same, so a driven session with a virtual pad verifies rumble with no hardware (see
  [mcp/CLAUDE.md](../../../mcp/CLAUDE.md)).

The authoring walkthrough is [docs/guides/authoring-rumble-clips.md](../../../docs/guides/authoring-rumble-clips.md).
