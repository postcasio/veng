# Plan 04 — lighting-pass cascade selection + hardware `SampleCmp`

**Goal:** the deferred lighting pass selects the right cascade per fragment and samples its
atlas tile with the **hardware comparison sampler** (`SampleCmp`) bound in set 1 (Plan 03),
cross-fading across the cascade boundary. This completes the CSM visibility path on the
hardware shadow-comparison path — no manual PCF.

## What lands

### Cascade selection + `SampleCmp` ([engine/assets/core/shaders/deferred_lighting.frag.slang](../../engine/assets/core/shaders/deferred_lighting.frag.slang))

The shader reads the trimmed `ViewConstants` (set 0 — `InvViewProj`/`CameraPosition`/`View`/`Proj`,
for world-position reconstruction and the view-space depth) and the new `ShadowConstants` (set 1
— `CascadeViewProj[MaxCascades]`, `CascadeSplits`, `ShadowParams`: 1/tileRes (texel size) /
blend band / cascade count / enabled). It declares **set 1**: the shadow atlas (`Texture2D`), the comparison
sampler (`SamplerComparisonState`, immutable in the layout), and the `ShadowConstants` uniform.
These edits live in the **one shared fragment body**; the AO-fold variant
(`deferred_lighting_ssao.frag.slang`, which `#define`s `VE_USE_SSAO` and `#include`s this file)
inherits them unchanged, so both lighting pipelines get cascade selection from a single authored
source. `DirectionalShadowVisibility` becomes cascade-aware and hardware-compared:

1. **Select the cascade.** Compute the fragment's **view-space depth** (from the reconstructed
   world position through `View`) and pick the smallest cascade `k` whose `CascadeSplits[k]`
   exceeds it. `ShadowParams` cascade-count 0 short-circuits to full visibility (the compiled-out
   gate, as the single-map path did).

2. **Project + `SampleCmp`.** Transform `worldPos` by `CascadeViewProj[k]` (which already lands
   in cascade `k`'s atlas tile, Plan 03 decision 2) to get the tile UV + the reference depth,
   then `shadowAtlas.SampleCmp(shadowCmp, uv, refDepth - bias)` — the hardware does the compare
   **and** the 2×2 bilinear PCF in one fetch. A small N×N grid of `SampleCmp` taps (offset by the
   texel size) widens the kernel where a softer penumbra is wanted; each tap is hardware-filtered.
   **Clamp every sampled UV (including kernel taps) to cascade `k`'s tile sub-rect** (inset by a
   half-texel), so a tap near a tile edge cannot bleed into the neighbouring cascade's tile — the
   atlas's one cost over a texture array, paid here.

3. **Per-cascade bias.** The reference-depth bias is **scaled per cascade** by the cascade's
   world-units-per-texel (coarser far cascades cover more world per texel and need more bias) to
   balance acne against peter-panning. A slope-scaled term (∝ `1 − dot(N, L)`) is folded in. Base
   bias constants stay in-shader (the shadow path is plumbing, not exposed params).

4. **Cross-fade the boundary.** Within a small blend band before a cascade's far split, sample
   both cascade `k` and `k+1` and lerp the two visibilities by the normalized distance into the
   band, so the resolution change across a cascade edge is a gradient, not a hard line. The last
   cascade fades to full visibility past its far plane. The band width rides `ShadowParams`.

Point/spot lights still pass visibility `1.0` (unshadowed) — CSM is the directional term only.

### `DebugView::Cascades` — the arm that pins the golden move ([engine/include/Veng/Renderer/SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h) + the lighting/debug path)

The golden moves in this plan (the `SampleCmp` cascade sampling changes the shadow, and the SSAO
fix below changes the AO term), and a re-blessed image alone is not an acceptable guard. So the
`DebugView::Cascades` arm lands **here**, with the selection logic it visualizes — not deferred to
Plan 05 — so the golden move is pinned by a CSM-distinguishing assertion at the moment it moves.
The arm tints the lit result by which cascade each fragment selected (0 red, 1 green, 2 blue, 3
yellow), reusing the exact view-depth → cascade-index logic from the selection step above, and
terminates the chain like the existing `Shadows`/`AO` debug arms. Plan 05 only wires the matching
`SceneRendererSettings` knobs and the example debug-UI exposure on top.

## Decisions

1. **Hardware `SampleCmp`, not manual PCF.** The comparison sampler lives in the dedicated set 1
   (Plan 03), outside set 0's bindless argument buffer, so the MoltenVK mistranslation that bars
   it from bindless does not apply. This is the normal, faster, smoother shadow path; the manual
   in-shader compare is gone. The validation gate (`VE_DEBUG`) pins that MoltenVK accepts the
   comparison sampler + `SampleCmp` in a plain descriptor set — the one residual portability risk
   this re-architecture takes on, verified rather than assumed.

2. **Select by view-space depth against `CascadeSplits`.** A direct compare (splits stored in
   view space, Plan 02 decision 4) — no per-cascade containment test. The fragment's view depth
   comes from the world position the pass already reconstructs.

