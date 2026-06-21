# Plan 03 — DebugView arm + docs/roadmap re-cut

**Goal:** add a `DebugView::Bloom` arm visualizing the bloom pyramid, then re-cut the
**live** documentation to describe the delivered end state — bloom is a fixed compute
battery, not a PostProcess material. No render code beyond the debug arm. Depends on
Plans 01–02 — the arm itself needs only Plan 01's pyramid; the dependency on Plan 02 is so the
re-cut docs can describe the delivered `BloomKernel` knob.

## What lands

### `DebugView::Bloom`

A `DebugView::Bloom` arm (joining `Albedo`/`Normal`/.../`AO`/`Shadows`/`Cascades`/
`PunctualShadows`) that terminates the chain after the bloom pass with a single fullscreen
debug blit of **mip 0 after the up-sweep** — the full accumulated bloom contribution before it
is composited (decision 2 below), not `m_BloomResultImage`, which is the composited
`hdr + mip0 * Intensity` and so includes the whole scene. The arm force-wires the bloom pass
(as the `AO`/`Shadows` arms force-wire their producing battery), then blits, exactly the
debug-blit shape the other battery arms use. It re-wires through `Configure`, the recompile
seam, like every other `DebugView` mode.

### The live-doc re-cut

The **historical planset READMEs stay as written** — planset-18/19's entries record what
those plansets shipped (bloom as a PostProcess material) and are not rewritten. The **live**
docs are re-cut to the current reality:

- **`CLAUDE.md` (root + `engine/CLAUDE.md`)** — the renderer and material paragraphs that
  describe bloom as "the first multi-stage authorable post chain" / "bloom authored as a
  PostProcess material" are re-cut: bloom is a **compute mip-pyramid `ScenePass`** (Karis
  bright-pass → progressive downsample → accumulating upsample → composite), a fixed battery
  like SSAO and the shadow atlas, with `Threshold`/`Intensity`/`Radius` per-frame `SceneView`
  values and `Bloom`/`BloomKernel` `SceneRendererSettings` topology knobs. The "PostProcess
  fullscreen-material path" paragraph keeps
  **tonemap** as its example and drops bloom as the multi-stage exemplar. State the
  supersession plainly (a present-tense fact: bloom is a compute pass), without "used to
  be a material" narrative.
- **`plans/future/scene-renderer.md`** — the area-8 design overview: the bloom battery is a
  compute mip pyramid; **SPD** (single-pass downsample) and **FFT/convolution lens bloom**
  are named future increments behind it.
- **`plans/future/README.md`** — the area-8 status line: mip-pyramid bloom delivered;
  SPD + FFT convolution bloom added to the named still-future increments. The planset-18/19
  "bloom as a PostProcess material" phrasings in the *delivered* recaps are left (they
  record what those plansets did), but the live "still future" list gains the two bloom
  follow-ons.
- **`plans/README.md`** — the planset-28 entry (a one-paragraph recap, in the index voice).
- **This planset's `README.md`** — the status table to `done`.

## Decisions

1. **Historical READMEs are immutable records; only live docs are re-cut.** A planset's
   README documents what *it* delivered — rewriting planset-19 to say "bloom is a compute
   pass" would falsify its record. The `CLAUDE.md`/`scene-renderer.md`/area-8-status surface
   describes the engine *as it is now*, so those move.

2. **The debug arm visualizes the accumulated bloom, not a single mip.** Showing mip 0 post
   up-sweep (the full multi-octave contribution) is the useful view for tuning
   `Threshold`/`Radius`; a per-mip inspector is unnecessary.

3. **SPD and FFT convolution are recorded as the two named bloom follow-ons.** The plan
   leaves the roadmap pointing at the next two genuine increments — the SPD bandwidth
   optimization (same look, one dispatch) and a separate cinematic FFT/convolution lens
   bloom — so the direction is explicit, not folded into this planset.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `DebugView::Bloom` enum arm. |
| `engine/src/Renderer/SceneRenderer.cpp` | The `Bloom` debug arm: force-wire the bloom pass + a fullscreen blit; `Configure` re-wire. |
| `CLAUDE.md`, `engine/CLAUDE.md` | Re-cut the bloom renderer/material paragraphs to the compute pyramid; tonemap stays the PostProcess-material exemplar. |
| `plans/future/scene-renderer.md` | Bloom battery = compute mip pyramid; SPD + FFT convolution named future. |
| `plans/future/README.md` | Area-8 status: mip bloom delivered; SPD + FFT convolution named still-future. |
| `plans/README.md` | The planset-28 index entry. |
| `plans/planset-28/README.md` | Status table → `done`. |

## Verification

- Clean build; `DebugView::Bloom` blits the accumulated bloom and re-wires through
  `Configure` like the other debug arms; `smoke_golden` does **not** move (the default
  `Final`/`Cod` path is unchanged).
- The docs read as present-tense fact (no "used to be a material" narrative); a reader who
  never saw planset-19 understands bloom as a compute battery from `CLAUDE.md` alone.
- `ctest` green across the bands; the validation gate clean (the debug blit adds no new
  barrier surface beyond the existing debug arms).
