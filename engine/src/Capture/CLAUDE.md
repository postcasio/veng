# Capture — recording the presented frame to a video file

`Veng::Capture` is one service: **`VideoRecorder`**, an `Application` member that records what the
application presents to a video file through the platform's hardware encoder, with **no CPU copy**.
Its public surface is `Veng/Capture/VideoRecorder.h`; everything else in this directory is internal.

The recorder exists in **every build configuration and on every platform**. Where the platform has
no encoder behind a shareable surface, the class is unchanged and `IsAvailable()` is false — no
preprocessor conditional reaches above this directory.

## The shape

```
Application::Frame
  ViewportCompositor::Composite(cmd)
     SwapChainCompositePass  → the swap chain image                     (the presented frame)
     CaptureSink::AcquireTarget(slot, extent) ──► VideoRecorder
                                                    RecorderCore::Acquire(slot)
                                                      VideoRecorderBackend::AcquirePixelBuffer()
                                                    Backend::ImportExternalTexture(texture)
     SwapChainCompositePass  → that imported image, in the sink's colour space
  Context::BeginFrame — slot retired ──► VideoRecorder::OnSlotRetired(slot)
                                            RecorderCore::Retire(slot)
                                              AppendVideo(buffer, pts); ReleasePixelBuffer(buffer)
```

Three layers, and the split between them is what makes the thing testable:

- **`VideoRecorder`** (`VideoRecorder.cpp`) is the only layer that knows about the renderer, the
  audio device and the host. It implements `Renderer::CaptureSink`, imports each buffer, installs and
  removes the audio block tap, drives the host's frame clock for a lockstep capture, and subscribes to
  swap-chain invalidation.
- **`RecorderCore`** (`RecorderCore.{h,cpp}`) is **device-free**: the settings validation, the slot
  map, both timestamp modes, the never-drop wait, the drain, and the reported state. It talks to a
  `VideoRecorderBackend` and to nothing else, so the unit band drives all of it against a fake backend
  and an injected clock.
- **`VideoRecorderBackend`** (`VideoRecorderBackend.h`) is the platform seam, dealing only in opaque
  handles — no Objective-C, AVFoundation or Core Video type reaches the layers above.
  `VideoRecorderApple.mm` implements it over `AVAssetWriter` and VideoToolbox;
  `VideoRecorderStub.cpp` is everywhere else and reports the backend absent.

## The frame's route, and why it rides slot retirement

The capture composite writes the pixel buffer **on the frame command buffer**. The encoder may read
that memory only once the command buffer's fence has been waited, which `Context` does when the frame
slot comes round again, `GetMaxFramesInFlight()` frames later — the same contract `AsyncReadback`
rides. So every frame:

