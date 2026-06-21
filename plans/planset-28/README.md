# planset-28 — mip-pyramid bloom (compute dual-filter)

**Phase goal:** replace the fixed single-level separable-Gaussian bloom with a
**mip-pyramid bloom** — a compute-driven progressive **downsample** + accumulating
**upsample** over an HDR mip chain, modeled on the delivered hi-Z compute mip
reduction. The result is the wide, soft, energy-conserving multi-octave glow the
single-level blur cannot produce, for **less bandwidth** than the current full-resolution
four-stage chain (most work happens at ≤¼ resolution — a structural expectation, not a
figure this planset's tests measure). The downsample/upsample filter is a **selectable kernel** — the
Call of Duty / Jimenez 13-tap-down / tent-up dual filter as the default, and the
bandwidth-optimized **Dual Kawase** filter (Bjørge, designed for tile-based GPUs — the
veng primary platform) as the configurable alternative — with **Karis-average firefly
suppression** on the first downsample so the bloom is temporally stable without TAA.
Takes up [future area 8](../future/README.md#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries)'s
post-stack quality work. Design overview: [future/scene-renderer.md](../future/scene-renderer.md).

Bloom today (planset-19) is a **single-level separable Gaussian** authored as a
PostProcess material chain: four full-resolution `PostProcessScenePass` stages —
bright-pass (soft-knee threshold) → 9-tap blur H → 9-tap blur V → composite — over four
renderer-owned full-`m_Extent` HDR intermediates ([SceneRenderer.cpp:1719](../../engine/src/Renderer/SceneRenderer.cpp:1719)).
A single 9-tap radius at native resolution is the most expensive way to buy the *least*
spread: it produces a tight, hard-edged glow with no wide falloff. A mip pyramid inverts
that — almost all work happens at ≤¼ resolution, and each octave contributes a different
glow radius, so the accumulated sum is wide and soft for *less* bandwidth than one
full-res pass.

The engine already proves the exact mechanism this needs. The **hi-Z depth pyramid**
([`CreateHiZ`, SceneRenderer.cpp:1501](../../engine/src/Renderer/SceneRenderer.cpp:1501))
is a compute mip-chain in this shape: one `Image` with `MipLevels` levels
(`Storage | Sampled`), a `vector` of single-mip storage `ImageView`s the reduction writes
per level, a whole-chain sampled view, per-level descriptor sets binding source → dest,
and a compute pipeline dispatched once per level with per-level barriers
([hi_z_reduce.comp.slang](../../engine/assets/core/shaders/hi_z_reduce.comp.slang)). A
bloom pyramid is that template with bloom filters and an additive up-sweep — so this
planset reuses validated, MoltenVK-checked infrastructure rather than inventing it.

## Scope decisions

1. **Bloom becomes a fixed compute `ScenePass`; it is no longer a PostProcess material.**
   This **supersedes planset-19 decision 6** ("bloom is a PostProcess material") and
   retires the planset-18 framing of bloom as "the first multi-stage authorable post
   chain" (**tonemap** remains the authorable-material exemplar). The rationale is the
   plumbing-vs-effect line planset-18/19 already drew: a mip pyramid is a **fixed
   dataflow** — its filters are not something an author tunes per-material, and the
   down/up sweep with additive accumulation *is* a compute reduction exactly like hi-Z
   and SSAO. Bloom rejoins SSAO and the shadow atlas as a hardcoded **battery** (a fixed
   engine pass, not an authorable material). The tunables keep the renderer's
   per-frame/topology split: `Threshold` / `Intensity` / `Radius` are per-frame `SceneView`
   values (where `Threshold` / `Intensity` already live, with `Radius` added), `Bloom` on/off
   + the kernel are `SceneRendererSettings` topology knobs. The four core bloom **materials**
   + their fragment shaders are **deleted**. `Exposure` — the one remaining tonemap knob that
   belongs per-frame — is relocated from `SceneRendererSettings` to `SceneView` by **Plan 00**,
   independently of the bloom rework.

2. **The pyramid is one image with N mip levels, mirroring hi-Z — not N separate
   targets.** A single `HdrFormat` (the linear HDR float the bloom operates in) image,
   `Usage = Storage | Sampled`, `MipLevels = max(1u, std::bit_width(max(w,h)) - 3)` so the
   coarsest level holds a ~8 px edge, not a degenerate 1×1. Per-mip single-level **storage**
   views for the compute writes; one whole-chain **sampled** view (mirroring hi-Z's
   `m_HiZSampleView`) plus a **clamp-to-edge linear sampler** for the bilinear reads — the one
   resource bloom adds over hi-Z, which point-`Load`s with no sampler (`HdrFormat` must
   advertise `SampledImageFilterLinear`; assert it at `CreateBloom`).
   This replaces today's **four full-resolution** intermediates (~4× `m_Extent`) with one
   geometric pyramid (~1.33× mip0) plus the reused full-res result (~2.33× `m_Extent` total),
   a net memory **reduction**.

3. **Per-level compute dispatches (the hi-Z pattern), not single-pass downsample.** The
   down-sweep and up-sweep each dispatch once per mip level with a barrier between levels,
   exactly as `hi_z_reduce` does — the proven-on-MoltenVK baseline. **AMD FidelityFX SPD**
   (the whole downsample in one dispatch via threadgroup memory + a device-scope atomic)
   is the named follow-on optimization, gated on validating `globallycoherent` storage +
   device-scope atomics under MoltenVK; it is a bandwidth win, not a look change, and is
   out of scope here.

4. **Bright-pass + Karis average are fused into the first downsample.** Mip 0 is produced
   by sampling the lit HDR target, applying the soft-knee `Threshold` (the existing
   bright-pass shape), and weighting the downsample taps by `1 / (1 + luma)` — a partial
   Karis tonemap — so a single firefly pixel cannot drive a flickering bloom. The Karis
   weighting is applied **only** on the HDR → mip 0 step; deeper levels use the plain
   filter. veng has no TAA to hide firefly flicker (history-buffer ringing is a future
   area-8 increment), so this suppression *is* the stability mechanism and is mandatory,
   not optional.

5. **The upsample is an in-place accumulating sweep, composited in linear HDR before
   tonemap.** From the coarsest level up: `mip[i] += upsample(mip[i+1]) * Radius` — each
   step samples the coarser level (bilinear, via the sampled view), reads-modifies-writes
   its own level (the same per-thread texel, no cross-thread hazard), with a barrier
   before the next finer level reads it. A final composite adds `mip[0] * Intensity` to
   the lit HDR into the tonemap-input target. Compute throughout (the additive blend the
   graphics path gets free from the ROP is one extra in-shader sample), so the chain stays
   off the render-pass path that a multi-resolution pyramid cannot keep on-tile.

6. **Two selectable kernels, chosen at `Configure`.** `SceneRendererSettings` gains a
   `BloomKernel { Cod, Kawase }` enum and a `Kernel` field (default `Cod`). (The enumerator is
   `Cod`; running prose writes the acronym "COD".) `Cod` is the 13-tap downsample / 3×3
   tent upsample dual filter — the well-documented reference, and the kernel the golden is
   blessed against. `Kawase` is the Dual Kawase filter (a cheaper 5-tap-down / 8-tap-up
   bilinear pattern designed for bandwidth-bound TBDR GPUs). The kernel choice changes the
   per-level shader, so it is a **topology knob** (`Configure` recompile), like the `Bloom`
   toggle — `Threshold` / `Intensity` / `Radius` stay per-frame, no recompile.

7. **FFT/convolution bloom is explicitly out — a separate future feature.** True lens
   character (anamorphic streaks, aperture ghosts) needs a GPU FFT convolution of the HDR
   frame against a kernel texture — a much larger, compute-heavy feature of its own (cf.
   Unreal's Convolution bloom), aimed at cinematic lens flares, not better everyday bloom.
   It is recorded as a named future area-8 increment, not folded in here.

8. **No editor-side plan; the editor surface is deferred to its reflection.** This planset
   wires only the *example's* hand-written debug UI (Plan 04). If the editor's settings
   inspector reflects `SceneRendererSettings`, the `Kernel` knob and the relocated `Exposure`
   appear there with no editor change; Plan 00 already audits the inspector for the `Exposure`
   move. Any custom editor widget for the new knobs is a separate editor increment, out of
   scope here.

## The bloom chain, before and after

| | Before (planset-19) | After (this planset) |
|---|---|---|
| Mechanism | 4 PostProcess **materials** (`PostProcessScenePass` ×4) | 1 fixed compute **`ScenePass`** (down/up/composite) |
| Filter | single-level 9-tap separable Gaussian, full-res | mip-pyramid dual filter (COD / Kawase), ≤¼-res per octave |
| Spread | one tight radius | wide, multi-octave, energy-conserving |
| Targets | 4 × full-`m_Extent` HDR images (~4× `m_Extent`) | 1 HDR mip pyramid + reused result (~2.33× `m_Extent`) |
| Firefly stability | none | Karis average on mip 0 |
| Authorable | yes (exposed material params) | no (renderer knobs — plumbing, like SSAO) |
| Knobs | `Threshold`, `Intensity` (material params) | `Threshold`/`Intensity`/`Radius` (per-frame `SceneView`), `Bloom`/`BloomKernel` (topology `Settings`) |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | [Move Exposure to SceneView](00-exposure-to-sceneview.md) | Relocate `Exposure` from `SceneRendererSettings` (topology) to `SceneView` (per-frame), so the tonemap reads it each `Execute` without a `Configure`; rewrite the exposure `gpu` test to the per-frame, no-recompile form. Independent of bloom; prerequisite for Plan 04's live exposure slider. | done |
| 01 | [Compute mip-pyramid bloom (COD dual filter)](01-compute-mip-pyramid-bloom.md) | Replace the four-stage material chain with one compute mip-pyramid `ScenePass`: an `HdrFormat` mip-chain image + per-mip storage views + one whole-chain sampled view + a linear sampler (mirroring `CreateHiZ`, plus the sampler hi-Z lacks); a down-sweep (bright-pass + Karis → mip 0, then 13-tap downsample per level) and an in-place tent up-sweep; a composite adding `mip0 * Intensity` to HDR ahead of tonemap. Delete the four bloom materials + their three fragment shaders. `Threshold`/`Intensity` stay per-frame on `SceneView`; add `Radius`. Regenerate the golden and strengthen the bloom property pins. | done |
| 02 | [Selectable Dual Kawase kernel](02-dual-kawase-kernel.md) | `SceneRendererSettings` gains a `BloomKernel { Cod, Kawase }` enum + `Kernel` field (default `Cod`); the down/up compute shaders gain the Kawase tap variant, selected at `Configure`. The `Cod` golden is unchanged; a `gpu` test asserts `Kawase` blooms a bright region too. Depends on 01. | done |
| 03 | [DebugView arm + docs/roadmap re-cut](03-debugview-docs.md) | A `DebugView::Bloom` arm blitting the accumulated bloom pyramid. Re-cut the live docs: `CLAUDE.md`'s renderer/material paragraphs (bloom is a compute battery, not a PostProcess material — supersede planset-19 decision 6 and the "authorable chain" framing), `scene-renderer.md`, the area-8 roadmap status (mip bloom delivered; SPD + FFT convolution named future), the `plans/README.md` entry, and this status table. No render code beyond the debug arm. | ready |
| 04 | [Example debug UI: exposure, bloom, settings audit](04-example-debug-controls.md) | Complete the hello-triangle "Scene" window's `SceneRendererSettings` coverage — add the unexposed controls (**Exposure**, the **Bloom** toggle + **Threshold**/**Intensity**/**Radius** + the **Kernel** combo), respecting the per-frame (no recompile) vs topology (`ReconfigureScene`) split. Example-only (`main.cpp`), no engine change. Depends on 00 (Exposure on `SceneView`) and 01–02 (the new bloom knobs). | ready |

## Dependency analysis

```
00 (Exposure → SceneView) ──────────────────────────────────────────────► 04 (example debug UI)
01 (compute mip-pyramid bloom, COD) ──► 02 (Kawase kernel option) ──┬──► 03 (DebugView + docs)
                                                                    └──► 04 (example debug UI)
```

- **Plan 00** is independent of bloom — it relocates `Exposure` to `SceneView` (a per-frame
  knob and a one-test rewrite) and runs in parallel with Plan 01. Plan 04 needs it for the
  live exposure slider.
- **Plan 01** is the whole working pyramid: resource, down-sweep, up-sweep, composite, and
  the deletion of the old material chain. It is **atomic** — a downsample-only or
  upsample-only intermediate cannot produce a correct image, and the golden check demands a
  correct image — so the full COD pyramid lands in one plan that leaves the renderer
  working and the golden re-blessed.
- **Plan 02** adds the second kernel behind a setting; it depends on 01's shaders and pass
  wiring and changes nothing for the default `Cod` path (the golden does not move).
- **Plans 03 and 04** are leaves after 02, mutually independent: **03** is the `DebugView`
  arm + the live-doc re-cut (the arm needs the pass; the docs describe the delivered end
  state); **04** is the example's debug-UI controls (needs Plan 00's `Exposure` move and the
  `BloomRadius`/`Kernel` knobs to exist). Neither moves the golden.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify
(clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a
device is present, the smoke PPM correct size + exit 0, the `smoke_golden` capture
re-checked) → update this table → one commit per plan (`Plan NN: <summary>`,
`Co-Authored-By` trailer).

Common to all plans:

- **`smoke_golden` moves once, in Plan 01, and is regenerated per the `CLAUDE.md`
  procedure.** The mip pyramid changes the bloom's shape on the sample scene's bright
  highlight (wider, softer falloff). Plan 01 regenerates `tests/golden/hello_triangle_scene.png`
  in the same pass and states the expected visual change; Plan 02's default `Cod` path and
  Plan 03's debug arm do **not** move it. Plan 01 carries a **`gpu` property assertion** (a
  bright region blooms with `Bloom` on vs off, and the glow reaches *wider* than a single
  9-tap radius would) so the feature is pinned by a property, not only a re-blessed image —
  reusing the existing bloom `gpu` test's fixture.
- **The compute mip-chain mirrors `CreateHiZ` — with two deliberate divergences.** The
  pyramid image, per-mip storage views, whole-chain sampled view, per-level descriptor sets,
  and per-level dispatch-with-barrier loop follow the hi-Z reduction's established shape; the
  bloom pipelines, sets, and views retire through the same deferred `Release()` path on
  `Resize`/`Configure`. Bloom diverges where a depth reduction does not: it samples
  **bilinearly** (a linear sampler hi-Z's point `Load` lacks), and its up-sweep
  **accumulates in place** (a same-image sample+RMW barrier shape hi-Z's pure reduction never
  exercises). Both are flagged in Plan 01 as items to verify rather than assume.
- **New `AssetId`s use a marked placeholder during implementation**, minted with `vengc
  generate-id` in the final pass (the new core compute shaders: bloom downsample, bloom
  upsample, bloom composite, and Plan 02's Kawase variants). Hardcoded C++ literals are
  uppercase hex; JSON packs are decimal. The deleted bloom material/shader ids are removed
  from `core.vengpack.json` and the core asset sources.
- **The validation gate stays clean.** The per-level compute dispatches carry
  graph-derived (or explicitly recorded, as hi-Z does) image barriers between levels and
  between the down/up/composite phases; recreates go through the deferred `Release()`. The
  `ctest -L validation` gate must stay green.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy applies;
  no "used to be a separable Gaussian" or "the old material chain" narrative in the code.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

Bloom is a **compute mip-pyramid** battery: the lit HDR target is bright-passed with
Karis-average firefly suppression into mip 0, progressively downsampled through an HDR mip
chain, then upsampled with an accumulating dual filter and composited back into linear HDR
ahead of tonemap. The filter kernel is selectable (`BloomKernel::Cod` default,
`BloomKernel::Kawase` the TBDR-optimized alternative), with `Threshold` / `Intensity` /
`Radius` per-frame `SceneView` values and `Bloom` / `BloomKernel` the topology knobs. Bloom
is a fixed engine pass like SSAO and the shadow atlas — no longer a PostProcess material — and
a `DebugView::Bloom` arm visualizes the pyramid. The hello-triangle "Scene" debug window
exposes the renderer's full tunable surface, including the new **Exposure** (moved to
`SceneView` for live tuning) and **bloom** controls. The single-pass downsample (SPD)
bandwidth optimization and a separate **FFT/convolution lens-bloom** feature stay named future
area-8 increments behind the delivered pyramid.
