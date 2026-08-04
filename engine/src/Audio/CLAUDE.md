# Veng/Audio — the audio subsystem

The engine's sound: an output device wrapped behind the Native idiom, a fixed bus-tree mixer, and a
real-time mixing path fed by an immutable voice snapshot. It plays PCM buffers through buses with
per-bus gain and a small effect surface, degrades to a silent null device when there is no hardware,
and reclaims the resources a voice references without ever touching them while the mixer is mid-mix.

The public surface is `engine/include/Veng/Audio/` (`AudioBus.h`, `AudioBuffer.h`, `Voice.h`,
`AudioDevice.h`, `AudioEngine.h`); the real-time internals are `engine/src/Audio/`. `AudioDevice`
is owned by `Application` for the whole run, mirroring `Renderer::Context`.

## The one thread rule this subsystem adds

veng's rule is that the render thread is single. The audio subsystem is the **one sanctioned
exception**, and it earns it by isolation:

> **The real-time callback thread reads the immutable triple-buffered snapshot and mixes. It calls
> no engine API and touches no engine state — no Scene, no Asset, no Renderer, not `AudioEngine`
> itself.** Everything the engine assumes about single threading stays true because nothing outside
> the audio subsystem shares mutable state with that thread.

The engine-external threads are miniaudio's playback callback thread (which runs `RenderBlock`), and
miniaudio's decode job threads once a later capability enables streaming decode. Neither reaches
engine state; the callback reads one POD `AudioFrame` and writes the output buffer.

## The Native idiom boundary

miniaudio is vendored as a single header (`engine/src/Vendor/MiniAudio.cpp` compiles the one
implementation TU; the header is a private include of `libveng`). Every `ma_*` type lives inside
`AudioDevice::Native`, defined in `AudioDevice.cpp`; no `Veng/Audio/*.h` sees a miniaudio type, and
the `include_hygiene` test guards it. veng drives `ma_device` directly — the high-level `ma_engine`
and its spatializer are **not** used — because veng owns the spatialization DSP: a voice carries the
final gain / pan / pitch / occlusion the caller already computed, so the numbers a later plan's tests
pin are the numbers the mixer consumes.

## The snapshot bridge and reclamation handshake

`AudioEngine` (main thread) holds the authoritative bus parameters and voice table. Each frame
`Publish()` copies them into a free back buffer of a **triple buffer** (`TripleBuffer.h`) and
publishes its index atomically; the callback latches the newest index and holds it for the whole
block. Three slots mean the writer never reclaims the slot the reader holds, so the hard-real-time
reader never spins, never blocks, and never sees a torn frame — the property double buffering cannot
give without a seqlock. The `AudioFrame` is POD, allocation-free, and fixed-capacity (`MaxVoices`).

Resource lifetime is a **handshake, not an assumption**. A `VoiceSnapshot` carries a raw pointer
into an `AudioBuffer`'s PCM. The callback publishes the serial of the frame it is mixing
(`ConsumedSerial`); when a voice is stopped, retired, or evicted, its source is moved to a
main-thread deferred-free queue tagged with the last serial that referenced it, and released only
once `ConsumedSerial` has passed that serial. So a source can never be freed while the callback is
mid-mix on it — the contract every later plan's voice-lifetime story rests on (`AssetManager` clip
eviction, ship-despawn teardown).

The reverse channel is a lock-free SPSC ring (`SpscRing.h`): when a finite voice exhausts, the
callback posts its `{slot, generation}` and the main thread drains it in `DrainRetired()` — the
callback learns nothing about the scene, and the scene learns of a retirement without a callback
into it.

## Shutdown / device-loss join order

`~AudioDevice` **stops and joins the callback thread first** (`ma_device_uninit`), before any member
destructs, so the real-time thread is quiesced before the engine's deferred sources release. The
member is declared on `Application` **after the asset manager and the world runner** so it destructs
before them: the mixing thread is joined before any clip or generator a voice may reference is freed.
A device-loss fallback would use the same stop-then-swap ordering; v1 handles device *loss* by
degrading to the null device (output stops, the game keeps running) and does **not** re-route to a
new device or follow a sample-rate change.

