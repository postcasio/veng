# planset-32 — the render allocation sizes itself

**Phase goal:** make the render-target **allocation** size *self-determining*, and stop a HiDPI
display from supersampling its backing extent. Today dynamic resolution has a fast inner loop —
`ComputeDynamicResolutionScale` eases a viewport's per-frame `RenderScale` toward a GPU-frame-time
budget, rendering into a sub-rect of high-water-mark-allocated targets ([`DynamicResolution.h`](../../engine/include/Veng/Renderer/DynamicResolution.h)) — but the **allocation** those targets
are sized at is a **hand-picked `MaxScale`** (`Viewport::AllocationScale()` returns the static
`DynamicResolutionSettings::MaxScale`). That is the knob this planset removes. And on a HiDPI display
the managed viewport sizes that allocation to the **swapchain framebuffer extent** — 2× the logical
window — so every render-graph image (a deferred g-buffer's 4 MRTs + depth + HDR + lit + TAA history
+ bloom pyramid + SSAO + SSR + hi-Z + output + gather + composite) is allocated at twice the pixels
the small window actually needs. Scaling the sub-rect down barely helps there, because the cost that
hurts on a unified-memory MoltenVK device — the allocation footprint and the full-allocation tail
(the terminal tonemap/upscale pass, the gather + swapchain composite, the TAA history-copy) — does
not scale with the sub-rect.

This planset adds the **slow outer loop**. The per-frame sub-rect stays the free, continuous knob;
the allocation becomes a **lazy, quantized, hysteretic, dwell-gated follower** of the sustained
sub-rect scale, so the allocation tracks what the scene actually sustains without the manual `MaxScale`
and **without thrashing** — the expensive `Resize` never reacts to a fast signal. Separately, the
HiDPI baseline becomes a **budget decision** (an allocation cap relative to the backing extent) rather
than the backing extent itself, so the steady state is right and the outer loop is only a safety net
for genuine sustained overload.

**This planset carries a second, independent track — texture compression.** Alongside the allocation
work it shrinks packed-asset size and texture memory through three composable axes: **zstd-compressed
archive blobs** (every asset type, on-disk size), an **offline mip chain**, and **BC7/ASTC GPU block
compression** (defaulting to **BC7**, persisting ~8:1 through to VRAM and sampling bandwidth). It shares
**no files** with the allocation track and is described under [Track B](#track-b--texture-compression)
below; the two tracks land in parallel.

## The shape

```
  GPU frame time ─►[inner loop]─► RenderScale ∈ [MinScale, allocScale]   per frame · FREE (sub-rect)
                  ComputeDynamicResolutionScale         │                  (built — planset unchanged)
                                                long EMA │ (seconds)
                                                         ▼
                  [outer loop]─► allocation tier ∈ {1.0, 0.75, 0.5}       seconds · RARE (Resize)
                  StepAllocationTier  ·  quantized · hysteresis band · asymmetric dwell
                                                         │ on tier change (debounced, safe moment)
                                                         ▼
                                  SceneRenderer::Resize(round(region · tier))

  HiDPI:  swapchain backing extent (≈2× points) ──cap──► allocation baseline   steady-state fix
                                            MaxAllocationScale / point-size budget
```

- **Two loops, two timescales, one physical quantity.** The inner loop (built) sets the per-frame
  sub-rect `RenderScale` from GPU frame time — continuous, free, reacts to every spike. The outer loop
  (new) sets the **allocation tier** from a multi-second EMA of that sub-rect scale — quantized, rare,
  reacts only to what the scene *sustains*. The allocation is always ≥ the sub-rect, so the sub-rect
  rides *inside* the chosen allocation and normal DRS fluctuation never pokes the ceiling.
- **The expensive knob never reacts to a fast signal.** This is the entire anti-thrash guarantee. The
  allocation moves only across **wide hysteresis gaps** (step down below one threshold, back up only
  above a higher one) after a **sustained dwell** (asymmetric: quicker to shrink under sustained load,
  slow to grow back), and only at coarse **quantized tiers** (few tiers ⇒ few possible transitions).
  A scene parked near a boundary cannot oscillate the allocation.
- **Reallocating is visually ~free.** A tier change is the same scale at two timescales: shrinking the
  allocation 1.0→0.5 turns a sub-rect riding at "0.5-of-1.0" into "1.0-of-0.5" — the rendered pixel
  count is unchanged across the `Resize`, so quality does not pop. The outer loop only stops paying
  footprint/bandwidth for an allocation it was not using. That continuity is what makes a lazy follower
  safe.
- **The HiDPI baseline is a budget, not the backing extent.** The managed viewport caps the allocation
  relative to the swapchain framebuffer extent (a `MaxAllocationScale` / logical-point budget), so a 2×
  HiDPI display is not silently supersampled. With the baseline right, the outer loop rarely fires — it
  is the safety net for sustained overload, not the steady state.

## The spine — four principles

1. **Allocation follows the sustained sub-rect, lazily.** The fast knob does all the fast work; the
   allocation is a slow, quantized follower of the inner loop's settled operating point (a long EMA of
   `RenderScale`). It never measures perf directly — it reads the inner loop's output, which already is
   the perf response.

