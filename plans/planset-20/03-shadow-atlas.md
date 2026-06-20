# Plan 03 — shadow atlas + dedicated-set handoff + multi-cascade render

**Goal:** stand up the GPU side of CSM — evolve `ShadowScenePass` into a cascaded pass that
owns a depth **atlas** (off bindless), render the scene depth once per cascade into its tile,
and add the **dedicated-set handoff** that delivers the atlas + an immutable comparison
sampler + a dynamic-uniform `ShadowConstants` buffer into the lighting pipeline's set 1.
`SceneRenderer::Execute` computes the cascades each frame (Plan 02), threads the raw matrices to
the shadow pass via `SceneView`, and writes the tile-remapped matrices + splits into
`ShadowConstants` — the per-view set-0 block is trimmed to camera/view state.

## What lands

### `ShadowScenePass` → a cascaded atlas pass, off bindless ([engine/src/Renderer/ShadowScenePass.{h,cpp}](../../engine/src/Renderer/ShadowScenePass.h))

The pass owns one D32 depth **atlas** of `ShadowResolution²` tiles, `DepthAttachment | Sampled`,
created at `Create`/`Resize`/`Configure` and recreated through the deferred `Release()` window.
It is **no longer registered into bindless** and exposes no `TextureHandle`; instead it exposes
its `Ref<ImageView>` (`GetShadowView()`) for the dedicated-set handoff, plus `GetCascadeCount()`
and the tile layout. The atlas is **sized to `CascadeCount`** — tiles laid out in a
`min(Count, 2)`-column × `ceil(Count / 2)`-row grid (1×1 for Count 1, 2×1 for 2, 2×2 for 3–4) —
so a low cascade count pays for no idle tiles. Cascade `k` maps to tile sub-rect `(k % 2, k / 2)`;
the one grid cell beyond `Count` (the 4th cell at Count 3) is cleared to depth = 1 by the single
attachment clear and never selected, so sampling an uninitialized tile cannot occur.

The per-cascade `ShadowResolution` **default drops to 1024** (the field's default in
`SceneRenderer.h`, set in Plan 05): the prior single shadow map was a 2048² D32 (≈16 MB), and a
default 4-cascade atlas at 1024 per tile is a 2048² D32 — **the same footprint**, now split four
ways for tighter near-camera density rather than spent on one map. (At the old 2048 default a 2×2
atlas would have been 4096² ≈ 64 MB — a 4× regression this avoids.)

**The `DebugView::Shadows` blit is repaired in this plan, not left broken.** Moving the atlas
off bindless breaks the existing bindless shadow-debug blit (it reads `io.ShadowHandle`). Rather
than leave a broken debug arm across the commit boundary, Plan 03 repoints the `Shadows` blit
onto the same dedicated-set bound-view seam when it moves the atlas, so the arm and its `gpu`
assertion stay green at this plan's close. The blit visualizes **raw depth**, so it samples the
atlas with an **ordinary (non-comparison) sampler** — *not* set 1's immutable comparison sampler,
which would return compare results rather than the depth value; the blit binds the atlas as a
plain sampled image with a standard bindless `SamplerHandle`. Plan 04 then adds the new `Cascades`
arm on top.

`Declare` contributes **one** depth-only `RenderGraph` pass that writes the whole atlas:

- One depth attachment (the atlas), cleared to depth = 1 at `BeginRendering`.
- For each active cascade `k`, set the **viewport + scissor** to cascade `k`'s tile sub-rect
  and draw the scene's opaque meshes with cascade `k`'s light matrix pushed (the existing
  per-submesh draw loop, run once per cascade). The depth-only pipeline and `shadow_depth.vert`
  (light-space-MVP push at offset 0) are **unchanged** — only the pushed matrix and the
  viewport differ per cascade.

### The `PassIO` bound-view seam ([engine/include/Veng/Renderer/ScenePass.h](../../engine/include/Veng/Renderer/ScenePass.h) + `PassIO`)

