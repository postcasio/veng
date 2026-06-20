# Plan 02 — Hi-Z depth pyramid

**Goal:** build a **hi-Z depth pyramid** — a max-Z mip chain reduced from the g-buffer depth
target by a compute pass — as a renderer-owned resource **persisted across frames**, so the
occlusion test (Plan 03) can sample *last frame's* pyramid. This plan builds and proves the
pyramid; it changes no draw and moves no golden. Depends on nothing in this planset; the
occlusion test (03) consumes it.

## What lands

### The hi-Z resource ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))

A renderer-owned `Ref<Image>` + per-mip `Ref<ImageView>`s, `R32Sfloat`, sized to the depth
target's extent with a full mip chain (`floor(log2(max(w,h))) + 1` levels), created in
`CreateGBuffer` beside `m_DepthImage` and recreated on `Resize`. It is **`ImageUsage::Storage |
ImageUsage::Sampled`** (the reduction writes each mip as a storage image; the occlusion test
samples it). Unlike the g-buffer/depth targets it is **not** cleared and **not** a graph
transient — it carries data from one frame to the next (decision 4 of the planset), so it is a
persisted renderer-owned image, registered into bindless once at `Create` (re-registered on
`Resize`) like the other sampled targets.

```cpp
// The hi-Z pyramid: a max-Z mip chain over the depth target, built by compute at
// the end of each Execute and sampled by next frame's occlusion test. Persisted
// across frames (temporal hi-Z), so it is renderer-owned, not a graph transient.
Ref<Image>              m_HiZImage;
vector<Ref<ImageView>>  m_HiZMips;        // one storage view per mip level
StorageImageHandle      m_HiZMipHandles[…];
TextureHandle           m_HiZSampleHandle;  // the full chain, sampled by Plan 03
```

### The reduction compute pass

A core-pack compute shader (`hi_z_reduce.comp.slang`, placeholder `AssetId` until verified)
reduces one mip into the next: each invocation reads a 2×2 (or larger) footprint of the source
mip and writes the **max** depth to the destination texel. Two source kinds:

- **Mip 0** samples the **depth target** (`m_DepthImage`, `D32Sfloat`) and writes
  `m_HiZMips[0]`. Reading a depth-format image as a sampled/storage source is the MoltenVK
  gotcha to verify (decision 2).
- **Mip n>0** reduces `m_HiZMips[n-1]` → `m_HiZMips[n]`.