2. **The expensive knob is thrash-proof by construction.** Quantized tiers + a hysteresis band per
   boundary + asymmetric dwell timers + reallocation only on a tier *change* mean the allocation moves
   at most once every few seconds and only after a durable regime shift. Thrashing is impossible because
   nothing fast enough to thrash is wired to it.

3. **A tier change is visually continuous.** The allocation and the sub-rect are one quantity; a `Resize`
   to match the sustained sub-rect keeps the rendered pixel count constant, so the follower never causes
   a visible step. Reallocation is pure reclamation, not a quality decision.

4. **The HiDPI cap is steady-state; the controller is the safety net.** Baseline allocation is a budget
   decision decoupled from the backing extent. The outer loop catches genuine sustained overload from a
   sane baseline, rather than papering over a 2× supersample no one asked for.

## What already exists (built; unchanged here)

- `DynamicResolutionSettings` + `ComputeDynamicResolutionScale` — the pure inner-loop frame-time
  controller (deadband, rate limit, scale² cost model).
- `Viewport::SetDynamicResolution` / `UpdateDynamicResolution` / `m_RenderScale` /
  `AllocationScale()` / `ViewRenderScale()` — the per-frame sub-rect drive; `SceneRenderer` renders the
  `round(allocExtent · RenderScale)` sub-rect and the terminal tonemap upscales it.
- `ManagedViewportInfo::DynamicResolution` — the managed-viewport opt-in.

The static `AllocationScale() == MaxScale` is the seam this planset replaces; the inner loop is reused
verbatim as the outer loop's input signal.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | Allocation-tier controller | A pure, device-free `AllocationTierSettings` + `AllocationTierState` + `StepAllocationTier` beside the inner-loop controller in `DynamicResolution.h`: a long EMA of the sub-rect scale → a quantized tier through a hysteresis band + asymmetric dwell timers. Unit-tested for no-oscillation, dwell, and hysteresis. Foundational, purely additive. | proposed |
| 01 | HiDPI allocation cap | `MaxAllocationScale` (cap on the allocation relative to the swapchain framebuffer extent) on `ViewportInfo`/`ManagedViewportInfo`, applied where the managed viewport derives its extent. Decouples the allocation baseline from the 2× backing extent. Independent of 00. | proposed |
| 02 | Viewport tier wiring | Drive the allocation tier from the inner loop: `Viewport` tracks the long EMA of `m_RenderScale`, steps the tier via `StepAllocationTier`, and on a tier change debounces a `SceneRenderer::Resize(round(region · tier))` — replacing the static `AllocationScale() == MaxScale`. Migrate hello-triangle (auto allocation; expose the tier in Stats). Depends on 00 (+ 01 for the baseline). | proposed |
| 03 | Docs + roadmap | `engine/CLAUDE.md` (the two-loop allocation model on the `SceneRenderer`/`Viewport` sections), `future/README.md` area 8, this record. Full `ctest` + `smoke_golden` + `validation_gate` green. | proposed |
| 04 | Archive zstd compression | **Track B.** Per-blob zstd in the `.vengpack` (format **v3**: the `flags` field carries the codec, the TOC entry gains `UncompressedSize`); the cooker compresses + picks stored-vs-zstd, `assetpack` inflates lazily on resolve. Shrinks every asset type on disk. Independent of the rest of Track B. | proposed |
| 05 | Texture mip chain | **Track B.** Offline mip generation in the texture cooker (sRGB-/linear-correct), the multi-mip blob layout, and a multi-region upload — replacing the single-mip-only restriction. Still uncompressed RGBA8; lays the infrastructure Plan 06 needs. | proposed |
| 06 | BC7 block compression | **Track B.** The default codec: `BC7Unorm`/`BC7Srgb` formats + `TypeMapping` + a `FormatInfo` block helper + a `textureCompressionBC` gate; a cooker-only BC7 encoder; block-aware upload. Cooks every texture to BC7 by default. Depends on 05. | proposed |
| 07 | ASTC block compression | **Track B.** `ASTC4x4` formats + a cooker-only `astc-encoder` + a `textureCompressionASTC_LDR` gate over the same machinery — proving both codecs. BC7 stays default; ASTC selectable internally. Depends on 06. | proposed |
| 08 | Texture-compression migration | **Track B.** Migrate hello-triangle (mipped BC7 over a zstd pack), regenerate the smoke golden on a BC-capable device, document the track across the `CLAUDE.md` set + `future/README.md` (the deferred developer-control work). Depends on 04–07. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