## The bus tree

`AudioBus` is a fixed enum (`Master / Music / SFX / UI / Ambience`), each a mixing group under the
master, created at init and living for the run. It is deliberately closed — no designer-authorable
graph. Each bus carries an independent gain and a small typed effect surface (gain, a one-pole
low-pass, a send into the master reverb); the master reverb is a first-party Freeverb-style node
(`Reverb.h`), since miniaudio has none, its per-block cost inside the voice budget. A voice's own
occlusion low-pass, pan, and reverb send ride the snapshot per voice.

## The voice budget

`MaxVoices` is a single device-wide, compile-time cap sizing the snapshot arrays. Every voice source
arbitrates against it. `AddVoice` takes a free slot when one exists; when the budget is full it
evicts the quietest active voice if the incoming voice is louder, and otherwise rejects the request
(an invalid handle). Real-time CPU is therefore bounded by `MaxVoices`, not by scene population.

## The null device

`AudioBackend::Null` (chosen automatically when no hardware initializes, or forced for
headless / CI / cook) has no `ma_device`. Its `Pump` runs the same `RenderBlock` mixer on the main
thread, advancing a **virtual playback clock** so finite voices still retire through the
retired-voice channel even with no output — a long scripted or CI client's one-shot pool does not
fill. Every public call succeeds and voices are tracked; only emission is skipped. Because the mixer
is pure CPU, the null backend is also what makes the whole contract unit-testable without hardware —
a test publishes a voice, calls `RenderBlock`, and reads the mixed buffer.

## The scene-facing AudioSystem

`AudioSystem` (`Veng/Audio/AudioSystem.h`) is the scene→device producer, the audio peer of the
renderer's `View<Light>` gather. It is a **View-phase `SceneSystem`**, and the phase is the point:
Sim runs fixed-step while View runs at frame rate with an interpolation `Alpha`, and the renderer
draws each mesh at its interpolated pose — so audio resolves each `AudioSource` at the **same
interpolated drawn pose** (`Scene::GetInterpolatedWorldTransform` at `SystemContext::Alpha`), and a
sound sits where its emitter is drawn rather than a partial tick ahead. It resolves the single
`AudioListener`'s live scene `Transform` (a listener at the origin when the scene has none, so
non-spatial sound still plays), differences listener and source positions frame-to-frame for the
Doppler velocities, and drives the `AudioEngine` voice table (which `Publish`es the snapshot).

**veng computes the spatialization, not miniaudio (decision D1).** The pure functions beside the
system — `DistanceAttenuation`, `StereoPan`, `DopplerRatio`, `ReverbSend` — produce the **final**
`VoiceParams` the mixer consumes, so the unit tests (`tests/unit/audio_spatial.cpp`) pin the shipped
math, not a shadow of it. A non-spatial source (`Spatial = false`) skips all of it and routes to its
bus at `Gain`. `PlayOnStart` sources begin with the simulation, looping sources persist, and a
finished non-looping source is dropped once the device reports it retired through the retired-voice
channel (surfaced by `IsVoiceLive`, never a callback into the scene). When active sources exceed the
voice cap the loudest-after-attenuation survive, a cheap partial sort matching the renderer's
`MaxLights` clamp.

**`OcclusionFactor` is an input the engine mixes, not one it computes.** The engine turns the
factor into a per-voice low-pass (`VoiceParams::Occlusion`, `0` an exact bypass); it does **not**
trace geometry to decide what occludes — the game supplies the factor from whatever it knows (a ray,
a portal test). This keeps the engine general while shipping the DSP.

The system drives `SystemContext::Audio` — the device-wide engine every system reaches, backed by
the null device when there is no hardware — so it is never inert: a headless scene simply mixes
through the null device. Each update it also calls `AudioEngine::UpdateManagedVoices` with the
resolved listener, merging the engine's code-triggered one-shots and music into the same snapshot.

## The code API and the music director

Beyond authored `AudioSource`s, any system fires sound through `SystemContext::Audio`:

- **`PlayOneShot(clip, OneShotParams)`** — a non-spatial fire-and-forget voice on a chosen bus
  (default `SFX`). **`PlayAt(clip, worldPos, SpatialOneShotParams)`** — a spatial voice fixed at a
  world position, spatialized against the listener exactly as an `AudioSource` is.
  **`SetVoicePose(handle, pos, vel)`** repositions a `PlayAt` voice each frame — the general
  capability a moving positioned voice (a projectile, a tracked remote emitter) reaches for.
- These share one **engine-owned pool** (`MaxOneShotVoices`) inside the wider `MaxVoices` budget:
  a full pool drops its quietest voice for a louder incoming one, else rejects the request. Each
  slot carries `Managed` metadata (kind, world pose, rolloff) parallel to the voice table; the
  `AudioSystem`'s per-frame `UpdateManagedVoices` re-spatializes the `Spatial` ones against the
  listener. A slot's metadata is reset on `AddVoice` / `RetireSlot`, so a reused slot never
  inherits a stale role.
- **`Music()`** returns the **`MusicDirector`**, the one-track policy over the Music bus. `Set(track,
  MusicTransition)` makes `track` the one logical background track, **equal-power crossfading** from
  the current one over `FadeSeconds` (0 is a hard cut); re-setting the playing track is a no-op. It
  holds at most the crossfade pair — two live Music voices — collapsing to one when the fade
  completes, and offers `Stop(fade)`, `SetGain`, `Current()`. It does not layer, stinger, or
  sequence. A level's authored initial track is the reflected `MusicState` component, read via
  `Scene::TryGetFirst` on world start and handed to the director once at its authored fade.

## Runtime-generated audio

Two runtime paths put code-made sound into the mix; pick by whether the sound is *finished* or
*ongoing*.

- **`AudioEngine::CreateClip(span<const f32>, AudioBufferFormat)`** — for a **finite one-shot built
  once**. It copies a code-built PCM buffer into an `AudioBuffer`, wraps it via `AudioClip::CreatePcm`,
  and `AssetManager::Adopt`s it as an `AudioClip` handle. The result is a Pcm clip in every respect
  except provenance: it plays through `PlayOneShot`/`PlayAt`, attaches to an `AudioSource`, or feeds
  the director, indistinguishable downstream from a cooked clip.
- **`IAudioGenerator` + `AudioEngine::PlayGenerator(gen, GeneratorVoiceParams)`** — for an
  **unbounded, continuously-varying voice** the mixer pulls from. The engine holds a *borrowed*
  pointer (the caller owns the generator and guarantees it outlives the voice) and, internally,
  drives it as one mono voice at the device rate; `Render` fills the samples on the mixing thread.
  Pitch (Doppler) is applied by resampling that stream, so a generator voice shares the clip
  attenuation/pan/Doppler/occlusion path with **no generator-specific case** — a spatial generator
  is a `Spatial` `Managed` voice moved each frame with `SetVoicePose` (position is a placement, never
  a generator param). A generator voice never retires by exhaustion; only `StopVoice` removes it.

> **`IAudioGenerator::Render` runs on the real-time mixing thread. It must be lock-free,
> allocation-free, and call no engine API — the same contract the snapshot bridge rests on. Live
> synthesis state reaches it *only* through a `GeneratorParams<T>` block (`Set` on the main/View
> thread, `Get` inside `Render`), never a direct call into `Render` from another thread.**

`GeneratorParams<T>` is the plan-00 triple buffer (`TripleBuffer.h`, now a public header) scoped to
one voice's POD parameters, so `Set` and `Get` at unrelated rates never tear and the RT reader never
spins. **Reclaiming a generator is the reclamation handshake made synchronous:** `StopVoice` on a
generator handle removes it from the live snapshot, publishes, and returns only once the callback's
consumed serial has passed that frame — after which the caller may free the borrowed generator with
no use-after-free (on the null device `StopVoice` drives the mixer itself, there being no RT thread).

## The self-test tone

`GenerateSelfTestTone` builds a short, faded sine burst — a bounded, finite mono buffer.
`AudioDevice::RunSelfTest` registers it as a one-shot; `Application` arms it only when
`VENG_AUDIO_SELFTEST` is set, so it is a diagnostic, not a chime on every launch.