Today every internal producer→consumer image hop reaches a consumer as a bindless
`TextureHandle` through `PassIO`. The shadow handoff needs a **second delivery kind**: a
producer's `Ref<ImageView>` bound into a consumer's **dedicated descriptor set**, not a
bindless slot. `PassIO` gains a named **bound-view** slot carrying a `Ref<ImageView>` (and the
target layout it will be sampled in); the renderer wires the shadow pass's `GetShadowView()`
into the lighting pass's bound-view slot at `Configure`. This is the generalizable seam — any
future closed producer→consumer sample (not just shadows) uses it instead of bindless.

The shadow resource stays an `Import`ed, graph-declared resource: the lighting pass still
declares `.Sample(shadow)` so the graph derives the `DepthAttachment → ShaderReadOnly`
transition. **Binding (dedicated set) and barrier (graph-declared `.Sample`) are separate
concerns** — only the binding moves off bindless.

### The lighting pipelines gain set 1 — the whole shadow system ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))

The deferred-lighting pass is a hardcoded engine `ScenePass`, so its pipeline layout is engine
code. There are **two** lighting pipelines — `m_LightingPipeline`/`m_LightingLayout` and the
AO-fold variant `m_SsaoLightingPipeline`/`m_SsaoLightingLayout`, selected by `Settings.AO` — but
**one** fragment body: `deferred_lighting_ssao.frag.slang` is `#define VE_USE_SSAO 1` +
`#include "deferred_lighting.frag.slang"`. So the shader-side shadow work (Plan 04) is authored
once in the shared body and both variants inherit it; only the **two pipeline layouts** each gain
the set-1 declarations here. Set 1 holds the complete directional-shadow system:

- **binding 0** — the shadow atlas (sampled image).
- **binding 1** — an **immutable comparison sampler** baked into the layout (`compareEnable`,
  `compareOp` LESS-or-equal, linear filter for the hardware 2×2 PCF).
- **binding 2** — the **`ShadowConstants` dynamic uniform buffer** (the cascade matrices +
  splits + params, below).

**Set 0 stays reserved for the bindless registry; the shadow layout lands at Vulkan set index 1.**
Both lighting layouts have an **empty** `DescriptorSetLayouts` today — they reach set 0 purely
through `BindlessRegistry::Bind`, which binds the registry's set against every pipeline layout
(`PipelineLayout` reserves index 0 for it). The set-1 shadow layout must therefore land at
descriptor-set **index 1** with index 0 left as the reserved registry slot. Confirm how
`PipelineLayout` assigns indices (a reserved/registry entry at 0, author sets at 1+) before
wiring, and add a validation-gate assertion that set 0 = registry and set 1 = shadow coexist on
this pipeline.