**00** (the pure controller) and **01** (the HiDPI cap) are independent and can land in parallel.
**02** depends on 00 (it calls `StepAllocationTier`) and reads better atop 01 (a sane baseline means
the tier rarely fires), but does not strictly require 01. **03** is last. Worktree-isolated parallel
dispatch should branch 02 from the 00(+01) integration commit — see [[project_megaexec_worktree_base]].

**Shared-file caveat:** 00 and 02 both touch `DynamicResolution.h` (00 adds the tier API; 02 only
consumes it) and 01 and 02 both touch `Viewport.h`/`Viewport.cpp` and `Application.h`/`Application.cpp`
(01 adds the cap to the extent derivation, 02 adds the tier state + the debounced `Resize`). Merge in
number order (01 then 02) rather than expecting a conflict-free parallel merge on those files.

**Track B (texture compression) is independent of Track A** — they share no files. Within Track B:
**04** (archive zstd) is standalone and parallel to everything. **05 → 06 → 07** is a strict chain:
05 lays the multi-mip blob + multi-region upload, 06 adds the BC7 formats / encoder / capability gate /
block-aware sizing over it, 07 slots ASTC into 06's machinery. **08** (migration + golden + docs)
depends on 04–07 and is last. Worktree-isolated dispatch branches 06 from the 05 integration commit and
07 from the 06 one — see [[project_megaexec_worktree_base]].

**Track B shared-file caveat:** 05, 06, and 07 all touch `cooker/src/Importers/TextureImporter.cpp`,
`engine/src/Asset/Loaders/TextureLoader.cpp`, and (06/07) `Renderer/Types.h` + `TypeMapping.h` +
`FormatInfo.h`. Because they form a sequential chain this is merge-in-order by construction, not a
parallel-merge hazard.

## Track B — texture compression

The current texture path cooks **raw, single-mip RGBA8** ([`TextureImporter.cpp`](../../cooker/src/Importers/TextureImporter.cpp)),
the `Renderer::Format` enum has **no compressed formats**, and the `.vengpack` stores blobs
**uncompressed** — so a 2048² albedo sits in the pack and in VRAM as a flat 16 MB. This track attacks
that on three composable axes:

1. **On-disk size (every asset) — zstd archive blobs (Plan 04).** A container-level change in
   `assetpack` + the cooker, orthogonal to texture formats: each cooked blob is zstd-compressed (the
   cooker picks stored-vs-compressed per blob) and the reader inflates lazily on resolve. Helps meshes
   and prefabs too, but only shrinks the *download* — pixels still inflate to full RGBA8 in VRAM.