3. **Per-cascade bias scaling.** A single world-space bias over-darkens near cascades and
   under-corrects far ones; scaling by world-units-per-texel balances them. With `SampleCmp` the
   bias is applied to the reference depth passed to the compare.

4. **A blend band, not a hard select.** The cross-fade is the standard CSM seam fix; the band
   width is a small fraction of the cascade range (on `ShadowParams`, not a user setting).

## Files

| File | Change |
|---|---|
| `engine/assets/core/shaders/view_constants.slang` (new) | The single canonical lean `ViewConstants` (`InvViewProj`, `CameraPosition`, `View`, `Proj`) + its set-0 binding + `LoadViewConstants` at the **one** canonical stride (`BindlessRegistry::ViewConstantsStride`). This **reconciles three divergent in-engine declarations**, not three identical copies: `material.slang` and `deferred_lighting` carried the full block (with the now-evicted `LightViewProj`/`ShadowParams`) at stride 512, while `ssao.frag.slang` declared a reduced block at stride **256**. After Plan 03 evicts the shadow fields to set 1, the residual lean block is common to all three; each `#include`s this file instead of redeclaring. |
| `engine/assets/core/shaders/shadow.slang` (new) | The set-1 `ShadowConstants` struct + the atlas/`SamplerComparisonState` binding declarations + the cascade-select/`SampleCmp` helpers. |
| `engine/assets/core/shaders/deferred_lighting.frag.slang` | `#include` `view_constants.slang` + `shadow.slang`; cascade selection by view depth; per-cascade tile `SampleCmp` with scaled bias + tile-UV clamp; boundary cross-fade. (The `_ssao` variant inherits all of this via its `#include`.) |
| `engine/assets/core/shaders/{material,ssao}.slang` | `material.slang` `#include`s `view_constants.slang` (drops its own `ViewConstants`). `ssao.frag.slang` likewise — and this **fixes a latent bug**: it currently reads the shared 512-stride buffer at stride **256** with a struct omitting `LightViewProj`/`ShadowParams`, so its `View`/`Proj` read at the wrong byte offsets and (on frame-in-flight ≥1) the wrong frame's region. The capture only ever exercises frame-in-flight 0, so it went unnoticed. Adopting the canonical block/stride corrects SSAO — its AO output **legitimately changes**, so the golden re-bless below must expect a (correct) shift in the AO term, not a regression. |
| `tests/gpu/assets/shaders/material_data.slang` | The cross-pack mirror (a game pack can't yet `#include` the engine header — the cross-pack include facility is future work). It already declares an **ultra-lean** `{InvViewProj, CameraPosition}` at stride 256; bring it to the **canonical** lean block + stride so it matches what the engine writes (add `View`/`Proj` only if the fixture's surface shader reads them — confirm by grep; a surface vertex stage typically needs neither). It carries **only** the view block, never `ShadowConstants` (set 1, which a surface fixture does not bind). |

## Verification

- Clean build; `ctest` green across the bands.
- **Before trimming `material.slang`, a grep confirms no Surface-domain shader reads
  `ViewConstants.LightViewProj`/`ShadowParams`** (the evicted fields) — converting the
  "unused by surface shaders" assumption into a checked fact before the struct loses them.
- **`gpu` band:** the directional shadow is cast on the receiver (the presence assertion holds).
  But presence alone cannot distinguish correct CSM from a broken cascade (a transposed tile
  remap, an inverted compare, or all-fragments-select-cascade-0 still darken the receiver), so
  add a **CSM-distinguishing** assertion via the `DebugView::Cascades` arm (landed in this plan):
  assert a **near** frame region resolves to cascade 0 and a **far** region resolves to a higher
  cascade index (distinct tint buckets) — pinning cascade *selection*, not just shadow presence.
  **This needs a fixture whose visible receiver actually spans a split.** The existing shadow
  fixture (8×8 plane, camera near 0.1 / far 100 at eye (0,5,5)) is degenerate for this: its whole
  visible surface lands in cascade 1 (view-depths ≈ 4.2–9.9 against splits 4.2 / 10.2 / 26.4), so
  it can never show cascade 0 or 2. Add a **grazing-view fixture** — a long receiver
  (`Plane(vec2(80))`) viewed down its length (eye (0,3,20) → target (0,0,−20)) — whose visible
  view-depths span ≈ 1.2 (cascade 0) → 20 (cascade 2) → 60 (cascade 3), bottom-to-top of frame.
  **Validation gate under `VE_DEBUG`** clean — the set-1 comparison sampler + `SampleCmp` +
  dynamic-offset uniform carry no validation error on MoltenVK (the dedicated-set hardware-compare
  path this planset bets on).
- **`smoke_golden` moves** (the `SampleCmp` cascade sampling makes the sharper shadow visible,
  **and** the SSAO stride/offset fix shifts the AO term); regenerated per the `CLAUDE.md`
  procedure with both expected changes stated (a crisper directional shadow with no cascade seam,
  plus a corrected — not regressed — AO contribution in the fixed smoke pose). The smoke PPM is
  the correct size and the launcher exits 0.
