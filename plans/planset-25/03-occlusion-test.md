# Plan 03 — Hi-Z occlusion-test primitive

**Goal:** build and prove the **conservative hi-Z occlusion test** — project a candidate's
world AABB to a screen-space footprint, select the matching pyramid mip, and report whether the
candidate is fully behind the stored max depth — as a standalone, test-pinned primitive that
**drops no draw yet**. It is wired into drawing only by the compute cull (Plan 05). Depends on
Plan 02 (the pyramid). Golden unmoved.

## What lands

### The occlusion test, in a shared shader header

A core-pack Slang header (`hi_z_occlusion.slang`) carries the test as a pure function so the
isolation compute pass (this plan) and the cull compute pass (Plan 05) share one
implementation:

```hlsl
// Returns true if the world AABB is provably hidden behind hiZ over its screen
// footprint. Conservative: a straddling, partially-on-screen, or near-plane-
// crossing box returns false (draw it). hiZ stores max (farthest) depth per texel.
bool IsOccluded(float3 boundsMin, float3 boundsMax, float4x4 prevViewProj,
                Texture2D<float> hiZ, uint2 hiZBaseExtent, float depthBias);
```

The test:

1. **Project the 8 corners** through `prevViewProj` (the *previous* frame's camera view-projection
   — the pyramid is last frame's). If any corner is behind the near plane (`w <= 0`), return
   **false** (not occluded) — a near-crossing box cannot be screen-bounded safely, so it draws.
2. **Screen-space AABB:** take the min/max of the 8 projected `xy` (NDC → `[0,1]` UV) and the
   **min** projected depth (the candidate's nearest point — `z_min`).
3. **Mip selection:** choose the mip whose texel covers the footprint's pixel extent
   (`level = ceil(log2(max(footprintWidthPx, footprintHeightPx)))`), so the footprint spans at
   most a 2×2 texel neighborhood at that level — the standard hi-Z LOD pick.
4. **Test:** sample the (up to 4) covered texels at that mip, take their **max** stored depth
   `d_far`. The candidate is occluded iff `z_min > d_far + depthBias` — its nearest point is
   behind the farthest occluder depth over the whole footprint. The bias absorbs reduction
   quantization; conservatism means a tie draws.

A footprint that falls partly off-screen, or whose UV is outside `[0,1]`, returns **false**
(draw) — off-screen-but-shadowing geometry and frustum-edge cases are the frustum cull's job,
not occlusion's.

### The isolation compute pass (this plan only)

To prove the test before Plan 05 wires it into indirect draw, this plan adds a compute pass that
reads a candidate buffer and writes a **per-candidate visibility byte** (1 = visible, 0 =
occluded) to a result buffer the test harness reads back. This pass is **not** part of the
render path — it exists to pin `IsOccluded` against known scenes — and it can be driven directly
from a `gpu`-band test fixture (mirroring [tests/compute_dispatch.cpp](../../tests/compute_dispatch.cpp))
rather than the renderer's graph, so it needs no buffer-resource graph surface (that is Plan 04).
The candidate/result buffers are plain `Buffer`s bound through an **explicit `DescriptorSet`/
`DescriptorSetLayout`** (the [tests/compute_dispatch.cpp](../../tests/compute_dispatch.cpp)
precedent — there is no bindless storage-*buffer* binding; set-0 bindless arrays are
sampled/storage *images*, samplers, and the fixed material/view/light buffers), populated by the
test.

### History-invalid → frustum-only (the correctness guarantee)

`SceneRenderer` tracks whether the previous-frame pyramid is **valid** to test against:

- **Frame 0** (no history) — invalid.
- **The frame after a `Resize`/`Configure`** recreated the pyramid (different extent / new
  resource) — invalid.
- **A large view delta** (the camera moved/rotated enough that last frame's depth is unrelated)
  — invalid. The metric is explicit and biased toward invalidation (drawing more is free; a
  missed invalidation risks a one-frame false-cull): invalidate when **either** the camera
  translated more than a fraction of the scene-bound diagonal (default **2%**, so a teleport-sized
  cut trips it while a normal step does not) **or** the forward axis rotated more than a small
  angle (default **10°**), comparing this frame's camera position/orientation to last frame's; the
  **projection changing at all** (FOV / near-far / aspect) also invalidates. The thresholds are
  named constants, tunable, with the safe direction documented (lower = more frustum-only frames).

When invalid, occlusion is **skipped for that frame** — the cull is frustum-only, drawing every
frustum survivor. This is what makes "occlusion never drops a visible draw" hold across
discontinuities: stale or absent history can only cause an object to be *drawn* (conservative),
never wrongly culled. The flag is computed in `Execute` and consumed by Plan 05's cull; this
plan lands the flag and its unit-tested transitions.

## Decisions

1. **Conservative on every uncertainty.** A near-plane crossing, an off-screen footprint, a
   straddling depth, or an invalid-history frame all resolve to **draw**. Occlusion is a pure
   optimization with the same correctness bar as frustum culling — it may leave a hidden draw in
   (a missed optimization) but must never drop a visible one (an artifact). Every branch above is
   chosen to fail toward drawing.

2. **Test against the previous frame's pyramid and view-projection together.** The pyramid is
   last frame's depth, so the projection that screen-bounds a candidate against it must also be
   last frame's `viewProj` — mixing this frame's projection with last frame's depth would
   misalign the footprint. The pair is captured at end of frame for next frame's test.

3. **Prove in isolation before wiring.** Landing the test as a result-buffer-writing pass pinned
   by a readback test (this plan) separates "is the math right?" from "is the draw path right?"
   (Plan 05). A min/max projection or mip-pick bug surfaces in a focused test with a known
   occluder, not as a golden drift buried under the indirect-draw rework.

4. **History invalidation is the renderer's, not the shader's.** The shader tests whatever
   pyramid + projection it is handed; the *decision* not to trust last frame's pyramid (resize,
   first frame, view cut) is renderer state, so a wrong call there disables occlusion for a frame
   rather than producing a stale cull. Keeping the guarantee on the CPU side keeps it auditable.

## Files

| File | Change |
|---|---|
| `engine/assets/` core pack (`hi_z_occlusion.slang`) | The shared `IsOccluded` test header; placeholder `AssetId` for the isolation compute shader. |
| `engine/src/Renderer/SceneRenderer.cpp` (+ header) | The previous-frame `viewProj` capture; the `m_HiZHistoryValid` flag + its frame-0/resize/view-delta transitions. |
| `tests/gpu/` (new `occlusion_test.cpp`) + the gpu suite source list | Drive the isolation compute pass over known scenes; assert the occluded/visible bytes. |
| `tests/unit/` (history-valid transitions) | Device-free: the validity flag is false on frame 0 / post-resize / large-view-delta, true otherwise. |

## Verification

- Clean build; `include_hygiene` unaffected.
- **GPU occlusion test** (`gpu` band): a fixture with an **occluder fully covering** a smaller
  mesh behind it → the covered mesh's visibility byte is **0** (occluded); the occluder's is
  **1**. A mesh **straddling** the occluder edge → **1** (draw — conservative). A mesh **in front
  of** the occluder → **1**. A mesh **crossing the near plane** → **1**. A footprint **off-screen**
  → **1**.
- **History-validity unit tests** (device-free): the flag is false on the first `Execute`, false
  on the `Execute` immediately after a `Resize`, false on a translation/rotation **just over** the
  threshold and on any projection change, true on a delta **just under** the threshold and on a
  steady-camera steady-extent frame — both sides of the boundary, the property that makes the
  conservative guarantee hold across discontinuities.
- **`smoke_golden` byte-identical** — the isolation pass writes only a result buffer nothing
  draws from; the render output is unchanged. Re-check, do not regenerate.
- **Validation gate clean under `VE_DEBUG`**; full `ctest` green across the unit/death/cooker
  bands and the `gpu` band where present.
</content>