2. **A prerequisite — the offline mip chain (Plan 05).** Mips are generated at cook (sRGB-correct) and
   stored in the blob, uploaded as one copy region per level. This is required for block compression
   both in value (a single-mip compressed texture aliases) and in mechanism: a compressed image
   **cannot** be GPU-blit-mipgen'd (`vkCmdBlitImage` rejects compressed formats), so its mips must be
   precomputed. Landed first on uncompressed RGBA8 to prove the blob + upload in isolation.

3. **VRAM + bandwidth — BC7/ASTC block compression (Plans 06–07).** The real win: a
   hardware-compressed format the GPU samples *directly*, ~8:1 that persists to sampling. **BC7 is the
   default** (Plan 06); **ASTC** is added over the same machinery (Plan 07) so both codecs work. The
   axes stack — a BC7 blob still benefits from zstd on top.

**The capability reality the gates encode.** Under MoltenVK, `textureCompressionBC` is **Apple-Silicon
only**, while `textureCompressionASTC_LDR` is **broad on Apple GPUs** (the natively-Metal-blessed
family); BC7 is the desktop/Windows standard. A device lacking the cooked codec's feature gets a logged
`AssetError::Unsupported` (the runtime does **not** transcode). This track makes both codecs cookable
and decodable, **defaulting to BC7**; **all developer control of the codec is deferred** to future
**area 15 — build configurations & project settings** ([`future/build-configurations.md`](../future/build-configurations.md)):
a project owns per-platform **build configurations** that hold the codec policy (a role → format
table), a texture declares a compression **role** rather than a raw codec, the cook-time config
dependency is implicit/coarse (one output pack per config), and the **editor gates preview to host
capability** (build any config, preview only what the host GPU can sample — so "ASTC on Windows" is
structurally impossible). Choosing the codec per device, an uncompressed fallback pack, BC5/BC4 channel
specialization, and wider ASTC footprints all live there.

## The decisions this planset settles

- **The allocation is self-determining.** The hand-picked `MaxScale` allocation goes away; the
  allocation tracks the sustained sub-rect through a quantized, hysteretic, dwell-gated outer loop. The
  cost is a small amount of controller state on `Viewport` and a rare, debounced `Resize`.
- **Anti-thrash is structural, not tuned.** The guarantee that the allocation cannot oscillate comes
  from *what drives it* (a multi-second EMA, never instantaneous perf) plus quantization + hysteresis +
  dwell — not from carefully chosen magic numbers. Tuning the tiers/dwell changes responsiveness, never
  correctness.
- **HiDPI is a budget, not a backing extent.** The allocation baseline is decoupled from the swapchain
  framebuffer extent, so the steady state on a HiDPI display is right and the controller is a safety net.
- **Textures cook compressed and mipped by default; the engine supports both BC7 and ASTC.** Raw
  single-mip RGBA8 is gone — the cook default is mipped BC7, with ASTC available over the same path, and
  the archive is zstd-compressed. The runtime does not transcode: a device lacking the cooked codec
  reports `AssetError::Unsupported`.
- **The codec abstraction is one block-info helper.** Upload sizing and the loader's per-level walk go
  through `FormatInfo::BytesForLevel` for every format (uncompressed = a 1×1 block), so adding a codec
  is a format row + a `TypeMapping` row + an encoder arm — not a new upload path.
- **Developer-facing codec control is deferred, by design.** This track hardcodes the BC7 default; the
  whole authoring layer — per-platform **build configurations** holding the codec policy, role-based
  per-asset compression, the coarse cook-time config dependency, and the editor's host-capability
  preview gate — is future **area 15** ([`future/build-configurations.md`](../future/build-configurations.md)).

## What remains (future)

**Safe-moment reallocation** — deferring the tier-change `Resize` to a scene transition / static camera
/ loading screen rather than firing it inline (the dwell already makes the inline hitch rare and
acceptable; this is a polish refinement). **Memory-driven tier capping** — choosing the *initial* tier
from a device memory query (`VkPhysicalDeviceMemoryProperties` budget) so a memory-starved device starts
low, distinct from the perf-driven outer loop. **A history ping-pong** to remove the TAA history-copy
from the full-allocation tail (the one full-res cost the sub-rect cannot reduce), the next lever if TAA
is still too expensive once the allocation is right. These stay named follow-ons behind this planset's
two-loop seam.
