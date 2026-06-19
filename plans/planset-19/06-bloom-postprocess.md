# Plan 06 — bloom as a PostProcess material

**Goal:** add bloom to the HDR chain, authored as a **tunable PostProcess-domain material**
run by the shipped `PostProcessScenePass` (planset-18) — not a bespoke C++ pass. This proves
the authorable post stack scales past tonemap *and* delivers the first **multi-stage chained
PostProcess path**: a bright-pass threshold + separable blur composited into the HDR result
before tonemap, with `Threshold`/`Intensity` as exposed material params. Depends only on
Plan 01 (a lit HDR image worth blooming); independent of 02–05.

## What lands

### The core bloom material(s)

A core PostProcess material (`bloom.vmat`, in `engine/assets/core/materials/`) and its
fragment shader(s), following the tonemap material's shape:

- **Inputs** — the upstream HDR target runtime-bound as a material texture handle field
  (`Material::SetTextureHandle`), exactly as tonemap runtime-binds HDR; each later stage
  runtime-binds the prior stage's output the same way.
- **Exposed params** — `Threshold` (luminance knee for the bright pass) and `Intensity`
  (bloom mix), written per frame through the ring-buffered material block.
- **Effect** — a bright-pass extracts pixels above `Threshold`; a **single-level separable
  Gaussian blur** (horizontal then vertical) builds the bloom; the result is added back to
  the HDR target scaled by `Intensity`.

Bloom is **exactly four fullscreen invocations** — bright-pass → blur H → blur V → composite
— so it is **four chained `PostProcessScenePass` stages** wired through `PassIO` (each a
fullscreen material invocation over the prior stage's target), composed by `SceneRenderer`
into the HDR chain ahead of tonemap. Intermediate stage targets are **renderer-owned imported
images** (registered into bindless): a later stage samples the prior stage's output through a
bindless `TextureHandle`, which needs a `Ref<ImageView>` — a graph transient (per-frame
`ImageView&` only) cannot be registered, so transients are not an option here. Each
intermediate is single-copy, consumed by the next stage within the internal graph (the graph
derives the read-then-write barriers), and recreated through the deferred `Release()` path on
`Resize`/`Configure`. New core shader + material `AssetId`s are minted in the final pass.

### `SceneRendererSettings` gains the bloom toggle

```cpp
bool Bloom = true;   // topology: the bloom stages on/off
```

A topology change (`Configure` → recompile). `Threshold`/`Intensity` are **material params**
(per-frame, written through the block), not settings — they tune the effect without
recompiling, the same split tonemap's `Exposure` uses. (A small `SceneRendererSettings`
mirror for them is optional ergonomics; the canonical home is the material's exposed params.)

## Decisions

1. **Bloom is a material, shadows/SSAO are not.** Bloom is a **tunable effect** with exposed
   knobs (threshold, intensity) — exactly what a PostProcess material is for. Shadows and
   SSAO are **plumbing** with no authorable surface, so they stay hardcoded `ScenePass`
   units. This is the plumbing-vs-effect line the tonemap planset drew, now applied to a
   second effect.

2. **Bloom delivers the multi-stage chained PostProcess path.** Tonemap exercised a single
   `PostProcessScenePass` invocation; bloom chains **four** instances with renderer-owned
   ping-pong intermediates, each runtime-binding the prior stage's output. This is a genuine
   extension of the planset-18 mechanism — the renderer owns the chain order and the
   intermediate targets, each pass owns its invocation — not a free reuse. No new *pass
   type*, but the first time the post stack is more than one stage deep.

3. **Threshold/Intensity are params, not settings.** They vary continuously without changing
   topology, so they ride the ring-buffered material block (per-frame writes, no recompile);
   `Bloom` on/off is the only topology knob.

4. **Composited before tonemap.** Bloom operates in linear HDR and is added before the
   tonemap maps HDR → output, the standard order; tonemap stays the final PostProcess
   material in the chain.

## Files

| File | Change |
|---|---|
| `engine/assets/core/materials/bloom.vmat.json` (new) | PostProcess material: HDR input handle field, `Threshold`/`Intensity` exposed params; minted ids. |
| `engine/assets/core/shaders/bloom_*.frag.{slang,shader.json}` (new) | Bright-pass + separable-blur + composite fragment stages; minted ids. |
| `engine/src/Renderer/SceneRenderer.cpp` | Wire the four-stage bloom `PostProcessScenePass` chain into the HDR chain ahead of tonemap when `Bloom`; own/recreate the intermediate targets (deferred `Release()`); runtime-bind each stage's input + drive params per frame. |
| `engine/include/Veng/Renderer/SceneRenderer.h` | `Bloom` setting. |
| `tests/...` | A gpu test asserting a bright region blooms with `Bloom` on vs off; `Threshold`/`Intensity` change the result without a recompile. |

## Verification

- Clean build; the core bloom material cooks into the core pack and loads through the normal
  `AssetManager` path (a PostProcess material like tonemap).
- `gpu` band + `smoke_golden`: regenerated to show bloom on a bright highlight; `Bloom=false`
  matches the no-bloom HDR→tonemap result; changing `Threshold`/`Intensity` per frame does
  not recompile.
- Validation gate clean (the four chained fullscreen passes carry graph-derived transitions
  between the HDR target and the renderer-owned blur intermediates; recreates go through the
  deferred `Release()`).