1. `AcquireTarget` takes a **fresh** buffer from the writer's pool, imports it, records `slot → {buffer,
   timestamp}`, and hands back the imported image.
2. `OnSlotRetired(slot)` — from `BeginFrame`, right after that slot's fence wait — appends the buffer
   with its timestamp and **releases the recorder's reference**.

The release is load-bearing. The pool vends a surface again only once *both* the encoder and the
recorder have let go of the buffer holding it, which is exactly why the recorder must never hold
buffers across frames: a handful of surfaces then serve a whole capture. Imports are cached by the
buffer's **surface**, so a recycled surface pays the import once rather than every frame it returns.

## Never drop, but never hang

Before taking a buffer the backend checks the writer's readiness and the pool's allocation threshold
(`GetMaxFramesInFlight() + 4` buffers, which at 4K is some thirty megabytes each — unbounded surface
growth is the alternative). Either refusal makes the frame **wait on the main thread**, so a real-time
capture slows the application rather than skipping a picture.

The wait is **bounded at two seconds** (`RecorderCore::EncoderWaitBoundSeconds`) and every iteration
re-asks the backend, which checks the writer's status. A failed writer ends the capture with its own
words; a wait past the bound ends it with `encoder stalled`. Both drain the file intact to the last
appended frame. The bound exists because the wait happens **inside `Composite`, with the swap chain
image already acquired** — an unbounded one would become a presentation stall with the Stop control
unreachable. `FramesWaited` and `WaitedForEncoderMs` surface the cause.

## Stop never waits for a future frame

Retirement fires from `BeginFrame`, and after `Stop` there may be no more frames at all — the user
quit, the window was minimized (the run loop parks while there is no presentable size), a scene change
stalled. So `Stop` **drains**: the `BeforeDrain` hook waits the device idle, every outstanding buffer
is appended in the order it was composited and released, the inputs are marked finished, and the
writer is asked to commit. The status is `Finalizing` until the writer's completion runs.

The completion **touches nothing the recorder owns** — it captures a shared flag and sets it — so a
recorder destroyed mid-commit is safe. `Application`'s teardown calls `Stop` and then waits for that
flag **without a bound**, because a bounded wait would leave a truncated movie; what ends a wait that
cannot complete is the writer's own failure, checked every iteration.

A capture that ends *inside* `AcquireTarget` (the wait bound, a failed writer, a failed import, a
presented frame of the wrong size) cannot remove the sink there — that would mutate the compositor
mid-composite — so it records the debt and detaches at the next slot retirement.

## Two modes, one pipeline

|  | Real time | Lockstep |
|---|---|---|
| Frame clock | wall | driven at `1/FrameRate` (`Application::DriveFrameClock`) |
| Timestamp | the wall second the buffer was taken, rebased to the capture's origin | `k / FrameRate`, `k` the recorder's **own** acquire count |
| File | variable frame rate | constant frame rate |
| Audio | the tap's callback-side blocks, drained at the next pump | exactly `round(delta × rate)` samples per frame, sample-locked |
| Writer | `expectsMediaDataInRealTime = YES` | `NO` |

`k` is the recorder's own count and not `Time::GetDrivenFrames()`: the clock's drive takes effect at
the next frame boundary, so the driven frame count would stamp the capture's first two frames zero.

Every timestamp is an integer in a fixed timescale (`VideoTimescale`, 60 000 — divisible by every
common frame rate) and is clamped to the previous plus one tick, because `AVAssetWriter` requires
strictly increasing presentation times and a float second cannot express that requirement exactly.
The session starts at `kCMTimeZero`, so the file's first frame is its own beginning.

## Audio is the mix, muxed

`Start` installs `AudioDevice::SetBlockTap`; each mixed block becomes a sample buffer of interleaved
float PCM on the file's sound track (`AudioTrack::Pcm` stores it; `Aac` lets the writer encode). Its
timestamp is the running sample count from zero, in the sample-rate timescale — the same origin as the
picture. A block the tap's ring dropped (`GetTapOverruns`) is written as **silence of exactly the lost
length**, so the sound track never drifts against the picture, and the loss is counted in the state.

A device mixing more than two channels is refused at `Start`: the writer needs a channel layout the
backend does not describe past stereo.

## Encoding is a setting, never a consequence of the display

The capture composite takes the **sink's** colour space, so the swap chain's own format never matters.

| `CaptureEncoding` | Pool format → imported format | Composite colour space | Profile | Colour tags |
|---|---|---|---|---|
| `Sdr` | `32BGRA` → `BGRA8Srgb` | `SrgbNonlinear` (the `_SRGB` store encodes) | HEVC Main / H.264 High / ProRes | BT.709, sRGB transfer, BT.709 matrix |
| `Hdr10` | `ARGB2101010LEPacked` → `A2R10G10B10Unorm` | `Hdr10St2084` | HEVC Main 10 / ProRes | BT.2020, PQ (ST 2084), BT.2020 matrix |

`Auto` resolves to `Hdr10` only on an HDR10 swap chain; an SDR **or extended-linear (EDR)** display
resolves to `Sdr`, a file most players and editors show correctly, with `Hdr10` one setting away.
`H264` with `Hdr10` is refused — the hardware encoder has no ten-bit profile. The container is always
QuickTime (`.mov`): it is the one that carries both ProRes and uncompressed float PCM.

Quality is denominated in **bits per pixel**, not megabits: the capture's extent is the framebuffer's
— six megapixels full-screen on a Retina display, half a megapixel in a small window — and one bitrate
cannot serve both. `BitrateMbps` overrides it outright.

## Any swap-chain change ends the capture

A recreation at a new extent changes the frame size under the writer, which a file cannot carry; a
same-extent recreation can move the **format or colour space**, which would change the capture
composite's input semantics under it. So the recorder subscribes to `AddSwapChainInvalidationCallback`
**once, in its constructor**, and only when `IsSwapChainCaptureSupported()` — the callback list has no
removal, and a headless context has no swap chain. The callback ends a running capture with
`swap chain changed`, draining the file intact.

## Output lives with the profiler's captures

A capture is named, never pathed: `VideoCaptureSettings::Name` is resolved under
`Diagnostics::CaptureDirectory()` (the build tree's `captures/`), and only the name's final path
component is taken, so a name carrying separators cannot escape. An empty name yields
`<app>-<yyyymmdd-hhmmss>`. This is the profiler's rule, for the profiler's reason: **no capture path is
ever a tool argument.**

## The panel is the engine's, the window is the host's

`Veng::UI::VideoCapturePanel(VideoRecorder&, VideoCapturePanelState&)`
(`Veng/UI/VideoCapture.h`) is the recorder's control surface, in the `DebugPanels.h` idiom: a free
function drawing into whatever window the caller opened, with **no window of its own and no static
state**. The host holds one `VideoCapturePanelState` — the edited `VideoCaptureSettings` plus the
name field's buffer — so a debug shell and the editor each place the panel in their own menu and
their own docking, and the engine owns no top-level window in a host's UI.

Three layouts, chosen by the status the one per-frame `GetState()` reports (that read is also what
advances a finished capture from `Finalizing` to `Off`):

- **Idle** — the settings over a **Start** button: the encoding combo, a codec combo whose
  impossible pairing is greyed with its reason in the row's own label (a disabled ImGui item never
  registers as hovered, so a tooltip on one would not appear), the bits-per-pixel drag with the
  megabit figure it derives beside it and an outright override, the lockstep checkbox with a frame
  rate enabled only under it, the frame budget, the audio combo, **Include overlay** (off, its
  tooltip saying the overlay is the application's own interface — the panel among it — while a
  world's HUD is recorded either way), the name field, and the resolved capture directory. A
  recorder that cannot record draws the reason where the button would be.
- **Recording** — the same settings drawn disabled above the live figures (codec, encoding, extent,
  frames against the budget, duration, file size, audio blocks) and the two warning rows that appear
  only when non-zero (`FramesWaited`/`WaitedForEncoderMs` as the encoder falling behind, and
  `AudioOverruns`) — over a **Stop** button, under which one disabled line states that closing the
  window does not stop the capture and that the window is not in the recording unless Include
  overlay was set.
- **Finalizing** — "Writing file…" and neither button, while the writer commits.

The last file's path is drawn in every state once one exists, and a non-empty `LastError` in the
theme's warning colour. Two pure helpers carry everything the panel does that is not a draw call, so
the unit band reaches them: `ResolveCaptureSettings(state)` folds the trimmed name buffer into the
settings a Start receives, and `CaptureBitrateMbps(settings, extent)` is `DeriveBitsPerSecond`
rounded to whole megabits — the recorder's own derivation, not a second one beside it.

## The tools are the profiler's trio, gated the same way

`veng::mcp` exposes the recorder through `McpHost::VideoRecorder`
(`function<Capture::VideoRecorder*()>`, null when the host has none):

| Tool | Gate | What it does |
|---|---|---|
| `render.capture_status` | always | Reports the state — `available`, `status` (`Off`/`Recording`/`Finalizing`), the resolved encoding, codec and bitrate, the extent, frames acquired and appended against the budget, duration, file size, the wait and overrun counters, the path, and `last_error`. |
| `render.capture_start` | `AllowMutations` | Begins a capture from an all-optional settings object, returning the state; a refusal is a tool error carrying the reason. |
| `render.capture_stop` | `AllowMutations` | Requests the stop and returns the state it leaves. |

A capture writes a file and drives the application's frame clock, so the two write verbs ride the
`AllowMutations` gate beside the profiler's capture verbs (`RegisterRenderCaptureWriteTools`, beside
`RegisterProfileWriteTools`); the status read is registered whatever the server's write posture.

Three properties of the surface follow from rules stated elsewhere and are worth naming here:

- **There is no directory argument, and an unknown key is refused.** The name resolves under
  `CaptureDirectory()` as above, so a key the schema does not name — `Directory` among them — is a
  tool error rather than a silently ignored field.
- **Validation is the handler's.** `McpServer` echoes a tool's `InputSchemaJson` into `tools/list`
  and checks nothing against it, so a zero frame rate, an unknown enumerator, a quality deriving no
  bitrate, and a codec that cannot carry the encoding are each refused by the handler.
- **`capture_stop` returns as soon as the stop is *requested*.** Finalization is asynchronous, so a
  caller that needs the file finished on disk polls `capture_status` until `status` is `Off`.

A host that leaves the resolver null — or a headless or non-Apple one that supplies a recorder that
cannot record — answers `capture_status` with `available` false and the reason stated, and refuses
both write verbs with it.

## Objective-C++ conventions in this directory

`VideoRecorderApple.mm` is **non-ARC** — the default for this tree's `.mm` sources, and what the
Vulkan implementation's unowned references require. Every object held across a call is retained by
hand and released in the destructor. The TU takes `SKIP_PRECOMPILE_HEADERS` (the engine PCH is a C++
header an Objective-C++ TU cannot consume) and links `AVFoundation`, `CoreMedia`, `CoreVideo`,
`VideoToolbox`, `Metal` and `IOSurface`.

Two platform facts the backend is written around, both measured rather than assumed:

- **Re-read `adaptor.pixelBufferPool` on every acquire.** The adaptor builds it lazily and has been
  seen to replace it mid-run; a stale pool vends buffers of the wrong shape with no error at all. The
  pixel format of every vended buffer is asserted.
- **The allocation threshold is an *auxiliary* attribute**, passed to
  `CVPixelBufferPoolCreatePixelBufferWithAuxAttributes`, not a pool attribute.
  `kCVReturnWouldExceedAllocationThreshold` is the back-pressure the never-drop wait rides.

## What the band proves and what only the live look can

**Every context in the test band is headless, and the capture composite runs only windowed.** That
line is the whole of what the band can and cannot reach, so it is worth being exact about:

**The band proves:**

- `tests/unit/video_recorder_core.cpp` — the slot map trailing the frames in flight by at most `N`;
  every buffer released exactly once, after its append; both timestamp modes including the
  strictly-increasing clamp; `Stop` draining with no further frame; the frame budget; the wait bound;
  a failed writer's words; an abort's reason; a second `Start` refused; the settings validation and
  the derived bitrate.
- `tests/unit/video_recorder_audio.cpp` — a driven pump's own samples reaching the file, contiguous
  from zero, and a dropped block written as silence of exactly its length.
- `tests/gpu/video_recorder_apple.mm` — the backend and the import end to end against a real file:
  the pool vends buffers the renderer can import and render into, they encode, and the movie that
  comes back carries ten frames of the right colours, a 48 kHz sound track of the right length, and —
  for `Hdr10` — HEVC Main 10 with BT.2020 primaries and PQ. Skips with a message where the session has
  no hardware encoder.
- `tests/gpu/video_recorder_unavailable.cpp` — a headless recorder constructs without touching a swap
  chain, reports itself unavailable, and refuses to start with a reason.
- `tests/unit/video_capture_panel_state.cpp` — the panel's two pure helpers: a typed name (trimmed,
  and empty when blank) is the name a Start receives, and the megabit figure is `DeriveBitsPerSecond`
  at every extent. Nothing is asserted about layout or draw.
- `tests/mcp_capture.cpp` — the tools over a host with no recorder, which is what a headless run is:
  the write gate both ways, `capture_status` answering with `available` false and every state field,
  the start schema naming no directory, both write verbs refusing with the reason, and the handler's
  settings validation as whole-call tool errors.

**Only the live look can prove:**

- That the **capture composite actually runs into the sink** — `ViewportCompositor::CompositeToSink`
  needs a presented frame, and there is none headless. The band proves the import and the encode on
  either side of it; the pass itself is unproven until a window has recorded.
- That a **lockstep capture of a slow scene plays back smooth**, and that its sound track stays locked
  to the picture through a whole recording rather than a hundred and twenty synthetic pumps.
- That `IncludeOverlay` does what it says, and that the recording's brightness and colour match what
  the window showed.
- That the **panel drives a real capture** — its Start and Stop, its live figures moving, and the
  settings it edits reaching the file — since the panel needs both a window and a recorder that can
  record.

A capture recorded from a window is therefore **load-bearing evidence, not decoration**.
