# Plan 01 — compute mip-pyramid bloom (COD dual filter)

**Goal:** replace the single-level separable-Gaussian bloom (four chained
`PostProcessScenePass` material stages over four full-resolution intermediates) with a
**compute mip-pyramid bloom** modeled on the hi-Z reduction: a bright-pass + Karis
downsample into mip 0, a progressive 13-tap downsample down the pyramid, an in-place tent
upsample accumulating back up, and a composite adding the result into the lit HDR ahead of
tonemap. This is the whole working pyramid in one atomic plan — a downsample-only or
upsample-only intermediate cannot produce a correct image, and the golden check demands
one. Depends only on the existing lit-HDR target and the shipped compute infrastructure.

## What lands

### The bloom pyramid resource — mirroring `CreateHiZ`

A `CreateBloom()` rework (replacing the four-image version at
[SceneRenderer.cpp:1719](../../engine/src/Renderer/SceneRenderer.cpp:1719)) builds **one**
mip-chain image and its views exactly as `CreateHiZ`
([SceneRenderer.cpp:1501](../../engine/src/Renderer/SceneRenderer.cpp:1501)) builds the
hi-Z pyramid:

- `m_BloomImage` — `Image::Create` with `Format = HdrFormat` (the linear HDR float the
  bloom operates in), `Usage = ImageUsage::Storage | ImageUsage::Sampled`, and
  `MipLevels = mipCount`. `mipCount = max(1u, std::bit_width(max(m_Extent.x, m_Extent.y)) -
  kBloomTileShift)` with `kBloomTileShift = 3` — stopping the chain ~3 levels short of 1×1 so
  the coarsest level holds a ~8 px edge, not a degenerate 1×1 contributing nothing. The
  `max(1u, …)` floor guards a tiny extent (mirroring hi-Z's `maxDim == 0` guard).
- `m_BloomMips` — a `vector<Ref<ImageView>>` of single-mip **storage** views
  (`BaseMipLevel = level, MipLevels = 1`), one per level, the down/up dispatches write.
- `m_BloomSampleView` — a **single whole-chain sampled** view, mirroring hi-Z's
  `m_HiZSampleView`; the down/up dispatches read a specific level by explicit LOD. Storage
  and sampled access to one mip need distinct views — the per-mip storage views above for the
  writes, this one sampled view for the reads.
- A **clamp-to-edge linear sampler** the down/up dispatches read through, so the wide taps
  clamp at level edges rather than wrap. This is the first place bloom diverges from hi-Z's
  resource shape: hi-Z reduces with a point `Load()` and **no sampler**
  ([hi_z_reduce.comp.slang:34](../../engine/assets/core/shaders/hi_z_reduce.comp.slang:34)
  reads a `Texture2D` via `.Load`), whereas every COD/Kawase tap is a **bilinear** half-texel
  fetch — so the bloom compute descriptor layout carries a sampler binding and the shaders
  `Sample()` (not `Load()`). Assert once at `CreateBloom` that `HdrFormat` (`RGBA16Sfloat`)
  advertises `SampledImageFilterLinear` — a capability hi-Z never exercises, so the
  "proven on MoltenVK" lineage does not cover it.
- `m_BloomResultImage` / `m_BloomResultView` / `m_BloomResultHandle` **already exist and are
  reused** ([SceneRenderer.h:703](../../engine/include/Veng/Renderer/SceneRenderer.h:703)):
  the composite stores into the same result target the tonemap samples today, so the
  tonemap-source wiring (`tonemapSourceId`/`tonemapSourceHandle`,
  [SceneRenderer.cpp:1882](../../engine/src/Renderer/SceneRenderer.cpp:1882)) is unchanged —
  only the *producer* of that target changes, from the four-stage material chain to the
  compute composite.

The four old intermediates (`m_BloomBright*`/`m_BloomBlurH*`/`m_BloomBlurV*` images, views,
and bindless handles) and their `Import`ed `ResourceId`s are **deleted**. The pyramid +
result retire through the same deferred `Release()` path on `Resize`/`Configure`.

### The compute pipelines + per-level descriptor sets

Three compute pipelines, built like `m_HiZReducePipeline`
([SceneRenderer.cpp:1146](../../engine/src/Renderer/SceneRenderer.cpp:1146)) — a
`ComputePipeline` per shader, each with a `PipelineLayout` and a small push range. The
**down/up** pipelines share one `DescriptorSetLayout` (a sampled source binding + the
clamp-to-edge sampler + a storage destination binding). The **composite** pipeline needs a
**distinct layout**: it samples **two** inputs — the lit HDR and bloom mip 0 — and stores one
result, so its set binds two sampled images + the sampler + the storage destination
(`bloom_composite.frag.slang` reads both `Hdr` and `Bloom` today; the compute port keeps
that, whether via two explicit sampled bindings or the existing bindless
`g_Textures[]`/`g_Samplers[]` path):

- **`bloom_down.comp.slang`** — samples the source (the lit HDR for mip 0, mip `i-1`
  otherwise) and writes the downsampled level. The HDR → mip 0 dispatch fuses the
  **bright-pass** (the existing soft-knee `Threshold` shape, ported from
  [bloom_brightpass.frag.slang](../../engine/assets/core/shaders/bloom_brightpass.frag.slang))
  and the **Karis average** (`1/(1+luma)` tap weighting); a push flag selects the
  bright-pass+Karis path for level 0 vs. the plain 13-tap path for deeper levels.
- **`bloom_up.comp.slang`** — samples the coarser level `i+1` with the 3×3 tent, scales by
  `Radius`, and **adds** it into level `i` (read-modify-write of the dispatch's own texel
  through the storage view).
- **`bloom_composite.comp.slang`** — `result = hdr + mip0 * Intensity` (replacing
  [bloom_composite.frag.slang](../../engine/assets/core/shaders/bloom_composite.frag.slang)
  as a compute store into `m_BloomResultImage`).

Per-level descriptor sets (one per down step, one per up step) bind source → destination,
built in the `CreateBloom` loop exactly as `m_HiZReduceSets` are
([SceneRenderer.cpp:1542](../../engine/src/Renderer/SceneRenderer.cpp:1542)).

### Execute: the down/up/composite sweep

In the bloom `ScenePass` callback (a compute `ScenePass`, contributing compute dispatches
into the renderer's internal graph the way the hi-Z reduce pass does), when `m_BloomActive`:

1. **Down-sweep** — for `level` 0..N-1: bind the level's set, push `{ texel size, level-0
   bright-pass flag, Threshold }`, dispatch over the level's extent, barrier before the
   next level reads it.
2. **Up-sweep** — for `level` N-2..0: read the coarser level `i+1` through the **sampled**
   view (bilinear tent), read-modify-write the dispatch's own texel in level `i` through its
   **storage** view (`mip[i] += tent(mip[i+1]) * Radius`), push `{ texel size, Radius }`,
   dispatch over the level's extent, barrier before the next finer level reads it. The pass
   declares both a `StorageRead` **and** a `StorageWrite` on level `i` (so the graph orders it
   after the down-sweep that wrote that level) plus a sampled read of level `i+1`.
3. **Composite** — push `{ Intensity }`, dispatch over `m_Extent`, store `hdr + mip0 *
   Intensity` into `m_BloomResultImage`; the tonemap samples it unchanged.

Per-frame values (`Threshold`/`Intensity`/`Radius`) ride push constants (small
per-invocation scalars — the deferred-path discipline), not a recompile.

### The knob split — per-frame on `SceneView`, topology on `Settings`

The per-frame bloom tunables already live on **`SceneView`**, written each `Execute`
([SceneRenderer.h:284](../../engine/include/Veng/Renderer/SceneRenderer.h:284)) — they
**stay there**, now feeding the compute push instead of the deleted material param block:

```cpp
struct SceneView {
    // ...
    f32 BloomThreshold = 1.0f;  // bright-pass knee  (unchanged location)
    f32 BloomIntensity = 1.0f;  // composite mix     (unchanged location)
    f32 BloomRadius    = 1.0f;  // upsample spread   (new)
};
```

- `BloomRadius` is a **new** `SceneView` field (the upsample spread Plan 01's tent up-sweep
  reads). `BloomThreshold` / `BloomIntensity` already live on `SceneView`; with bloom now a
  compute pass they feed the compute push instead of the deleted material's param block, so
  their existing doc comments (which reference "the bloom material's param block",
  [SceneRenderer.h:875](../../engine/include/Veng/Renderer/SceneRenderer.h:875)) are re-cut to
  the compute push in this pass.
- **`SceneRendererSettings` keeps only the bloom *topology* knob, `Bloom`** (on/off →
  recompile). Plan 02 adds `BloomKernel` beside it.
- **`Exposure` is *not* touched here** — Plan 00 relocates it from `SceneRendererSettings` to
  `SceneView` independently of bloom; Plan 01 neither moves nor reads it.

### Deletions

- The **four** bloom materials — `bloom_brightpass.vmat.json`,
  `bloom_blur_horizontal.vmat.json`, `bloom_blur_vertical.vmat.json`,
  `bloom_composite.vmat.json` — and their four `material` references in `core.vengpack.json`.
- The **three** bloom **fragment** shaders — `bloom_brightpass`, `bloom_blur` (the one shader
  shared by both blur materials), `bloom_composite` — `.slang` + `.shader.json`, and their
  three `shader` references in `core.vengpack.json` (their logic moves into the compute
  shaders). The four-materials-to-three-shaders asymmetry is expected: `bloom_blur` backs both
  the horizontal and vertical blur materials.
- The `m_BloomBrightMaterial`/`m_BloomBlurH/V`/`m_BloomCompositeMaterial` handles and their
  `LoadMaterial` calls ([SceneRenderer.cpp:1080](../../engine/src/Renderer/SceneRenderer.cpp:1080)),
  the four-stage `PostProcessScenePass` wiring
  ([SceneRenderer.cpp:1885](../../engine/src/Renderer/SceneRenderer.cpp:1885)), and the four
  old intermediate images/views/handles/ids.

## Decisions

1. **One atomic plan.** The pyramid produces a correct image only with all of
   down + up + composite present, and every plan must leave the golden green — so the full
   COD pyramid (and the deletion of the material chain it replaces) lands together.

2. **Compute, per-level dispatch, mirroring hi-Z.** The reduction shape, the per-mip
   storage views, the per-level descriptor sets, and the dispatch-with-barrier loop are the
   `CreateHiZ`/`hi_z_reduce` pattern — proven on MoltenVK. SPD (single-dispatch downsample)
   is a named follow-on, not this plan.

3. **Bright-pass + Karis fused into mip 0.** One fewer full-res pass than a standalone
   bright-pass, and the Karis weighting belongs precisely on the HDR → mip 0 downsample. The
   soft-knee `Threshold` shape is preserved from the existing bright-pass shader.

4. **In-place accumulating up-sweep.** `mip[i] += upsample(mip[i+1]) * Radius` reads and
   writes the dispatch's own texel — no cross-thread hazard — with a per-level barrier; the
   standard COD structure, and it keeps the pyramid to one image. This is the second place
   bloom diverges from hi-Z (a pure max-reduction, never an accumulate): a single dispatch
   both **samples** mip `i+1` (ShaderReadOnly) and **read-modify-writes** mip `i` (General) of
   the *same* image — two subresources in one pass. Before this lands, confirm the RenderGraph
   derives correct per-subresource barriers for a same-image sample(`i+1`) + storage-RMW(`i`)
   pass (a throwaway two-mip check); if it does not, fall back to **explicitly recorded**
   barriers (as hi-Z's reduce loop already records its own), not graph-derived ones. The
   "no new validation surface" expectation holds only once this is verified.

5. **Composite into the existing result target; tonemap wiring unchanged.** The composite
   writes `m_BloomResultImage`, which the tonemap already samples as
   `tonemapSourceHandle`; only the *producer* of that target changes, so the HDR-chain
   wiring downstream of bloom is untouched.

6. **Per-frame bloom knobs stay on `SceneView`, feeding the compute push.** They were
   exposed material params, but `SceneView` already carried `BloomThreshold`/`BloomIntensity`
   to drive that material each frame; with bloom now a compute pass they drive the compute
   push instead — same per-frame/no-recompile behavior, same home. `BloomRadius` joins them.
   (`Exposure`'s relocation to `SceneView` is Plan 00's, not this plan's.)

## Files

| File | Change |
|---|---|
| `engine/assets/core/shaders/bloom_down.comp.{slang,shader.json}` (new) | Downsample compute: 13-tap; level-0 bright-pass + Karis; minted ids. |
| `engine/assets/core/shaders/bloom_up.comp.{slang,shader.json}` (new) | Tent upsample-accumulate compute; minted ids. |
| `engine/assets/core/shaders/bloom_composite.comp.{slang,shader.json}` (new) | `hdr + mip0 * Intensity` compute store; minted ids. |
| `engine/assets/core/materials/bloom_{brightpass,blur_horizontal,blur_vertical,composite}.vmat.json` (delete) | The four old PostProcess bloom materials. |
| `engine/assets/core/shaders/bloom_{brightpass,blur,composite}.frag.{slang,shader.json}` (delete) | The three old fragment stages (`bloom_blur` backs both blur materials). |
| `engine/assets/core/core.vengpack.json` | Drop the deleted bloom material/shader entries; add the three (Plan 02: five) new compute shaders. |
| `engine/src/Renderer/SceneRenderer.cpp` | Rework `CreateBloom` to the mip pyramid + per-level sets; build the three compute pipelines; replace the four-stage material wiring with the compute down/up/composite sweep in the bloom `ScenePass`; delete the old materials/intermediates. |
| `engine/include/Veng/Renderer/SceneRenderer.h` | Add `BloomRadius` to `SceneView` and re-cut the `BloomThreshold`/`BloomIntensity` doc comments to the compute push; the pyramid members (`m_BloomImage`/`m_BloomMips`/`m_BloomSampleView`) replacing the four intermediates; the new pipeline/set/layout members (down/up shared layout + the distinct composite layout). `Exposure` is untouched here (Plan 00). |
| `tests/gpu/scene_renderer.cpp` | Strengthen the bloom property pins (the re-blessed golden cannot catch a regression in the commit that regenerates it): (a) a bright region blooms on vs off; (b) **far-halo lift** — luminance at a radius beyond any 9-tap reach (the deleted blur's ±4-texel half-width — sample ≥ ~12 px from the highlight, outside the old fixture's 8 px core) rises with bloom on; (c) **radius monotonicity** — halo energy grows across ≥3 increasing `Radius` values; (d) **firefly suppression** — a single injected HDR spike does not blow the halo out of proportion (pins the "mandatory" Karis average); (e) `Threshold`/`Intensity`/`Radius` change the result with **no** recompile. |

## Verification

- Clean build; the three new compute shaders cook into the core pack and load through the
  normal `AssetManager` path; the deleted four materials + three fragment shaders are gone
  from the pack and nothing references them.
- `gpu` band + `smoke_golden`: **regenerated** to show the wider, softer mip bloom on the
  sample scene's bright highlight (per the `CLAUDE.md` golden procedure). Because Plan 01 both
  rewrites the bloom and regenerates the golden, the golden has **no regression power for this
  commit** — the property pins (a)–(e) above carry the correctness load, not the re-blessed
  image. When regenerating, check the new soft glow leaves headroom in the fuzzy compare's
  mismatch fraction (a wide glow spreads small per-tap differences over more pixels than the
  old tight highlight); widen the tolerance deliberately, with a stated reason, if it does
  not. `Bloom = false` matches the raw HDR → tonemap result; changing
  `BloomThreshold`/`BloomIntensity`/`BloomRadius` per frame does not recompile.
- `ctest -L validation` clean: the per-level compute dispatches and the down/up/composite
  phase boundaries carry correct image barriers, and `Resize`/`Configure` recreates go through
  the deferred `Release()`. **Note** the up-sweep's same-image sample+RMW barrier shape
  (decision 4) is the one new validation surface — verify it before trusting a green gate,
  falling back to explicitly recorded barriers if the graph does not derive them.
- Memory sanity: the bloom footprint drops from four full-`m_Extent` HDR images (~4×
  `m_Extent`) to one geometric pyramid (~1.33× mip0) **plus** the reused full-res result
  (~2.33× `m_Extent` total) — still a net reduction.
