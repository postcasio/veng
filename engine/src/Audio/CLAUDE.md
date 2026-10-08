# Veng/Audio — the audio subsystem

The engine's sound: an output device wrapped behind the Native idiom, a fixed bus-tree mixer, and a
real-time mixing path fed by an immutable voice snapshot. It plays PCM buffers through buses with
per-bus gain and a small effect surface, degrades to a silent null device when there is no hardware,
and reclaims the resources a voice references without ever touching them while the mixer is mid-mix.

The public surface is `engine/include/Veng/Audio/` (`AudioBus.h`, `AudioBuffer.h`, `Voice.h`,
`AudioDevice.h`, `AudioEngine.h`, `ScopedAudio.h`); the real-time internals are `engine/src/Audio/`.
`AudioDevice` is owned by `Application` for the whole run, mirroring `Renderer::Context`, and is
constructed over the application's `PresentationScopes` registry, which every voice is judged by (see
[Every voice belongs to a scope](#every-voice-belongs-to-a-scope)).

## The one thread rule this subsystem adds

veng's rule is that the render thread is single. The audio subsystem is the **one sanctioned
exception**, and it earns it by isolation:

> **The real-time callback thread reads the immutable triple-buffered snapshot and mixes. It calls
> no engine API and touches no engine state — no Scene, no Asset, no Renderer, not `AudioEngine`
> itself.** Everything the engine assumes about single threading stays true because nothing outside
> the audio subsystem shares mutable state with that thread.

The engine-external threads are **two**: miniaudio's playback callback thread (which runs
`RenderBlock`), and the **engine-owned decode thread** that keeps streaming voices' PCM rings filled
(`DecodeThreadMain`, see [Streaming voices](#streaming-voices)). Neither reaches engine state — the
callback reads one POD `AudioFrame` and writes the output buffer; the decode thread touches only the
stream voices' lock-free rings and their decoders. Both are stopped and joined before any resource a
voice references is freed.

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
into an `AudioBuffer`'s PCM (or a generator, or a stream voice's ring). The callback publishes the
serial of the frame it is mixing (`ConsumedSerial`); when a voice is stopped, retired, or evicted,
its source is moved to a main-thread deferred-free queue tagged with the last serial that referenced
it, and released only once `ConsumedSerial` has passed that serial. So a source can never be freed
while the callback is mid-mix on it — the contract every voice-lifetime story rests on
(`AssetManager` clip eviction, ship-despawn teardown). A **stream voice adds a second party** to the
handshake — the decode thread also references it — so its free waits on both the mixer's consumed
serial *and* the decode thread's release ack (see [Streaming voices](#streaming-voices)); a
**buffered generator** adds the fill thread the same way. **Stopping a voice never blocks the
caller** — whatever the source, `StopVoice` only retires the slot and queues the source, and the
release happens later inside `Pump` (`CollectDeferred`), on the main thread. That is why the engine
owns a reference to every source a voice reads, a generator included: a source the caller could free
on return would force the stop to wait for every audio thread first.

**A voice snapshot carries a `Held` flag** — one more POD field. The mixer skips a held voice
outright, before its cursor, its generator or its ring is touched: no samples, no cursor advance, no
`Render` call, no drain of a stream or buffered ring (the decode and fill threads find the ring full
and wait). So a held voice resumes on the exact sample it stopped on, and a held finite voice cannot
retire. A **muted** voice is not held: it is published at zero gain and mixes on, advancing.

The reverse channel is a lock-free SPSC ring (`SpscRing.h`): when a finite voice exhausts, the
callback posts its `{slot, generation}` and the main thread drains it in `DrainRetired()` — the
callback learns nothing about the scene, and the scene learns of a retirement without a callback
into it.

## Shutdown / device-loss join order

`~AudioDevice` **stops and joins both engine-external threads before any member destructs**: the
**decode thread first** (set `DecodeStop`, notify its condition variable, join), then the **callback
thread** (`ma_device_uninit`). Once both are quiesced the engine's deferred sources — buffers,
generators, and the stream voices carrying decoders and clip handles — release safely as members
destruct. The member is declared on `Application` **after the asset manager and the world runner** so it destructs
before them: neither mixing nor decoding is running before any clip, generator, or decoder a voice
references is freed. Device-loss uses the same stop-then-quiesce ordering; the null-device fallback
handles device *loss* by degrading to the null backend (output stops, the game keeps running) and
does **not** re-route to a new device or follow a sample-rate change.

## The bus tree

The bus set is a **data-driven graph** the game authors, not a fixed enum. A voice is tagged with a
**`BusId`** — the interned hash of a bus-name string, constructible without the graph
(`BusId{"Engines"}`); the well-known root names (`Master / Music / SFX / UI / Ambience`) are engine
constants (`AudioBuses::Master()` …) that resolve only if the active graph declares them. A game's
complete topology is a cooked **`AudioBusGraph`** asset (`AudioBusGraphData`, a
`vector<AudioBusDef>` — each `{ Id, Parent, DefaultGain, LowpassCutoff, ReverbSend }`), adopted at
boot through **`AudioEngine::ConfigureBusGraph`** (named by `ApplicationInfo::AudioBusGraph`,
mirroring the graphics schema). An authored graph is the **complete** topology and **replaces** the
engine default; the engine does not inject its roots into it. With no authored graph the engine runs
a built-in **roots-only default** (`Master ⊃ {Music, SFX, UI, Ambience}` at unity), so the editor and
both examples get the roots free. A graph is validated at adoption — exactly one root and it is
`Master`, every parent resolves, acyclic, depth ≤ `MaxBusDepth`, count ≤ `MaxBuses`, no interned-hash
collision — and a rejected graph falls back to the default with a logged error. An id absent from the
active graph resolves to **`Master`** with a one-time warning.

Each bus carries an independent gain and a small typed effect surface (gain, a one-pole low-pass, a
send into the master reverb); the master reverb is a first-party Freeverb-style node, since miniaudio
has none, its per-block cost inside the voice budget. **Gain composes down the tree** (an SFX-parent
gain scales all its children); **per-bus DSP is leaf-only** — a low-pass or reverb-send on a bus with
children would filter that summed mix (hierarchical DSP routing, out of scope), so
`SetBusLowpassCutoff` / `SetBusReverbSend` on a non-leaf bus is ignored. A voice's own occlusion
low-pass, pan, and reverb send ride the snapshot per voice.

**The tree lives only on the control thread; the RT callback reads a flat table.** `SetBusGain` and
`ConfigureBusGraph` write control-thread state, and the once-per-frame `Publish()` flattens the tree
into fixed-capacity POD arrays (`[MaxBuses]`) in the snapshot: a **deterministic, index-stable
child-before-parent order** (Master last), each bus's parent index, gain, and leaf-only DSP. Each
voice's `BusId` is resolved to its leaf-bus **index** on the control thread at `Publish` (unknown ⇒
the Master index), so the RT does no lookup. The RT-owned accumulators and per-bus filter state are
sized to **`MaxBuses` once in `PrepareScratch`** and never resized by `ConfigureBusGraph` (resizing a
vector the RT reads would race). The RT **fold** is one bounded
pass — sum each voice into its leaf index, then fold each bus (through its low-pass, scaled by its
gain) into its parent index, Master last as the output — allocation-free, recursion-free, no tree
walk. Gain composition is a consequence of the fold.