The reduction is a **chain of compute dispatches** declared in the renderer's internal graph
after the lighting/tonemap passes (so it reduces *this* frame's completed depth), each mip a
`Dispatch(ceil(w/8), ceil(h/8), 1)` over an 8×8 workgroup, dispatch *k* reading mip *k-1* and
writing mip *k*.

**This needs a per-mip (subresource) graph surface the image-only graph does not have today**
(decision 5). `PassContext::Resolved(ResourceId)` resolves to a *single* `ImageView`
([RenderGraph.h:82](../../engine/include/Veng/Renderer/RenderGraph.h)), and barrier state is
tracked per whole image — so a chain of dispatches that read mip *k-1* and write mip *k* of the
*same* image can neither bind the two distinct mip views from one handle nor get the per-mip
read-after-write barrier derived (the whole-image tracker sees only "writes the image, reads the
image"). Plan 02 extends the graph to address an individual mip level:

- **`ImportImageMips(name, levelCount)`** — imports a renderer-owned image as `levelCount`
  per-mip subresource resources; the renderer supplies the per-mip `Ref<ImageView>`s
  (`m_HiZMips`), and mip 0's source depth view, as `ImportBinding`s per frame.
- **`PassContext::ResolvedMip(ResourceId, u32 level)`** — the per-frame concrete mip view a
  dispatch records against, the subresource analogue of `Resolved(id)`.
- **Per-subresource barrier state** in the graph's resource tracker / `BarrierDecision`, keyed on
  `(image, mipLevel)`, so dispatch *k+1*'s `.StorageRead` of mip *k* after dispatch *k*'s
  `.StorageWrite` of it derives a `General/ShaderWrite → General/ShaderRead` barrier on **that mip
  only**, and the depth source's `DepthAttachment → ShaderReadOnly` transition falls out the same
  way.

This is net-new graph infrastructure, parallel in weight to Plan 04's buffer-resource class — the
hi-Z chain is its first consumer; any future mip-chain/downsample pass reuses it. (It is
independent of Plan 04's buffer surface — a different axis of the same graph.) Compute pipelines,
`Dispatch`, and storage-image bindless slots already exist
([CommandBuffer.h:183](../../engine/include/Veng/Renderer/CommandBuffer.h),
[BindlessRegistry.h](../../engine/include/Veng/Renderer/BindlessRegistry.h)) with a working
precedent in [tests/compute_dispatch.cpp](../../tests/compute_dispatch.cpp).

### Odd-extent handling

A mip whose parent has an odd dimension must include the extra row/column in its max so no depth
sample is dropped from the pyramid (a dropped far sample would make the test *less*
conservative — it could report a footprint as occluded when an unsampled near texel was
actually in front). The reduction reads a 3×3 footprint when the parent dimension is odd,
clamped to the parent extent — the standard hi-Z odd-mip fix. This is the single correctness
subtlety the CPU reference test (below) pins.

## Decisions

1. **Max-Z reduction, matching the engine's depth convention.** veng uses `D32Sfloat` with the
   standard Vulkan `[0,1]` depth (near = 0, far = 1, [GBuffer.h:50](../../engine/include/Veng/Renderer/GBuffer.h)),
   so "fully behind the nearest stored depth over a footprint" means a candidate whose nearest
   screen-space depth is **greater** than the pyramid's stored max over that footprint. The
   reduction therefore stores the **max** (farthest) depth per texel, and a coarser mip's texel
   is the max of its children — so testing against a single coarse texel is conservative (it
   represents the farthest geometry in the footprint). A min reduction would invert the test and
   cull visible geometry; the convention is pinned here so Plan 03 does not re-derive it.

2. **Reading the depth target in compute is a verified MoltenVK step.** Sampling/loading a
   `D32Sfloat` image as a compute source is platform-sensitive (depth-format storage/sample
   support, the tile-store of the depth target out of tile memory). Verify support on the dev
   box and, if a direct depth load is unsupported, copy/blit depth into an `R32Sfloat` mip-0
   first. Either way validate under `VE_DEBUG`.

3. **Persisted across frames, renderer-owned — not a transient.** The occlusion test reads last
   frame's pyramid (temporal hi-Z, planset decision 4), but the depth target is single-copy and
   cleared each frame, so the pyramid must be a resource that *survives* the frame. This is the
   reserved cross-frame-read case the renderer's frames-in-flight contract names
   ([engine/CLAUDE.md](../../engine/CLAUDE.md), frames-in-flight): the pyramid is written (the
   reduction) at the end of frame *n* and sampled (the occlusion test) at the start of frame
   *n+1*. Both record on the single graphics queue in submission order, so no ring or semaphore is
   needed — but the cross-frame transition is **renderer-recorded, not graph-derived**: the
   internal graph compiles once and replays and cannot see across frames (the same reason the
   single-copy output handoff uses an explicit `PrepareForAccess` bracket, not a derived barrier).
   The renderer records the pyramid's transition from last frame's storage-write state to this
   frame's sampled-read state at the start of `Execute`, the mirror of the reduction chain's final
   write. A ring is the escalation only if a later async/temporal consumer reads an older frame.

4. **Known cost, stated.** On the tile-based MoltenVK GPU, the reduction stores the depth target
   out of tile memory and reads it back, then builds a full mip chain — strictly more bandwidth
   than the no-occlusion path, the same store-then-sample tax the deferred g-buffer already pays
   ([future/scene-renderer.md:310](../future/scene-renderer.md)). It is justified only above a
   candidate count where the saved draws outweigh it; on the one-cube smoke scene it is pure
   overhead, which is why occlusion is a toggle (Plan 06) and the smoke path leaves it off. As an
   order of magnitude, the build + test is expected to pay off past ~hundreds of candidate
   submeshes per view, or once planset-24's `N + 6N` shadow views reuse the single upload
   (decision 9) — below that the CPU frustum cull alone wins, the reason CPU is the default.

5. **A per-mip subresource graph surface, not a side channel.** The reduction's per-mip
   read-after-write hazards are exactly what the graph exists to derive, but the image-only graph
   resolves one `ImageView` per resource and tracks state per whole image, so it cannot address or
   barrier an individual mip. Smuggling the mip chain past the graph (renderer-held views with
   hand-written per-mip barriers) would re-introduce the hand barriers `RenderGraph` abolished and
   break under the validation gate. Extending the graph with per-mip imports + per-subresource
   barrier state keeps the chain's barriers derived and is reusable by any future downsample pass —
   the same "make it a first-class graph resource" call Plan 04 makes for buffers, on the image
   axis. The hi-Z chain is its first and only consumer this planset.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/RenderGraph.h` + `engine/src/Renderer/RenderGraph.cpp` | `ImportImageMips`, `PassContext::ResolvedMip`; per-mip subresource resources in the resource table + compile/resolve. |
| `engine/src/Renderer/Backend/BarrierDecision.cpp` (+ the graph tracker) | Per-subresource (`image`, `mipLevel`) barrier state so per-mip RAW transitions derive. |
| `engine/src/Renderer/SceneRenderer.cpp` (+ its header) | `m_HiZImage`/`m_HiZMips`/handles; create + register in `CreateGBuffer`, recreate on `Resize`; the renderer-recorded cross-frame transition (decision 3); declare the reduction compute chain in the internal graph after tonemap. |
| `engine/assets/` core pack (`hi_z_reduce.comp.slang` + `.shader.json`) | The max-Z reduction compute shader; placeholder `AssetId` until minted. |
| `tests/gpu/` (new `hi_z_pyramid.cpp`) + the gpu suite source list | Build-and-readback correctness test vs a CPU reference reduction. |

## Verification

- Clean build; `include_hygiene` unaffected (no public-header change).
- **GPU correctness test** (`gpu` band, `SKIP_RETURN_CODE 77` with no ICD): render a known depth
  field into the depth target, run the reduction, read back each mip, and compare against a
  **CPU reference reduction** (the same max-of-footprint, including the odd-extent 3×3 clamp) —
  every mip texel matches. A field with an odd extent exercises the odd-mip path; a field with a
  single far texel in a near footprint verifies the far value survives to the coarsest mip (the
  conservatism property).
- **`smoke_golden` byte-identical** — this plan adds a pass that writes only the hi-Z resource,
  which nothing samples yet; the rendered output is unchanged. Re-check, do not regenerate.
- **Per-subresource barrier unit test** (device-free): a `.StorageWrite` on mip *k* followed by a
  `.StorageRead` of mip *k* derives a `General/ShaderWrite → General/ShaderRead` barrier on that
  mip; a read of a *different* mip derives none — the per-`(image, mipLevel)` tracking the chain
  relies on, pinned like Plan 04's buffer barrier-decision case.
- **Validation gate clean under `VE_DEBUG`** — the depth `→ ShaderReadOnly` and per-mip
  storage-image transitions fall out of the declared per-subresource graph use; pin the
  `validation_gate` run. The benign MoltenVK buffer-robustness WARN is unaffected.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
</content>