Bindings 0–1 are written **once** at `Create`/`Configure`/`Resize`: the atlas is single-copy
(one image reused every frame, serialized by the graph's write→sample barrier), so no per-frame
descriptor write. Binding 2 is the ring (below): one buffer of `framesInFlight` regions, the
**current region selected by a dynamic offset at `vkCmdBindDescriptorSets`**. The pass binds set 1
each `Execute` (with the frame's dynamic offset) alongside set 0 (still bindless, for the
g-buffer).

When shadows are off (the pass compiled out), set 1 binds a 1×1 dummy depth (cleared to 1.0, so
`SampleCmp` yields full visibility) + a zeroed `ShadowConstants` so the layout is always
satisfied. These dummies are **owned by `SceneRenderer`** (allocated at `Create`, long-lived past
any `Configure`), not by the per-recompile lighting pass — the immutable comparison sampler, the
dummy atlas, and the dummy buffer must exist whenever the layout does, independent of the shadow
pass's lifetime.

**Two net-new descriptor-layer extensions land first, each its own device-free-buildable step
before the lighting layout consumes it:**

1. **Immutable samplers.** `DescriptorBinding` carries no `ImmutableSamplers` field today and
   `DescriptorSetLayout` never sets `pImmutableSamplers`. Add a
   `DescriptorBinding::ImmutableSamplers` (a `vector<Ref<Sampler>>`) threaded into the backend's
   `vk::DescriptorSetLayoutBinding`, and a write path that binds an immutable-sampler image
   binding with no explicit sampler. `Sampler` already supports a comparison sampler via its
   create info; `DescriptorType::SampledImage` + `Sampler` already exist (so the split set-1
   layout is expressible).

2. **Dynamic uniform buffers.** The dynamic-offset bind path does **not** exist today —
   `DescriptorType` has no `UniformBufferDynamic`, `DescriptorSetBindInfo` has no offsets field,
   and `BindDescriptorSets` passes `nullptr`/0 for `pDynamicOffsets`/`dynamicOffsetCount`. Add
   `DescriptorType::UniformBufferDynamic` (+ its exhaustive `TypeMapping` arm), a
   `DescriptorSetBindInfo::DynamicOffsets` (`span<const u32>`), and thread
   `pDynamicOffsets`/`dynamicOffsetCount` through `CommandBuffer::BindDescriptorSets`. This is a
   reusable per-frame-constants mechanism, not shadow-specific.

### The view block is trimmed; the cascades ride a new `ShadowConstants` buffer ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))

The shadow state leaves the per-view block. `ViewConstantsBlock` (set 0, binding 5, ringed by
index-fold) keeps **only genuine camera/view state** — its `LightViewProj` and `ShadowParams`
are removed:

```cpp
struct ViewConstantsBlock          // set 0, binding 5
{
    mat4  InvViewProj;             // 64
    vec4  CameraPosition;          // 16
    mat4  View;                    // 64
    mat4  Proj;                    // 64
};                                 // 208 ≤ ViewConstantsStride (512) — material-facing
```

A new `ShadowConstantsBlock` carries the cascades, bound in **set 1, binding 2** as a
dynamic uniform buffer (ringed `framesInFlight` regions, current region picked by the bind-time
dynamic offset):

```cpp
struct ShadowConstantsBlock        // set 1, binding 2 (std140 uniform)
{
    mat4  CascadeViewProj[MaxCascades];  // 256  (tile-remap baked in — for the lighting sample)
    vec4  CascadeSplits;                 // 16   (per-cascade view-space far distance, packed in one vec4)
    vec4  ShadowParams;                  // 16   (x 1/tileRes, y blend-band, z cascade count, w enabled)
};                                       // 288  (no shadow/sampler handle — the atlas is in set 1, not bindless)
```

It is a **std140 uniform** (not the byte-addressed storage path the set-0 ring uses), so the
layout follows std140 rules: `CascadeViewProj` is a genuine `float4x4[MaxCascades]` (each element
16-byte aligned) and the four splits ride a single `vec4` (`CascadeSplits`) rather than a
`float[4]` — a std140 `float[4]` pads each element to a 16-byte stride, so one `vec4` is both
correct and compact. The CPU `ShadowConstantsBlock` mirrors this padding exactly. The per-frame
**ring stride is `align_up(sizeof(ShadowConstantsBlock), minUniformBufferOffsetAlignment)`** (the
device limit, commonly 256 → 512 for this 288-byte block), and the bind-time dynamic offset is
`frame * ringStride`; a runtime check pins the stride against the queried limit. A `static_assert`
pins `ViewConstantsBlock` against its stride. `ShadowParams` carries no bindless handle (the atlas
is a set-1 binding) — only the texel size, blend band, cascade count, and the enabled gate.

`SceneRenderer::Execute` computes the cascades once per frame and distributes them to the two
consumers — the shadow pass (CPU, raw matrices) and the lighting pass (GPU, tile-remapped):

```cpp
const AABB sceneBounds = SceneBounds(view.World);
const CascadeData cascades = ComputeCascades(
    view.Camera, directionalTravel, sceneBounds,
    { .Count = m_Settings.CascadeCount, .Lambda = m_Settings.CascadeSplitLambda,
      .Resolution = m_Settings.ShadowResolution });
```

`SceneBounds(view.World)` runs every frame but is consumed **only** for the per-cascade
near-plane extension (off-screen casters, Plan 02 step 5); the cascade XY extent comes purely
from the camera frustum slice. With no spatial structure yet it is a full-scene reduction over the
amortized `ComputeWorldMatrices` pass `SceneBounds` already shares — acceptable at current scene
sizes, and the deferred frustum-culling work is what will make it earn a BVH.

- **To the shadow pass (CPU):** `SceneView` replaces its single `mat4 LightViewProj` with
  `mat4 CascadeViewProj[MaxCascades]` + `u32 CascadeCount`, carrying the **raw** (non-tile-
  remapped) cascade matrices — the shadow pass renders each cascade with its raw matrix pushed
  and the viewport placing it in the tile. `ShadowScenePass` is the one reader, migrated in the
  same pass.
- **To the lighting pass (GPU):** the renderer composes each cascade's atlas-tile remap onto
  `cascades.ViewProj`, writes them into `ShadowConstantsBlock.CascadeViewProj`, `cascades.SplitFar`
  into `CascadeSplits`, and the count + enabled gate into `ShadowParams`, flushing the current
  frame's region of the set-1 ring.

The shadow gate (`m_ShadowActive && m_ShadowPass && haveDirectional`) is unchanged; off → count 0
→ the lighting pass reads full visibility. `ComputeCascades` is still **called** when shadows are
off (count 0), and the atlas is still cleared, so no frame reads a stale region.

## Decisions

1. **The atlas leaves bindless; it reaches the lighting pass through set 1.** A shadow map is
   a closed producer→consumer resource, so it does not belong in the global registry. A
   dedicated set with an immutable comparison sampler unblocks hardware `SampleCmp` (Plan 04)
   and sidesteps the MoltenVK argument-buffer bar on comparison samplers in set 0.

2. **The tile remap is baked into each cascade's `ViewProj`.** The atlas-tile transform (NDC →
   the tile's UV sub-rect) is folded into the matrix the renderer packs, so the lighting pass
   samples `mul(CascadeViewProj[k], worldPos)` and lands in the tile — the render viewport and
   the sample agree by construction. `ComputeCascades` (Plan 02) stays tile-agnostic; the
   renderer composes the tile transform from the pass's layout when packing.

3. **One atlas, one pass, N viewports.** All cascades write one depth attachment in one graph
   pass, each via its own viewport; the graph derives one write → one sample barrier. A
   texture array (one view per layer) would force N passes or multiview against veng's
   one-`RenderingInfo`-per-pass model — the atlas keeps rendering single-pass (README
   decision 6).

4. **Set 1 mixes a write-once atlas with a ringed `ShadowConstants`.** The atlas + comparison
   sampler (bindings 0–1) are written at `Create`/`Configure`/`Resize` only — the atlas is one
   image reused each frame (serialized by the graph's write→sample barrier), matching the
   renderer's single-copy contract. The `ShadowConstants` uniform (binding 2) is per-frame data,
   so it rings `framesInFlight` regions in one buffer and the current region is selected by a
   **dynamic offset** at bind — the conventional Vulkan per-frame-constants ring, available here
   because set 1 is a plain descriptor set (set 0's index-fold is only a MoltenVK
   argument-buffer workaround). One `DescriptorSet`, static image bindings, dynamic buffer offset.

5. **Cascade count + tile size are recompile, not per-frame.** Both resize the atlas, so they
   go through `Configure`/`Resize` → recompile (wired to settings in Plan 05). The per-frame
   matrices never recompile.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/DescriptorSetLayout.h` + `engine/src/Renderer/Backend/DescriptorSetLayout.cpp` + `DescriptorSet.cpp` | `DescriptorBinding::ImmutableSamplers`; thread `pImmutableSamplers` into the layout; an immutable-sampler image-write path (no explicit sampler). |
| `engine/include/Veng/Renderer/Types.h` + `engine/src/Renderer/Backend/TypeMapping.h` | `DescriptorType::UniformBufferDynamic` + its exhaustive `TypeMapping` arm. |
| `engine/include/Veng/Renderer/CommandBuffer.h` + `engine/src/Renderer/Backend/CommandBuffer.cpp` | `DescriptorSetBindInfo::DynamicOffsets`; thread `pDynamicOffsets`/`dynamicOffsetCount` through `BindDescriptorSets`. |
| `engine/include/Veng/Renderer/ScenePass.h` (+ `PassIO`) | The bound-view slot kind (a `Ref<ImageView>` + target layout) for dedicated-set delivery. |
| `engine/include/Veng/Renderer/SceneRenderer.h` (`SceneView`) | Replace `mat4 LightViewProj` with `mat4 CascadeViewProj[MaxCascades]` + `u32 CascadeCount` (raw matrices for the shadow pass). |
| `engine/src/Renderer/ShadowScenePass.{h,cpp}` | Atlas (2×2 tiles), **off bindless**; `GetShadowView()`/`GetCascadeCount()`; per-cascade viewport + raw-matrix draw loop reading `SceneView`; repoint the `Shadows` debug blit onto the bound-view seam. |
| `engine/src/Renderer/SceneRenderer.cpp` (both lighting layouts) | Set-1 layout on **both** `m_LightingLayout` and `m_SsaoLightingLayout` (atlas sampled image + immutable comparison sampler + `ShadowConstants` dynamic uniform), set 0 reserved for the registry; write bindings 0–1 on recreate; bind set 1 each `Execute` with the frame's dynamic offset; `SceneRenderer`-owned dummy atlas (cleared to 1.0) + zeroed `ShadowConstants` + comparison sampler that outlive any `Configure`. |
| `engine/src/Renderer/SceneRenderer.cpp` | `ViewConstantsBlock` trimmed to camera/view; new `ShadowConstantsBlock` + its set-1 dynamic-uniform ring; `Execute` runs `ComputeCascades`, threads raw matrices to `SceneView`, composes tile remap into `ShadowConstants`; shadow gate updated. |

## Verification

- Clean build; `ctest` green across the bands.
- **`gpu` band:** the cascaded render produces a shadow on the receiver (the property
  assertion holds); the atlas is sized to `CascadeCount` (2×2 at the default count 4). **A
  cross-frames-in-flight data-correctness test** pins the dynamic-offset ring (the one part of
  this plan that re-uses the mechanism MoltenVK mistranslated in set 0): vary the light/camera
  every frame and render past `framesInFlight` frames, asserting the shadow tracks the current
  frame's matrices — a wrong dynamic offset reads a stale region and fails this, where the
  validation gate (a data hazard, not a layout error) would not. **Validation gate under
  `VE_DEBUG`** clean — the atlas's `RenderingInfo` (one depth attachment), the per-cascade
  viewport draws, the set-1 comparison-sampler descriptor, the dynamic-offset uniform bind, and
  the depth-as-texture transition carry the graph-derived/declared layout with **no MoltenVK
  validation error** (the dedicated comparison sampler + dynamic uniform offset outside the
  argument buffer is the specific thing this gate pins).
- **`smoke_golden` moves** (regenerated per the `CLAUDE.md` procedure): the shadow on the
  receiver is rendered through the near cascade — sharper than the fixed-box single map. The
  smoke PPM is the correct size (≈ 2,764,816 bytes) and the launcher exits 0.
- The `static_assert` holds (`ViewConstantsBlock` ≤ its stride); the `ShadowConstants` ring
  stride equals `align_up(sizeof, minUniformBufferOffsetAlignment)`, runtime-checked against the
  device limit.
- **The write→sample barrier survives the binding move.** The lighting pass's `.Sample(shadow)`
  (not its descriptor binding) is what the graph derives the `DepthAttachment → ShaderReadOnly`
  transition from, so moving the *binding* to set 1 leaves the barrier intact — confirmed against
  the compiled graph's derived transition, with the `VE_DEBUG` sync layer raising nothing on the
  depth-as-texture read.
- The `DebugView::Shadows` arm and its existing `gpu` assertion stay green at this plan's close
  (the repointed bound-view blit, not a broken bindless read).