**The reverb is a public, embeddable effect (`Veng/Audio/Reverb.h`), used two ways.** It is one
`Reverb` class — a bank of feedback-comb and all-pass filters expressed on `Dsp::DelayLine` (and
`Dsp::Lfo` for the High-quality tap modulation) — driving both the mixer's master node
(`AudioDevice`'s `Native::MasterReverb`) and any generator that embeds one to wet only the sub-mix
it chooses; there is no second implementation. The contract is `Prepare(sampleRate)` off the RT
thread (the one allocating call — it sizes the banks to the maximum) and `ProcessBlock(send, wetL,
wetR, frames, params)` on it (**no lock, no alloc**), reading a mono send and writing a stereo wet
pair. `ReverbParams::Quality` (`Low / Standard / High`) trades CPU for tail density: fewer filters
at Low for a cheap send, the classic 8-comb/4-all-pass bank at Standard, and the full bank with
slow LFO-modulated comb taps at High for a denser, less-ringing tail. **Standard reproduces the
classic configuration exactly**, so the master node's default behaviour is unchanged; because the
banks are sized to the maximum in `Prepare`, switching quality between blocks stays allocation-free.

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
fill. The decode thread runs regardless of backend (it is not gated on "is there a device"), so a
finite **stream** voice fills its ring, drains through the null `Pump`, and retires headless too. Every public call succeeds and voices are tracked; only emission is skipped. Because the mixer
is pure CPU, the null backend is also what makes the whole contract unit-testable without hardware —
a test publishes a voice, calls `RenderBlock`, and reads the mixed buffer.

## The driven device and the block tap

**`AudioDevice::SetDriven` puts a *hardware* device on that same path.** A caller whose frame clock
is not paced by real time (`Time::Drive`) stops the hardware — `ma_device_stop` is synchronous, so
the callback thread is quiescent for the whole span and the main thread is the mixer's only caller,
which is what keeps the one thread rule intact — and each `Pump` then mixes exactly
`delta × sampleRate` frames itself. The `kMaxPumpFrames` clamp applies only in wall mode: it bounds a
*measured* delta (a stalled frame, a breakpoint), and clamping an exact driven delta would silently
mix short. Nothing is emitted for the span, and a `ma_device_start` that fails on release degrades to
the null backend with a logged error, the same device-loss policy as everywhere else.

**No main-thread call waits on the mixer.** A null device has no mixing thread and a driven
device's is stopped, so a wait that spins until the callback thread consumes a generation would wait
for itself on either. Reclamation therefore polls rather than waits: `CollectDeferred` runs inside
`Pump`, after that pump's own mix, and frees whatever the consumed serial has passed. A new wait on
the consumed serial would have to drive the mixer itself on a null or driven device; the GPU tier
pins that `StopVoice` returns on a driven hardware device.

**The block tap is the seam a consumer takes the mix through.** `SetBlockTap` installs one sink that
receives every mixed block on the main thread, in order: `Pump` hands it the block directly on a null
or driven device, and a running hardware device's callback pushes each block into an `SpscRing` of
fixed POD blocks (`AudioDevice::TapBlockFrames` frames × at most `MaxTapChannels`, since the backend
reuses its output buffer the instant the callback returns) that `Pump` drains. A full ring drops the
**newest** block and counts it in `GetTapOverruns()`, so the mixing thread never blocks and a
consumer that must stay aligned substitutes silence for the loss. With no tap installed the mixing
thread reads one atomic and does nothing else.

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

The system drives `SystemContext::Audio` — the facade bound to its scene's presentation scope, over
the null device when there is no hardware — so it is never inert: a headless scene simply mixes
through the null device. Its source voices therefore belong to the scene: a **paused** world runs no
View pass, so its scope is `Held` and its sources freeze where they are, with no system code; an
**unpresented** world's are muted; a **closed** scene's stop. Each update it also sets the scene's
listener on that scope (`ScopedAudio::SetListener`), which is what the scene's `PlayAt` voices are
spatialized against too, and submits the scene's `MusicState` as that scope's music request (see
[The code API and the music director](#the-code-api-and-the-music-director)). `OnStop` still stops
the voices it holds and withdraws the request; the scope's closure would also.
It does not advance the engine — that is the application's once-per-frame `AudioEngine::Update`.

## Every voice belongs to a scope

**Every voice carries the `PresentationScopeId` it was started in**
([../Scene/CLAUDE.md](../Scene/CLAUDE.md), "Presentation scopes"), so pause, presentation, closure
and replay fall out of the scope's states with no world ids. The engine's starting calls take the
scope explicitly — `AddVoice(scope, …)`, `AddClipVoice(scope, …)`, `PlayOneShot(scope, …)`,
`PlayAt(scope, …)`, `PlayGenerator(scope, …)` — and refuse to start in a scope that has closed (or
was never handed out); the handle-keyed controls (`SetVoicePose`, `SetVoiceParams`, `StopVoice`,
`IsVoiceLive`, `GetVoiceParams`) take none. A voice is judged at its start, so one started in a `Held`
scope is published held from its first snapshot and stays fresh until the lease returns.

**`AudioEngine::Update(delta)` runs once per frame**, in `Application::Frame`'s presentation step right
after `PresentationScopes::Resolve()` — never from a system, so nothing advances once per world. Per
voice, by its scope's latched state: `Closed` stops it through the ordinary retire-and-reclaim path
(so a world's end stops its one-shots, generators and source voices alike), `Held` publishes it
frozen, `Muted` publishes it at zero gain while it advances (a muted one-shot ends exactly when it
would have ended audibly), `Live` publishes it as authored. Then every spatial voice is
re-spatialized against **its own scope's** listener, the music requests are arbitrated (see
[The code API and the music director](#the-code-api-and-the-music-director)), and the music director
advances once by the frame delta.

**A listener per scope.** `SetListener(scope, pose)` keeps one pose per open scope (dropped by the
`Update` that finds the scope closed); a scope that never set one — the application scope — listens
from the origin. A `PlayAt` voice is spatialized against the listener of the scene that started it,
whatever order the worlds tick in.

**`Audio::ScopedAudio` is what a scene reaches** (`Veng/Audio/ScopedAudio.h`): the engine, the scope
and the replay flag, held by value as `SystemContext::Audio` and bound by the context factory, so a
system never names a scope. Its starts (`PlayOneShot`, `PlayAt`, `PlayGenerator`, `AddClipVoice`) go
into its scope and **start nothing inside a reconciliation replay** — a Sim system fires sound with
no `IsReplay` gate of its own — while its controls work as usual in a replay. `SetListener` sets the
scope's listener, and `SetMusicRequest` sets or withdraws its scope's standing music request (state,
not a start, so not replay-gated). `CreateClip` and `GetOutputSampleRate` pass through unscoped (a
clip is an asset, not a voice). Bus gains, bus DSP and the master reverb are device-wide settings,
not a scene's sound, so they stay on the engine (`Application::GetAudioEngine()`). An unbound facade
(`ScopedAudio::Unbound()`) starts nothing; it has no public default, so a context omitting its facade
does not compile. **`Application::GetApplicationAudio()`** is the same facade over the always-`Live`
application scope — application code, tooling and an editor's audition — and never a scene system's
path. A Gui driver reaches the same facade as `GuiDriverFrame::Audio`, built by its presenting
viewport over the driven scene's scope (see [../Gui/CLAUDE.md](../Gui/CLAUDE.md), "The driver"), so a
HUD's click in a paused world is held with that world. A unit test takes its contexts from
`tests/support/TestServices.h`, whose null device is judged by the bundle's own registry.

## The code API and the music director

Beyond authored `AudioSource`s, any system fires sound through `SystemContext::Audio`:

- **`PlayOneShot(clip, OneShotParams)`** — a non-spatial fire-and-forget voice on a chosen bus
  (default `SFX`). **`PlayAt(clip, worldPos, SpatialOneShotParams)`** — a spatial voice fixed at a
  world position, spatialized against its scene's listener exactly as an `AudioSource` is.
  **`SetVoicePose(handle, pos, vel)`** repositions a `PlayAt` voice each frame — the general
  capability a moving positioned voice (a projectile, a tracked remote emitter) reaches for.
- These share one **engine-owned pool** (`MaxOneShotVoices`) inside the wider `MaxVoices` budget:
  a full pool drops its quietest voice for a louder incoming one, else rejects the request. Each
  slot carries `Managed` metadata (kind, world pose, rolloff) parallel to the voice table; the
  engine's per-frame `Update` re-spatializes the `Spatial` ones against their scope's listener. A
  slot's metadata is reset on `AddVoice` / `RetireSlot`, so a reused slot never inherits a stale
  role.
- **Music is requested, not set.** Each presentation scope holds at most one standing
  **`MusicRequest`** (`Track`, `FadeSeconds`, `Loop`, `Priority`) — set or withdrawn through
  `ScopedAudio::SetMusicRequest` (`AudioEngine::SetMusicRequest(scope, …)`), held until replaced or
  until the scope closes. A scene's `AudioSystem` submits its **`MusicState`** component (the first
  the scene finds; absent → withdrawn) every View update, so a runtime edit is live and a **paused**
  scene, whose View phase does not run, keeps its last request standing; application code requests
  through `Application::GetApplicationAudio().SetMusicRequest(…)`. A request with no `Track` asks
  for silence at its priority.
- **The arbitration** runs once a frame in `AudioEngine::Update`, after the scope states are latched.
  A closed scope's request is dropped. A request is **eligible** when its scope is the application
  scope, `Live` (presented and running), or `Held` **while still presented** — it carries a
  presentation rank, so it is paused on screen (or has not yet run its first View pass). A `Muted`
  scope (an open scene nothing presents) is never eligible, even alone, and neither is a `Held` one
  with no rank (a paused scene nothing presents): the director's voices are always heard, so letting
  an unpresented scene take the music would make it audible. The **winner** has the highest
  `Priority`; ties go to the lower **presentation rank** — the registration index of the first
  viewport presenting the scope's scene, 0 being the primary viewport, stamped each frame by the
  Application ([../Scene/CLAUDE.md](../Scene/CLAUDE.md), "Presentation scopes") — a ranked scope
  beating an unranked one (so the application scope loses a tie to any presented scene), and then to
  the older scope. The director crossfades to the winner's track at the winner's `FadeSeconds`
  when its track or loop differs from what plays (the same track never re-triggers), and fades out
  over the last winner's `FadeSeconds` when nothing is eligible. So closing the winning world returns
  the music to the next request the next frame, closing the last fades it out, a pause neither
  restarts nor stops the track, an overlay outranks the world it covers only by its `Priority`, and a
  world opened in the background never takes the music. `GetMusicRequests()` and `GetMusicWinner()`
  expose the standing requests and the winner for tooling. A track the director crossfades back to
  restarts from its beginning.
- **`Music()`** returns the **`MusicDirector`**, the one-track policy over the Music bus the
  arbitration drives (its `Set`/`Stop` are engine-internal). It **equal-power crossfades** from the
  current track over the transition's fade (0 is a hard cut), holds at most the crossfade pair — two
  live Music voices — collapsing to one when the fade completes, starts a track whose clip was still
  loading when it became current once the clip is resident, and offers `SetGain`, `Current()` and
  `GetVoiceStates()` for the music volume and inspection. It does not layer, stinger, or sequence.
  The director is device-wide: its voices belong to the application scope, and its crossfade advances
  in the engine's once-per-frame `Update`.

## Runtime-generated audio

Two runtime paths put code-made sound into the mix; pick by whether the sound is *finished* or
*ongoing*.

- **`AudioEngine::CreateClip(span<const f32>, AudioBufferFormat)`** — for a **finite one-shot built
  once**. It copies a code-built PCM buffer into an `AudioBuffer`, wraps it via `AudioClip::CreatePcm`,
  and `AssetManager::Adopt`s it as an `AudioClip` handle. The result is a Pcm clip in every respect
  except provenance: it plays through `PlayOneShot`/`PlayAt`, attaches to an `AudioSource`, or feeds
  the director, indistinguishable downstream from a cooked clip.
- **`IAudioGenerator` + `PlayGenerator(gen, GeneratorVoiceParams)`** (through a facade) — for an
  **unbounded, continuously-varying voice** the mixer pulls from. `PlayGenerator` takes a
  `Ref<IAudioGenerator>` and the voice shares ownership of it: the caller keeps its own reference
  to drive the generator's params, or drops it whenever it likes; `Render` fills the
  samples on the mixing thread at the device rate. Pitch (Doppler) is applied by resampling that
  stream, so a generator voice shares the clip attenuation/pan/Doppler/occlusion path with **no
  generator-specific case** — a spatial generator is a `Spatial` `Managed` voice moved each frame
  with `SetVoicePose` (position is a placement, never a generator param). A generator voice never
  retires by exhaustion; only `StopVoice` (or its scope's closure) removes it, and a held one is not
  rendered at all.

  **A generator voice is mono by default, and a non-spatial one may declare stereo.** A voice
  registers its width through `GeneratorVoiceParams::Channels` — `1` (mono) or `2` (an interleaved
  stereo image). A **mono** voice renders one stream that the pan stage fans to stereo, exactly as a
  clip does. A **stereo** voice's two channels are *authored* by the generator: they are summed
  straight into the stereo bus, **bypassing the pan** (a non-spatial voice has no distance and no pan
  to compute — its stereo image is what the generator draws), resampled per channel through the base
  `Pitch` only, and folded to the mono reverb send as `(L + R) × 0.5 × ReverbSend`. Stereo is
  **non-spatial only**: spatialization is a mono-source-then-pan model, so a stereo point source is
  undefined and `PlayGenerator` **rejects** a `Channels == 2 && Spatial` request with an invalid
  handle. The spatial path is untouched and mono by construction.

> **`IAudioGenerator::Render` runs on the real-time mixing thread. It must be lock-free,
> allocation-free, and call no engine API — the same contract the snapshot bridge rests on. Live
> synthesis state reaches it *only* through a `GeneratorParams<T>` block (`Set` on the main/View
> thread, `Get` inside `Render`), never a direct call into `Render` from another thread.**

`GeneratorParams<T>` is the snapshot's triple buffer (`TripleBuffer.h`, a public header) scoped to
one voice's POD parameters, so `Set` and `Get` at unrelated rates never tear and the RT reader never
spins. **A generator is reclaimed like any other source:** `StopVoice` (or an eviction) retires the
slot and moves the engine's reference into the deferred-free queue, released once the consumed
serial passes the last frame that named it — and, for a buffered voice, once the fill thread has
acknowledged the removal, the reference riding the `BufferedGenerator` wrapper it renders through.
The release happens on the main thread, so a generator's destructor never runs on the real-time or
fill thread, and whichever reference goes last destroys it: a caller still holding one keeps the
generator alive past the voice. The flip side is that **a stopped generator may still be rendered
until it is reclaimed**, so a caller holding a reference changes it only through its
`GeneratorParams` block, even after the stop; to restart with different state, start a new generator.

## The Dsp primitive library

`Veng/Audio/Dsp/` is the reusable **synthesis parts** a consumer composes to *invent* a sound
inside its own `IAudioGenerator`, rather than hand-rolling oscillators and filters. It ships the
**parts, not an instrument** — there is deliberately no `SynthVoice`, preset patch, or routing
graph, because how the parts wire into a playable voice is a taste that belongs in the consumer.
The public surface is one header per primitive under `engine/include/Veng/Audio/Dsp/` plus the
umbrella `Veng/Audio/Dsp.h`; every primitive is **header-inline** (a phase accumulator, a pair of
integrators, a ring index — no `.cpp` earns its keep) and lives in namespace `Veng::Audio::Dsp`.

- **`Oscillator`** — a band-limited oscillator whose waveform is a **continuous shape** parameter
  (sine → triangle → saw → square as one axis), so a consumer interpolates *between* timbres. The
  non-sine archetypes are **PolyBLEP** band-limited (PolyBLAMP for the triangle's slope corners) so
  a swept oscillator does not alias.
- **`Noise`** — white and pink from one seeded xorshift PRNG; a fixed seed reproduces a sequence.
- **`Envelope`** — an ADSR advancing by **sample count** (`NoteOn`/`NoteOff`, `Tick`/`Advance`),
  segment lengths set in samples (a seconds setter converts), exposing `IsActive` to retire a voice.
- **`Filter`** — a resonant **TPT (Zavalishin) state-variable filter**: LP/HP/BP/notch from one
  core, stable under per-sample cutoff modulation — the response the bus/occlusion one-pole cannot
  give. The one-pole stays where it is; this SVF is the synthesis filter.
- **`Lfo`** — the same morphable shape at sub-audio rate (unipolar/bipolar, settable phase),
  un-band-limited by design.
- **`DelayLine`** — a fractional-read ring buffer sized once in `Prepare`; `Write`/`Read`/
  `ReadInterpolated` are allocation-free. The storage a comb, all-pass, echo, or chorus is built on.
- **`Smoother`** — a one-pole control-rate slew easing a stepped parameter (a cutoff jump, a gain
  change) so it does not zipper.
- **`CustomSource` / `CustomFilter`** — the escape-hatch nodes for DSP that maps onto no standard
  primitive, first-class beside `Oscillator` and `Filter`.

**The RT contract each obeys.** A primitive a generator invokes inside `Render` inherits the
mixing-thread contract exactly: it is **lock-free, allocation-free, and calls no engine API**. It
**advances purely by the sample count it is handed** (`Tick` per sample, `frames` variable — the
mixer chunks at `kMaxChunkFrames` and asks for the resampled source-sample count, never a fixed
block), so it makes no block-size assumption. The **only** allocation is `DelayLine::Prepare`, run
off the RT thread before use. Everything is CPU-testable with no device (`tests/unit/audio_dsp.cpp`
asserts frequency, band-limiting, envelope segment shape, filter response and modulation stability,
noise character/determinism, delay readback, and smoother approach as properties).

**A `CustomSource`/`CustomFilter` callable is bound off the RT thread, while its live parameters
still cross only through `GeneratorParams`.** A `function` is not POD and may allocate when
constructed, so it can never cross the triple-buffered param block — it is bound **once**, at the
point the generator's node graph is built. *Invoking* an already-constructed `function` allocates
nothing, so the call itself is RT-safe. The bound callable's **live knobs still flow through the
param block** like every other node's: the generator latches its `GeneratorParams<T>` once at the
top of its own `Render`, stores the latched values in plain members, and the callable — which closed
over a pointer to that state at construction — reads them. Only the *code* is fixed at build time,
never the parameters.

**The worked example is `examples/template`'s `DemoSynth`.** It composes the primitives into one
`IAudioGenerator` — two `Oscillator`s a fifth apart, spread across a stereo pair, through a resonant
`Filter` low-pass (cutoff eased by a `Smoother`), amplitude-shaped by an `Envelope`, into an
**embedded `Reverb`** (`Prepare`d once off the RT thread, `ProcessBlock` wetting only this voice) —
registered non-spatial **stereo** (`GeneratorVoiceParams{ .Channels = 2, .Spatial = false }`) on the
Music bus and driven live by a clock nudging the cutoff through a `GeneratorParams` block. It exercises
the primitive library, the public reverb, and the stereo generator path in one place, out-of-tree via
`find_package(veng)` — the consumer proof the toolkit is held to. It is a **demonstrator of
composition, not a synth to reuse**: the oscillator count, the interval, and the routing are the
example's arbitrary taste, not an engine opinion.

## Streaming voices

A **stream voice** is the third voice source beside a resident PCM buffer and an `IAudioGenerator`,
and it is how an **Encoded** (Vorbis) `AudioClip` plays: long music and ambient beds stay encoded in
memory and are decoded **incrementally at play time** rather than resident as PCM. Downstream it is
indistinguishable from a resident voice — it attenuates, pans, Dopplers, occludes, loops, and
crossfades identically — because only the *source* of samples differs, never the mix.

The shape is a producer/consumer split across the thread boundary, so the RT contract is never
touched:

- **The RT callback only drains.** Each stream voice owns a per-voice lock-free **SPSC ring of mono
  PCM** at the clip's sample rate (`StreamVoice::Ring`, `SpscRing<f32>`), the decode thread the sole
  producer and the callback the sole consumer. `RenderStreamChunk` pops samples through the **same
  two-sample resampling window the generator path uses** (there is no second resampler) — the clip →
  device rate conversion and any Doppler pitch ride that one window. An empty ring is an **underrun**:
  the callback fills the block's remaining frames with silence and freezes the window so playback
  resumes cleanly when samples return — it never blocks and never allocates.
- **The decode thread fills.** One engine-owned thread (`DecodeThreadMain`) owns every active stream
  voice's `VorbisMemoryDecoder` and, each pass, tops up any ring with room to spare
  (`TopUpStream` — decode only what fits so nothing decoded is dropped, collapse each frame to mono,
  push), then sleeps on a condition variable until woken. It learns of voices through an **ordered
  command ring** (`SpscRing<StreamCommand>`): the main thread posts an `Add` when a stream starts and
  a `Remove` when it retires. It reaches no engine state — only the rings and the decoders — so it is
  the second sanctioned engine-external thread beside the callback.
- **Looping is off-RT and seamless.** When a looping stream's decoder reports its end, the decode
  thread `SeekStart`s it and keeps filling the same ring with the head samples, so the loop point
  carries no gap. A finite (non-looping) stream instead sets `AtEnd`; the callback drains the ring,
  sees `AtEnd`, and retires the voice through the existing lock-free retired-voice channel.
- **Reclamation is the dual handshake.** A stream voice is referenced by two threads, so freeing it
  waits on both: the mixer's `ConsumedSerial` must pass the last frame that named its ring (the
  serial handshake every voice rides), **and** the decode thread must acknowledge the `Remove` by
  setting `ReleasedByDecoder`. Only then does `CollectDeferred` free the `StreamVoice` — which holds
  the decoder and an `AssetHandle<AudioClip>` pinning the clip's bytes — so `AssetManager` clip
  eviction and a director track-change are use-after-free-safe.

`AudioEngine::AddClipVoice` is the entry point that chooses the path by storage: a `Pcm` clip plays
through `AddVoice` off its resident buffer, an `Encoded` clip through `AddStreamVoice`. Every clip
consumer routes through it — the `AudioSystem`'s authored `AudioSource`s, `PlayOneShot`/`PlayAt`, and
the `MusicDirector` — so a stream clip is accepted anywhere a resident one is. Music is the expected
stream case (the music director streams an Encoded track and crossfades it against any mix of
stream and resident tracks); a long authored ambient bed on an `AudioSource` is the other.

## The self-test tone

`GenerateSelfTestTone` builds a short, faded sine burst — a bounded, finite mono buffer.
`AudioDevice::RunSelfTest` registers it as a one-shot; `Application` arms it only when
`VENG_AUDIO_SELFTEST` is set, so it is a diagnostic, not a chime on every launch.

## Inspecting the mix

`AudioEngine::GetVoiceInfos()` returns a read-only `vector<VoiceInfo>` — one record per live voice,
in slot order — carrying each voice's handle, bus, final gain/pan/pitch/occlusion/reverb-send, its
loop flag, whether it is a clip or a generator, its role (`VoiceOrigin`:
source/one-shot/spatial/music), its owning `Scope` and that scope's `State` as the voice was last
judged (held is frozen, muted is silent), and — for a spatial voice — the world position and velocity
the engine holds. It takes no lock and mutates nothing: a seam for tooling, not for the mix.

The MCP surface exposes it as **`audio.list_voices`** (`veng/mcp`, `src/AudioTools.cpp`): a read-only
tool reporting every voice on the device with its scope and state, plus the music director's current track, the winning
scope and every scope's standing music request, so a driven
session confirms "the right things are playing" without a speaker. It reaches the engine through
`McpHost::Audio` and runs at the frame pump point like the other engine tools; see
[mcp/CLAUDE.md](../../../mcp/CLAUDE.md). The engine's own consumption exemplars wire the whole
subsystem — an authored looping `AudioSource` and an `AudioListener`, a code-triggered `PlayOneShot`
through `SystemContext::Audio` (owned by its scene and replay-gated with no code of its own), an
authored `MusicState`, and a trivial `IAudioGenerator` + `CreateClip` demo through
`GetApplicationAudio()` — in `examples/hello-triangle`; `examples/template` takes the minimal
listener + source plus the composed `DemoSynth` above (also application-scoped), the consumer proof a
new capability is held to.
