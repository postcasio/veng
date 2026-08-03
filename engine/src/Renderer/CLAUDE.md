# The renderer — RenderGraph, SceneRenderer, Viewport, bindless

The public renderer API lives under `engine/include/Veng/Renderer/` and its Vulkan backend under
`engine/src/Renderer/Backend/` (the public class lives in `Veng/Renderer/X.h`; its impl in
`src/Renderer/Backend/X.cpp`). Project-wide conventions — error policy, the Native idiom, resource
ownership — live in [the root CLAUDE.md](../../../CLAUDE.md); the runtime overview and the
`Application` drive in [engine/CLAUDE.md](../../CLAUDE.md); the scene/ECS layer in
[../Scene/CLAUDE.md](../Scene/CLAUDE.md); materials and shaders in
[../Asset/CLAUDE.md](../Asset/CLAUDE.md).

## RenderGraph: barriers fall out of declared use

Don't hand-write layout transitions/barriers. Declare a pass with the resources it writes
(`.Color(...)`) and reads (`.Sample(...)`); the graph derives the layout transitions and drives
`BeginRendering`/`EndRendering`.

Passes name **logical resources**, addressed by a vk-free `ResourceId`, not a concrete
`Ref<ImageView>`:

- **`CreateTransient({.Format, .Extent, .Usage})`** declares a graph-owned transient — the graph
  allocates its `Image`/`ImageView` at compile, resolves it per frame, and may alias
  non-overlapping transients onto shared backing.
- **`Import(name)`** declares an external resource (the swapchain image, an app-owned target). The
  graph never allocates or aliases it; its concrete view is supplied per frame as an
  `ImportBinding` passed to `Execute`.

A pass's `Execute` callback receives a **`PassContext`** — `Cmd()` for the command buffer and
`Resolved(ResourceId)` for a declared transient's concrete view this frame. A callback may not
capture a transient's view (an aliased transient has no fixed backing); it resolves through the
context at record time.

`RenderGraph` is a **builder**: declaring passes records nothing. `Compile()` derives the
barrier/transition schedule, allocates transients, builds each graphics pass's `RenderingInfo`, and
runs one-time validation, returning a `Unique<CompiledGraph>`. `CompiledGraph::Execute(cmd,
imports)` replays that baked schedule per frame — only the per-pass callbacks run. A consumer
re-`Compile()`s only on a **structural** change (a pass added/removed, a transient's extent/format
changed); per-frame data never recompiles. See `BuildCompositeGraph` (compile) and
`CompositeToSwapChain` (replay) in the hello-triangle `main.cpp` for the pattern — a member
compiled graph held across frames, imports bound per frame, re-compiled on resize.

## SceneRenderer: the deferred über-pipeline

`SceneRenderer` is a long-lived, configurable render pipeline on top of `RenderGraph`: it owns an
offscreen target, renders a `Scene` from a `Camera` through an **internal compiled `RenderGraph`**
of reusable `ScenePass` units, and hands back a sampleable result. It is **`Unique`,
single-owner** (nothing holds a `Ref` to one); `Create(const SceneRendererInfo&)` is the factory.

### File layout — the shape a new battery lands in

The renderer is split along three conventions, and a new battery follows all of them:

- **A pass lives in its own file under `Passes/`.** Every `ScenePass` — the g-buffer, deferred
  lighting, translucent, picking, TAA, scene-color copy, the directional and punctual shadow
  passes, SSAO, the skybox, the sky/point-field/volume passes, the depth-of-field composite, the
  debug draw (and its companion
  billboard pick), and the debug blits — is a `.h/.cpp` pair in `src/Renderer/Passes/`. The
  renderer holds them in `m_Passes` and wires them in `Rebuild`; each pass owns its own sizing,
  declared reads/writes, and recording. `PostProcessScenePass` is the one split case: its class
  is declared in the public `Veng/Renderer/ScenePass.h`, so only its implementation
  (`Passes/PostProcessScenePass.cpp`) lives here.

  **`GatherPass` and `SwapChainCompositePass` sit outside this model by design.** Neither is a
  `ScenePass` subclass nor an `m_Passes` member — they are the public gather/composite tail
  consumed by `Application`, `ViewportCompositor`, and the tests, so their headers are public
  (`Veng/Renderer/`) and their implementations stay directly in `src/Renderer/`.
- **A battery's resources live on an owned internal subsystem.** Each cluster of images /
  pipelines / descriptor sets / bindless handles / per-frame work is a renderer-owned `Unique<>`
  object in `src/Renderer/` (forward-declared in `SceneRenderer.h`), on the `EnvironmentIbl`
  precedent: `ShadowSystem`, `BloomPyramid`, `AutoExposureMeter`, `TaaResolve`, `SsrChain`,
  `DofChain`,
  `RefractionGrab`, `GpuCullSystem`, `PickingSystem`, and `SkyResolver` (which itself owns the
  three sky radiance-cube helpers `EnvironmentIbl` / `AtmospherePrecompute` / `SkyCubemapBake`).
  A subsystem owns its full vertical slice — its `Create`/recreate path, its `Declare*`
  contribution, its per-frame work — and **releases its own bindless handles in its own
  destructor**, so `~SceneRenderer`'s hand-list holds only the spine handles.
- **A renderer-internal header lives in `src/Renderer/`, never `include/Veng/`.** `SkyResolver.h`,
  `DrawPlan.h`, `SkySourceKind.h`, `FrameTopology.h`, `DrawGather.h`, `DebugBlitPipelines.h`, and
  `SceneRendererIds.h` are private headers beside the sources that consume them. Two properties
  follow and are load-bearing: they sit **outside the `include_hygiene` sweep**, which compiles
  *public* headers only, so nothing about them touches the engine's API surface; and
  `veng_test_unit` **can include them directly**, because `engine/src` is on its include path —
  which is what makes a device-free renderer decision (`FrameTopology.h`, `DrawGather.h`)
  unit-testable with no ICD and no CMake plumbing beyond a source-list entry. A constant read on
  both sides of a file boundary goes in `SceneRendererIds.h` at namespace scope rather than being
  duplicated into two anonymous namespaces: internal linkage is what forces the choice, and two
  spellings of an `AssetId` or a format is a silent-divergence hazard that surfaces as a validation
  error or a subtly wrong image.

What **stays on the renderer** is the wiring and orchestration, not a battery — and the layout
separates *deciding* from *wiring*:

- **`Rebuild()` is the wiring hub, and that is now its whole job.** It reads top-to-bottom as the
  pipeline order: a straight run of `AddPass` calls whose conditions are fields of one
  already-resolved topology. **It is deliberately not split further.** Its body carries ~469
  renderer-state references across ~291 of its ~635 lines — every subsystem pointer, both
  pass-handle sets, every resource id — so lifting it yields either a context struct that is
  `SceneRenderer&` in disguise or a friend class, and buys nothing. Long, but honest. What would
  justify revisiting is a **second axis of variation** (a second render path, not merely more
  batteries), never line count.
- **The topology decision is a device-free pure function**, not a prologue.
  `ResolveFrameTopology(settings, sky) → FrameTopology` (`FrameTopology.{h,cpp}`) maps the topology
  settings plus the resolved sky to every pass-set decision the graph body reads — no context, no
  allocation, no I/O, a function of its arguments alone. The renderer holds the result as a single
  `Unique<FrameTopology> m_Topology` behind a namespace-scope forward declaration (the
  `DebugBlitPipelines` pattern in that same header), so a decision made in `Rebuild` and read three
  phases later in `Execute` / `BuildImportBindings` names *one* member instead of one of eight
  scattered flags. The two genuine side effects stay at the call site: the skylight notification,
  and the auto-exposure enable **edge**, which is measured against the *previous* topology and so
  cannot live inside a function of the current inputs. Because the resolve is pure, its rules are
  pinned by `tests/unit/frame_topology.cpp` in the `fast` band rather than by a golden image. A gate
  with more than two meaningful states is a **named enum field**, not a boolean pair:
  `DofStages { None, CocOnly, Full }` makes "composited without the stages wired" unrepresentable
  rather than merely unreachable.
- **The three per-frame field-active flags are not topology and stay loose members.**
  `m_PointFieldActive`, `m_ScenePointFieldActive`, and `m_VolumeFieldActive` are resolved from
  *scene content* each frame (`ResolvePointFields` / `ResolveVolumeFields`), not from settings — so
  folding them into `FrameTopology` would break exactly the purity the unit cases pin. If they ever
  want the same treatment it is as a separate per-frame *content* struct.
- **`PrepareDraws` is the cross-plan draw/cull/skinning coordinator**, and only that: the per-frame
  plan reset, the ring bases, the frustum descent, and the cull arm. The three gather phases — the
  static opaque slot layout and its grouping, the skinned slots and their palettes, the translucent
  draws and their sort — are free functions in `DrawGather.{h,cpp}`, beside the `DrawPlan.h` types
  they fill. They take one `DrawGatherInput` bundle by const reference and the genuinely mutable
  state (the plans, the palette-base map, the shared `DrawBudget`) as explicit by-reference
  parameters, so mutation is visible at the call site. Threading **one** budget through all three
  is what keeps the static opaque range contiguous from 0, which the GPU cull arrays index by; the
  retained cull arm asserts that invariant directly. The grouping loop both the static and skinned
  phases run is one pure `GroupContiguousSlots` over a span of slots, covered by
  `tests/unit/draw_grouping.cpp`.
- **The per-frame budget is a type, not a predicate the phases each re-test.** `DrawBudget`
  (`DrawBudget.h`, header-only and device-free) owns both cursors, both limits (`MaxCullCandidates`
  slots, `MaxSkinningMatricesPerFrame` palette matrices), and the per-phase drop counts; a phase
  calls `TryClaimSlot` — or, for a skinned draw, the single all-or-nothing `TryClaimSkinnedDraw`,
  which returns a `SkinnedClaim` rather than a bool because a palette failure skips the instance
  while a slot failure ends the phase — and reports what it abandoned through `RecordDropped`. The
  reason it is a type is that the same predicate open-coded at three sites is what let the three
  reactions diverge. **The policy is clamp, drop, count, log once per renderer**: a frame over
  budget lays out what fits and abandons the rest, `SceneRenderer::GetDrawBudgetStats()` reports
  the limit, the grants, and the per-phase drops, and the renderer warns once for its lifetime
  (the latch is the renderer's, so the budget stays I/O-free and unit-testable in the `fast` band
  — `tests/unit/draw_budget.cpp`). The static phase's count carries a caveat: it triages the
  skinned and translucent survivors as it lays out its own slots, so its overflow ends the triage
  too and `StaticDropped` covers every remaining candidate, including ones a later phase would
  have drawn.
- **Construction lives in `SceneRendererResources.cpp`** — the `Create` half of the lifetime split
  below, compiled as a **second translation unit of the same class**, not a new type. The six
  `Create*` members keep unchanged signatures and reference `m_Internal` nowhere, so the split needs
  no shared internal header and no context object.
- **`DebugBlitPipelines` is its own `.h/.cpp` pair** in `src/Renderer/`, de-nested from
  `SceneRenderer` to `Veng::Renderer::DebugBlitPipelines` so its header stands alone without pulling
  in `SceneRenderer.h`. It reads no renderer state and takes everything it needs as parameters; the
  renderer holds it as a `Unique<>` behind a forward declaration.
- **`Execute()` is the frame orchestrator**, decomposed into named phase helpers —
  `ResolveRenderScale`, `ApplyTransformInterpolation`, `ResolveScenePasses`, `BuildImportBindings`,
  `RecordFrameHistory` — around the inline resolve core.
- **The Create/Resize/Configure/Execute lifetime split** (below) and the accessor block stay on the
  class.

The renderer also owns the shared **spine** every battery reads: the output target, the g-buffer
(albedo/normal/ORM/depth + velocity/emissive), the HDR target, the LTC LUTs, the shared sampler, and
the previous-frame view state (packed into the set-0 view-constants block every frame). The public
types split across three headers — `SceneRendererSettings.h` (the topology/sizing knobs, the
`DebugView` vocabulary), `SceneView.h` (the per-frame input + `SceneRendererInfo`), and
`SceneRenderer.h` (the class) — with the last re-including the first two, so a settings panel
includes only what it needs. `SceneRenderer::Internal` (opaque, `.cpp`-private) holds the
compiled graph and the draw plans.

### The lifetime split

Its surface is a **lifetime split** keyed on how often each piece of state changes:

- `Create(info)` — once: allocate persistent resources (output, g-buffer, HDR targets; fullscreen
  pipelines), build + compile the graph. The six `Create*` members that do that allocating compile
  in **`SceneRendererResources.cpp`**, a second translation unit of the same class; `Resize` and
  `Configure` call back into them from `SceneRenderer.cpp` unchanged.
- `Resize(extent)` — recreate the **allocation**-sized images via the retire path, re-register
  them into bindless, rebuild + re-`Compile()`. The extent is the *allocation* the targets live
  in; the per-frame `SceneView::RenderScale` renders into a top-left
  `round(allocExtent · RenderScale)` **sub-rect** of that allocation (`GetValidExtent()`), and the
  terminal tonemap upscales the sub-rect to the full output — so a per-frame resolution change
  costs no `Resize`, only a smaller rendered region. Sizing the allocation is the slow knob; the
  sub-rect is the fast one (see the `Viewport` section's two-loop model).
- `Configure(settings)` — recreate affected resources, rebuild + re-`Compile()` the topology.
- `Execute(cmd, view)` — every frame: replay the graph against this frame's `SceneView`. **Never**
  reallocates or recompiles.
- `GetOutput()` — the sampleable `Ref<ImageView>` of the owned result. **Resize and Configure
  invalidate it** (the old image retires, a new one is created); a consumer caching a bindless
  `TextureHandle` or ImGui texture from it must re-fetch and re-register after those calls.

### The deferred pipeline and its batteries

`SceneRenderer` is a **physically-based deferred renderer**: a metallic-roughness five-target
g-buffer (albedo G0, world-normal G1, packed occlusion/roughness/metallic G2,
per-object velocity G3, HDR emissive G4, plus a sampled depth attachment) with **tangent-space
normal mapping**, a
fullscreen **Cook-Torrance** lighting pass evaluating GGX specular + Lambert diffuse over
**multiple typed lights** (directional / point / spot) and reconstructing world position from
depth, then tonemap to the output. The batteries hang off the g-buffer: **cascaded shadow maps**
for the directional light and a **shared punctual shadow atlas** for a bounded set of point/spot
lights, **SSAO** folded into the ambient/occlusion term, a **compute mip-pyramid bloom** ahead of
tonemap, and an optional **TAA** resolve (off by default) between lighting and tonemap. Each
battery is a `SceneRendererSettings` toggle driving the `Configure` recompile.

### TAA

**TAA is an HDR-space temporal resolve** (`Settings.TAA`, off by default). It jitters the
projection by a Halton(2, 3) sub-pixel offset each frame (`Renderer/TaaJitter.h`, a pure
device-free helper), routes the lighting pass into a separate **lit** target, and inserts a
**resolve** pass (lit + reprojected history + velocity/depth → the HDR target the bloom/tonemap
tail already samples) and a **history-copy** pass (HDR → the persisted history for next frame).
Motion is **per-object** and **folded into the g-buffer pass**: velocity is a fourth g-buffer
channel (**G3**, `RG16Sfloat`), written by the surface fragment as `SV_Target3` alongside
G0/G1/G2 — `curUV - prevUV` from the per-vertex current and previous clip positions, computed by
the shared `ComputeMotionVector` helper. So there is **no separate velocity prepass**: the one
geometry rasterization that fills the g-buffer also fills velocity. The previous position comes
from a per-draw `PrevWorld` matrix (`GpuDrawData` carries it; the renderer tracks each entity's
prior world in `m_PreviousWorlds`, keyed by packed `Entity` and swapped each frame) and the
unjittered `CurViewProj`/`PrevViewProj` (both in the set-0 view-constants block); the skinned
surface vertex stage additionally skins the previous position through the previous-frame palette
(`PrevPaletteBase`), so deformation motion writes velocity too. The resolve uses the velocity
vector for geometry (camera **and** object motion) and falls back to depth-based camera
reprojection for the cleared background. Because velocity is a g-buffer channel it is **always
written and always allocated** (the cost is one extra `RG16Sfloat` target plus a clip-position
write, not a second geometry pass); with TAA off it is written but unread, and the `MotionVectors`
debug arm blits it directly. The opaque material contract is therefore **G0/G1/G2/G3/G4** —
velocity is the fourth MRT channel of the surface output, not a separate pass, and emissive (G4)
is the fifth. History is **YCoCg
variance-clipped** to the 3×3 neighborhood, sampled with a **Catmull-Rom** filter, and blended
with **luminance weighting** (Karis anti-flicker); offscreen reprojection and the first frame
after a `Resize`/`Configure` fall back to the current color (`m_TaaHistoryReset`). The history is
a renderer-owned persisted image written and read within the renderer's own single-queue graph
each frame, so it needs no cross-frame ring or semaphore.

### Shadows: directional cascades + the punctual atlas

The directional light is shadowed by **cascaded shadow maps**, and a **bounded set of punctual
lights** (`MaxShadowedPunctual`) by a **shared punctual shadow atlas**. The directional cascades
split the camera frustum into depth slices, each cascade fit (bounding-sphere + texel-snapped) to
its slice and rendered into a depth **atlas** in **one** pass (per-cascade viewports); the
lighting pass selects the cascade by the fragment's view-space depth, remaps to the atlas tile,
and **`SampleCmp`s** it through a **hardware comparison sampler** with a boundary cross-fade.
Cascade fit is pure, device-free math (`Renderer::ComputeCascades`,
`Veng/Renderer/ShadowCascades.h`) over the camera, light direction, and the world-space scene
bound (the bound only extends each cascade's near plane toward the light to catch off-screen
casters; the XY extent is the frustum slice). The punctual lights add the second arm: a **spot**
renders one perspective shadow map through a single frustum, a **point** renders six 90° cube
faces, both into the shared punctual atlas (a 2D atlas of `MaxShadowedPunctual·CubeFaceCount`
tiles); the lighting pass samples each shadowed light's map (projective for a spot, cube-direction
for a point) with the **same** hardware `SampleCmp` + PCF and multiplies the visibility into that
light's contribution. The punctual view math is pure, device-free glm
(`Renderer::ComputeSpotShadowView` / `ComputePointShadowView`, `Veng/Renderer/PunctualShadows.h`)
beside `ShadowCascades.h`. Each shadow view culls its casters through `SceneBroadphase::Cull`
against **its own** frustum — the camera frustum for the g-buffer, each cascade's light frustum,
each spot's frustum, each cube face's frustum.

**Set 1 is a general shadow system**, not just the directional one: the directional cascade atlas,
the punctual shadow atlas, a **shared** immutable comparison sampler (hardware `SampleCmp`), the
per-frame `ShadowConstants` block (the directional cascade matrices + splits + params) bound as a
**dynamic uniform**, and the `PunctualShadowBlock` (the per-light shadow records — view-proj(s),
tile rects, type) ringed beside it. All of set 1 is held **off the set-0 bindless registry**,
where a comparison sampler mistranslates inside the Metal argument buffer on MoltenVK and a closed
producer→consumer resource needs no global registration. The set-0 view-constants block stays
trimmed to material-facing camera/view state. A `GpuLight`'s shadow **slot** (an index into the
punctual record array, or `-1` for unshadowed) rides the `Cone.zw` padding, keeping `LightStride`
fixed. `CascadeCount`, `CascadeSplitLambda`, and `ShadowResolution` (default 1024) are the
directional CSM knobs; `PunctualShadows` (the on/off toggle) and `PunctualShadowResolution` (the
per-tile edge length) are the punctual knobs; `DebugView::Cascades` tints each fragment by the
cascade it selects and `DebugView::PunctualShadows` blits the punctual atlas. **A Translucent submesh casts no shadow.** Both shadow passes gate each candidate on
`Renderer::CastsShadow` (`src/Renderer/DrawGather.h`): a resident material that is not
`MaterialDomain::Translucent`. The domain writes no opaque depth, is drawn after the lighting it
would have to occlude, and is documented as never occluding another translucent, so rasterizing a
solid shadow from it contradicts every other way it behaves. Alpha-cut and stained-glass casters are
a separate capability — both need the shadow pass to *shade* rather than to rasterize depth. This
per-light shadow cull is the **prime consumer of the BVH broadphase** — one tree queried many times (`N`
spot frustums + `6N` cube faces per frame, on top of the camera and cascade queries).

### Bloom

**Bloom is a compute mip-pyramid battery**, a fixed engine pass like SSAO and the shadow atlas —
not a PostProcess material. The lit HDR target is bright-passed (a soft-knee `Threshold` with
**Karis-average** firefly suppression on the first downsample, the firefly-stability mechanism
that holds whether or not the optional TAA resolve is on) into a single `HdrFormat` mip-chain
image's mip 0, **progressively downsampled** through the chain, then **upsampled** with an
accumulating dual filter (`mip[i] += upsample(mip[i+1]) * Radius`) and **composited** back into
linear HDR (`hdr + mip0 * Intensity`) ahead of tonemap. The whole sweep is **compute**: per-level
dispatches with a barrier between levels, mirroring the hi-Z reduction's mip-chain shape (one
image with N mip levels, per-mip storage views for the writes, a whole-chain sampled view + a
clamp-to-edge linear sampler for the bilinear taps, per-level descriptor sets, all off bindless).
The filter kernel is a `BloomKernel { Cod, Kawase }` topology knob — the COD/Jimenez 13-tap-down /
tent-up dual filter (the default and the golden's kernel) or the bandwidth-optimized **Dual
Kawase** filter for the TBDR GPUs veng primarily targets. `Bloom` (on/off) and `Kernel` are
`SceneRendererSettings` topology knobs (a `Configure` recompile); `Threshold` / `Intensity` /
`Radius` are per-frame `SceneView` values that ride the compute push, so tuning them never
recompiles. `DebugView::Bloom` blits pyramid mip 0 after the up-sweep — the accumulated bloom
contribution before composite.

### Depth of field and the physical camera

**Depth of field is a half-resolution ring-gather compute battery** (`DofChain`,
`Settings.DepthOfField`, **off by default**) whose parameters come from an authored camera rather
than bare blur knobs. It follows the `SsrChain` template exactly: an owned subsystem, compute
stages, one fullscreen composite (`DofCompositeScenePass`, `Passes/DofCompositeScenePass.h`)
splicing itself in by re-routing the downstream source id, and a `DebugView` arm that force-wires
the chain independently of the feature toggle. The composite sits **ahead of bloom**, so a
defocused highlight still blooms.

The chain is five stages. **CoC + prefilter** reconstructs view-space depth, evaluates the
thin-lens circle of confusion, and splits scene color into a **near** and a **far** half-resolution
layer with each layer's own radius in alpha, plus a single-tap signed-radius/depth buffer.
**Tile dilation** reduces each 8×8 tile *and its eight neighbours* into one record (minimum depth,
largest near and far radii) — the dilation is what lets a gather see the near-field spill its
neighbours advertise, and it bounds each gather's kernel so an in-focus tile costs one tap.
**Ring gather** runs once per layer over concentric rings (`8·r` samples on ring `r`, each ring
rotated half a step so successive rings interleave rather than lining up on spokes), weighting a
sample by the scatter test read backwards, `saturate(sampleCoc − distance + 1)`. The far layer
additionally clamps each sample's radius to the destination's, so background blur cannot bleed over
sharp foreground; the near layer is deliberately unclamped, which is how a defocused foreground
spills over sharp geometry behind it. **Fill** is a 3×3 center-weighted tent closing the
single-texel gaps a fixed sample budget leaves at a large radius. **Composite** blends far then
near over the full-resolution HDR by each layer's coverage, so a zero-coverage texel is the HDR
value it was.

**The gate carries its own debug arm.** `dofActive` is
`(Mode == Final && DepthOfField) || Mode == DebugView::CoC` — the SSR gate shape, where the
disjunct is what lets `DebugView::CoC` force-wire the chain's first two stages **with the feature
off** and blit the signed-radius buffer (near ramps red, far ramps blue, in-focus is black),
normalized against the frame's own clamped `DofMaxCoc` so brightness reads as the fraction of the
configured budget a texel uses and a shallow budget still fills the ramp.

#### The physical camera drives it — and the units are the trap

`CameraProjection::Physical` sits beside `Perspective`/`Orthographic` on the `Camera` component
(`Veng/Scene/Camera.h`), so authored `FovY` content is untouched and there is no
derivation-precedence ambiguity. Its four authored fields and **their units**:

| Field | Unit | Default |
|---|---|---|
| `FocalLength` | **millimetres** | 50 |
| `SensorHeight` | **millimetres** | 24 |
| `FStop` | f-number (dimensionless); aperture diameter is `FocalLength / FStop` | 2.8 |
| `FocusDistance` | **metres** | 10 |

**The millimetre/metre mix is the whole footgun** — a silent 1000× error hides exactly here — so
the engine has **one conversion site**: `ComputeCameraLens` normalizes the two millimetre fields
into a metres-only `CameraLens`, and *everything downstream is metres*. Never convert anywhere
else. A `Physical` camera resolves as a perspective projection whose vertical field of view is
`2·atan(SensorHeight / (2·FocalLength))` — a **ratio**, so the authored millimetres cancel and
that expression alone is unit-safe — and the resolved `CameraView` carries the `CameraLens`, read
back through `CameraView::GetLens()`. `GetLens()` is engaged **iff** the camera was `Physical`, so
a consumer reads it as "was this view authored in physical terms". `ComputeDofParams` adds the one
thing a lens does not know — the viewport's pixel height — yielding `CocScale`
(pixels per metre, `viewportPixelHeight / SensorHeight`); `ComputeCircleOfConfusion` is the curve
those constants define, `CocScale · Aperture · (depth − FocusDistance) / depth`, signed so the near
field is negative. All of it is device-free inline math in the public header, unit-testable with no
ICD.

**With a `Physical` camera, the camera wins — and the level's focus fields go inactive.** The five
per-frame `ViewState`/`SceneView` fields are `DofFocusDistance`, `DofAperture`, `DofCocScale`,
`DofMaxCoc`, and `DofRingCount`. `ApplyLevelRenderSettings` stays a **pure, unconditional
mapping** — it records authored intent into the persistent knobs and never inspects the camera —
while the viewport glue fills the lens-derived fields in the per-frame copy it pushes. So
camera-wins holds **by construction every frame**, the stored authored values survive untouched,
and they come back to life the moment the camera stops being `Physical`. `DofCocScale` is
*always* derived by the glue (sensor height × viewport pixel height) and is never hand-authored in
any mode. `ViewState::DofFromPhysicalCamera` is the flag the settings panel and level editor read
to show `DofFocusDistance`/`DofAperture` **inactive**, so an author is not editing values nothing
consults.

**`DofMaxCoc` and `DofRingCount` still apply in every camera mode** — they are quality knobs, not
lens properties, and a physical camera does not drive them. Both are **hard-clamped where they are
pushed** (`ClampDofMaxCoc` → `DofCocCeiling`, `ClampDofRingCount` → `MaxDofRings`, `Renderer/DofTile.h`)
because `LevelRenderSettings` routes authored values in from a cooked level and **an archive is
untrusted input**; the ring count is a GPU loop bound, and the gather shader ceilings it a second
time against a compile-time `MaxRings` so no missed CPU clamp can ever produce an unbounded loop.

#### Translucency defocuses by the geometry behind it

The translucent composite sits **upstream** of the DoF chain, so translucent draws are already
blended into the color the chain blurs — but they write **no opaque depth**, and the CoC is
evaluated from the depth attachment alone. A translucent pixel therefore defocuses by the circle of
confusion of the **opaque geometry behind it**, not its own distance: glass at the focus plane in
front of a distant background blurs with that background. This is the standard limitation of
gather-based depth of field on a deferred pipeline, and the engine accepts it.

### IBL and the sky

**Image-based lighting is a split-sum IBL battery driven by a per-scene environment map.** A
resident `AssetHandle<EnvironmentMap>` rides the per-frame `SceneView` (set by the app through the
`Viewport`'s `ViewState`, like `Exposure`); a renderer-owned **`EnvironmentIbl`** helper
(`engine/src/Renderer/EnvironmentIbl.{h,cpp}`) generates the maps it derives — a **radiance
cubemap** (the skybox source), a **diffuse irradiance cubemap**, a **GGX-prefiltered specular
cubemap** (roughness mip chain), and the environment-independent **BRDF integration LUT** — all
through compute (the four `ibl_*.comp` core shaders), mirroring the bloom/hi-Z
compute-with-manual-barriers pattern (per-face/per-mip storage views, cube sampled views, explicit
`PrepareForAccess` barriers, all off bindless). Generation is recorded **once when the bound
environment changes** (a `m_LastEnvironment` gate in `Execute`, into the same command buffer
before the graph runs); the BRDF LUT is generated once on first use. The four sampled maps + a
linear sampler reach the deferred lighting pass as **one dedicated descriptor set bound at set
2** — **off the set-0 bindless registry**, mirroring the shadow-atlas "closed producer→consumer"
precedent (a cubemap in a Metal argument buffer is a MoltenVK risk, and a closed resource needs no
global registration). The lighting fragment replaces its flat hemispheric ambient with
`kD · irradiance · albedo` diffuse + `prefiltered · (F · brdf.x + brdf.y)` specular when an
environment is bound. IBL is a **runtime push flag** (`IblEnabled`), not a pipeline variant: the
set is always bound and valid (the maps are transitioned to a sampled layout at first `Execute`
even before an environment arrives), so a scene **without** a lighting sky falls back to the exact
flat-ambient path and renders unchanged.

**The sky is one component, and every sky source is a radiance-cube producer.** The scene carries
one author-opt-in **`Sky` component** (`Veng/Scene/Components.h`): a **source** (`SkySource`
variant — `EnvironmentSky` an environment map, `AtmosphereSky` the procedural atmosphere,
`MaterialSky` an authored Sky-domain material), an `Intensity`, and a **lighting tier**
(`SkyLighting` — `None` display-only, `SH` a spherical-harmonic diffuse ambient, `IBL` the full
split-sum). The renderer **resolves this component itself each `Execute`** (`ResolveSky`,
`TryGetFirst<Sky>` — the lights model, no consumer mapping call or topology toggle) and recompiles
its own pass set at the frame boundary when the resolved source-kind / tier / bake-mode changes.
Every source produces the **same radiance cube** the skybox samples and the IBL convolution reads,
so **what you see and what lights the scene agree by construction**: an environment is a cube
(equirect→cube), a `MaterialSky` or `AtmosphereSky` in `SkyMode::Baked` bakes to a cube
(`SkyCubemapBake`, six fullscreen face renders over a fixed per-face basis + a 1×1 far-plane
stand-in depth, re-baked on the source's dirty signal), and both display through the one
**`SkyboxScenePass`** (a fullscreen pass compositing the cube over the cleared-depth background,
`discard`ing foreground, writing the same scene-color target lighting wrote so the sky resolves,
reflects, and tonemaps with the scene). `SkyMode::Direct` keeps the per-pixel passes as the
authored **dynamic** modes — `SkyScenePass` (procedural atmosphere) and `SkyMaterialScenePass`
(authored material) — for a continuously-animating sky; a direct source **cannot light** (it has
no cube), so a direct source with a lighting tier degrades to background-only with a one-time
warning (bake to light). Both lighting tiers read the resolved source's one cube: `SH` projects it
to the irradiance SH the lighting pass folds into its ambient arm
(`EnvironmentIbl::ProjectCubeToIrradianceSh`, a device-free readback), `IBL` convolves it into the
split-sum maps (`EnvironmentIbl::GenerateFromCube`) — one cube→SH / cube→IBL path for every
source, no per-source special case. `SceneView::EnvironmentIntensity`/`AtmosphereIntensity` are
per-frame push values (no recompile). (The cooked `EnvironmentMap` asset — the
radiance/irradiance/prefiltered/BRDF maps — is `AssetTypes::Environment`; the `EnvironmentSky`
source is the scene-authoring front-end that references it.)

### A sky reconstructs its ray without the camera's translation

`SkyViewDirection` (`Veng/sky.slang`) reads **`InvViewRotProj`** — the inverse of
`Proj x the rotation-only View` — rather than `InvViewProj`. A sky wants the *ray* through a pixel,
which is a property of where the camera looks and not of where it is; deriving it from
`InvViewProj` means forming a far-plane world point and subtracting the camera off it, two large
nearly-equal numbers whose small difference is the answer. That cancellation costs f32 precision in
proportion to the camera's distance from the world origin, and costs it **unevenly across the
frame** — worst along the rays whose far point lands furthest out — so a star field shimmers, and
shimmers worse looking away from the origin than toward it.

Measured on a consumer at one solar radius per scene unit: at 3,400 units from the origin a
stationary view's sky differed by 1.7/255 frame to frame with 2.3% of pixels moving more than 16
levels; through the rotation-only inverse it is pixel-identical, and stays so at 80,000 units. The
matrix is jittered with `Proj`, so it still agrees with what was rasterized. `SkyCubemapBake` writes
its face basis into both fields — that basis is already a pure direction mapping with no translation
in it.

### View constants: the ring-buffered set-0 block

Per-view data rides a **ring-buffered view-constants buffer**, not push constants: the
`InvViewProj`/`CameraPosition` (for world-position reconstruction), the view/projection, and the
SSAO view/projection live in a set-0 buffer selected by an index fold (a dynamic-offset descriptor
mistranslates in set 0 on MoltenVK). This buffer is **shared across every `Viewport`** (it lives
in the `Context`-owned `BindlessRegistry`), so it is ringed `framesInFlight * MaxViewsPerFrame`
deep and each `SceneRenderer::Execute` claims its own slot (`BindlessRegistry::TryBeginView`, reset
per frame): two viewports rendering in one frame write distinct regions rather than the second's
camera clobbering the region the first's draws still read at submit. The shared per-frame light
buffer rings the same way.

**`MaxViewsPerFrame` (32) is a budget spent against, not a contract.** Its consumers are the
registered viewports (one slot each), one face per driven scene capture, and a sky cube bake (six
slots, twelve on the SH tier whose readback bake claims its own six) in the frame a sky is dirty —
so ordinary content can want more than one frame holds, and the ceiling is sized by memory the whole
ring pays (`framesInFlight * MaxViewsPerFrame` regions of ~6 KB). `TryBeginView` therefore **returns
false rather than asserting** when the budget is spent, warning once, and each consumer degrades:
`SceneRenderer::Execute` records nothing and its target keeps the last frame's content, a sky bake is
skipped **whole** and left dirty for the next frame with room (a half-filled cube marked clean would
be permanently wrong), and `ViewportCompositor::RenderRegistered` reserves one slot per registered
viewport before driving the captures at all — so **captures give way before viewports do**, a missing
reflection over a stale window. An over-budget capture set is driven **round-robin** across frames
from a retained cursor (`CaptureRotation.h`, the device-free arithmetic), which costs each map refresh
latency instead of starving whichever captures registered last. Its stride is **640 bytes**. The shadow system's own state — the
cascade matrices, splits, and params — rides the **set-1** `ShadowConstants` block instead, so set
0 stays a lean, material-facing view block (shared by materials, lighting, and SSAO). Push
constants in the deferred path carry only small per-invocation bindless handle indices and the
live light count; the typed lights ride a separate ring-buffered light buffer the lighting pass
loops over.

### SceneView: the per-frame view

The per-frame `SceneView` carries everything a pass needs for one frame: the world, the resolved
camera, the delta / interpolation alpha, the render scale and extent, the live light count (the
typed lights themselves ride the ring-buffered light buffer, up to `MaxLights`), and the exposure
and per-frame tuning knobs (the bloom `Threshold` / `Intensity` / `Radius`, the environment /
atmosphere intensities). It reaches pass callbacks through an **opaque `void* userData`** channel
on `RenderGraph::PassContext` / `CompiledGraph::Execute` — so `RenderGraph` stays scene-agnostic.
A `ScenePass` reads it back through a typed `ScenePassContext` (`Cmd()` / `View()` /
`Resolved(id)`); `View()` asserts the pointer is non-null before the reinterpret, and
`SceneRenderer` sets it on every `Execute`. Because these are per-frame values, tuning them never
recompiles.

### Culling and the BVH broadphase

The scene-drawing passes **cull at submesh granularity through a BVH broadphase**. A
renderer-owned `SceneBroadphase` (`Veng/Scene/SceneBroadphase.h`) holds a bounding volume
hierarchy whose **leaves are per-submesh** — one leaf per `SubMesh`, on its local-space `AABB`
folded over the submesh's index range at load (no cooked-format change). Each `Execute` calls
`SceneBroadphase::Sync`: it re-gathers the candidates (the pure `GatherMeshes` pass,
`Veng/Scene/Visibility.h`, over every resident `(Transform, MeshRenderer)` entity — world matrix +
world-space `AABB` + resident mesh) and rebuilds the tree **only on a frame the scene's spatial
version moved** (or a still-loading mesh became resident) — a static scene rebuilds the tree not
at all and queries a stable one. The gathered list rides `std::span<const VisibleMesh> Visible` on
`SceneView`; `SceneBroadphase::Cull` descends the tree once per view — the g-buffer geometry pass
with the **camera** frustum, the cascaded shadow pass once per cascade with **each cascade's**
light frustum, each punctual light's view with its own — so the many-view shadow workload queries
**one tree, many times** rather than re-scanning the list per view. A query returns exactly the
linear scan's per-submesh survivor set (a node wholly outside a frustum rejects its subtree; a
leaf is accepted on its tight box), so the cull is conservative (an extra draw, never a dropped
visible submesh) and the rendered image is **byte-identical** — only the draw calls issued differ.

`SceneRendererSettings::Cull` selects how those survivors are submitted. Under **`CullMode::CPU`**
(the default) the renderer records a direct per-submesh draw for each camera-frustum survivor.
Under **`CullMode::GPU`** the same frustum survivors are uploaded to a GPU buffer, a **compute**
pass runs a **hi-Z occlusion test** over each candidate's screen-space AABB against the
**previous-frame depth pyramid** and writes each `VkDrawIndexedIndirectCommand`'s `instanceCount`
(1 for a survivor, 0 for an occluded candidate, which executes as a no-op), and the geometry pass
issues the whole fixed buffer through a single `vkCmdDrawIndexedIndirect` per mesh group. The
compute does **not** re-run frustum culling — the BVH already did; it adds only occlusion. The
hi-Z pyramid is a **max-Z mip chain** reduced from the depth target by compute into a
renderer-owned, cross-frame-persisted resource (temporal hi-Z: the test reads last frame's chain,
so a history-invalid frame — frame 0, the frame after a `Resize`/`Configure`, or a large view
delta — is frustum-only, never a stale false-cull). `SceneRendererSettings::Occlusion` gates the
occlusion test within the GPU path; with it off the GPU path issues every camera-frustum survivor.
The submission shape is the **`drawIndirectCount`-free** form MoltenVK supports
(`multiDrawIndirect` + `drawIndirectFirstInstance`, the candidate id carried in each command's
`firstInstance` and read as an instance-rate vertex attribute); both modes drive the **same
buffer-indexed surface shader**, differing only in submission. `CullMode::GPU` is gated on
`Context::IsGpuDrivenCullingSupported()`: on a device lacking either feature the renderer logs
once and falls back to `CullMode::CPU`, and `GetActiveCullMode()` reports the real mode.

`SceneRendererSettings::FrustumCull` (default on) toggles the frustum cull itself; the cull funnel
is reported by `GetLastVisibleCount()` (gathered submesh candidates) →
`GetFrustumSurvivedCount()` (frustum survivors) → `GetLastDrawnCount()` (equal to the frustum
survivors on the CPU path — it counts survivors, not slots laid out) → `GetDrawBudgetStats()` (the
slot limit, the slots the three gather phases granted, and the submeshes each dropped once a
budget was exhausted — the stage that makes a clamped frame legible, since the survivor counts
above it do not move when the clamp fires), with `GetLastGpuSurvivorCount()` the GPU occlusion
survivor count read back one frame late under `CullMode::GPU`, and `DidBroadphaseRebuildLastFrame()` /
`GetBroadphaseNodeCount()` reporting whether the tree rebuilt and its size. Tree maintenance is
rebuild-on-version-move, not incremental; culling granularity is per-submesh, not meshlet;
occlusion is temporal hi-Z, not two-pass; shadow views cull on the CPU BVH only.

### ScenePass and PassIO

A `ScenePass` is a reusable, self-contained pipeline stage (`Configure` / `Resize` /
`Declare(RenderGraph&, const PassIO&)`) that **contributes** one or more `RenderGraph` passes into
the renderer's single internal graph — it is not a `RenderGraph::Pass`. The renderer owns the
**wiring** (which pass reads whose target, via the named-slot `PassIO`); each pass owns **itself**
(sizing, declared reads/writes, recording). It knows only how to record, never what feeds it.

The renderer's pipeline images (g-buffer albedo / world-normal / ORM / velocity / emissive, depth,
HDR, the bloom mip pyramid + composite result, output) are **renderer-owned `Image`/`ImageView`s
`Import`ed** into
the internal graph — not graph transients — because a fullscreen pass samples an upstream target
through the bindless set-0 array, which needs a `Ref<ImageView>` to `Register` (a transient
exposes only a per-frame `ImageView&`). They are registered into bindless once at `Create`
(re-registered on `Resize`) and reach the sampling pass as `TextureHandle`s through `PassIO`. The
one exception is the **shadow atlas**: a closed producer→consumer resource (the shadow pass writes
it, the lighting pass and the `DebugView` blit read it, nothing else) reaches its consumer through
a **dedicated descriptor set** via a `PassIO` **bound-view** slot — off bindless — because the
lighting pass uses a comparison sampler / `SampleCmp`, which a set-0 bindless argument buffer bars
on MoltenVK, and a closed resource needs no global registration. It is still an `Import`ed,
graph-declared resource (the lighting pass's `.Sample` drives the graph-derived
`DepthAttachment → ShaderReadOnly` barrier); only its *binding* sits off bindless.

The über-pipeline is **batteries-included, not extensible**: a bespoke pass graph still means
dropping to `RenderGraph` directly (the composite path the sample retains).
`SceneRendererSettings` carries the topology/sizing knobs — `DebugView Mode` (Final, plus the
`Albedo` / `Normal` / `Depth` / `Emissive` g-buffer arms, the `Roughness` / `Metallic` / `Occlusion`
packed-ORM-channel arms, and the `AO` / `Shadows` / `Cascades` / `PunctualShadows` / `Bloom` /
`MotionVectors` / `CoC` battery-target arms) re-wires the pass set through `Configure`, the
recompile
seam; the `Bloom` / `Shadows` / `PunctualShadows` / `AO` / `DepthOfField` battery toggles, the
bloom `Kernel`, and
`ShadowResolution` / `PunctualShadowResolution` are the other recompile knobs. A debug arm
terminates the chain after the g-buffer (and, for `AO` / `Shadows` / `PunctualShadows`, the
force-wired producing battery pass) with a single fullscreen debug blit; the `Bloom` arm
additionally runs the lighting pass and the force-wired bloom sweep and blits pyramid mip 0 after
the up-sweep, the `MotionVectors` arm blits the per-object velocity g-buffer channel (written
by the surface pass every frame) colorized as an optical-flow field (hue = direction, brightness =
magnitude), and the `Emissive` arm blits the G4 emissive channel directly — the authored emissive
contribution alone, independent of lighting. Per-frame values (`Exposure`, the bloom `Threshold` / `Intensity` / `Radius`, the
camera, the lights) ride `SceneView`, so tuning them never recompiles.

### Single-copy targets and the frames-in-flight contract

The renderer-owned images are **single-copy**: one `Execute` resolves and completes before the
next begins, written-then-read images within a frame are ordered by the graph's derived barriers,
and the retire path covers destruction safety on `Resize`/`Configure`. The output is consumed in
the frame it is written — a compositor samples `GetOutput()` for the same frame the renderer wrote
it.

**Frames-in-flight contract.** The output stays single-copy across frames-in-flight. A consumer
transitions it for its read (`PrepareForAccess(Sample)`) and the next frame's scene render
transitions it back (`PrepareForAccess(ColorAttachment)`, recorded by the renderer before each
`Execute`), bracketing a cross-graph handoff no single graph can derive a barrier for. This
barrier suffices without a semaphore or a ring because both halves record on the single graphics
queue in submission order, so the barrier's first synchronization scope reaches the prior frame's
read; the internal targets (g-buffer, depth, HDR) are single-copy and serialized by the renderer's
own graph. The output is single-copy and unringed — the contract holds exactly because both halves
record on the single graphics queue. The TAA resolve needs neither a ring nor a semaphore: its
history is a renderer-owned persisted image written and read inside the renderer's own
single-queue graph each frame, ordered by the graph's derived barriers.

### The deferred opaque material g-buffer contract

An opaque (Surface-domain) material's **fragment shader outputs** are **g-buffer channels**, not
final swapchain color, written through a single engine-provided `GBufferOutput` struct
(`float4 Albedo : SV_Target0; float4 Normal : SV_Target1; float4 ORM : SV_Target2;
float2 Velocity : SV_Target3; float3 Emissive : SV_Target4;`). Albedo (G0) is sRGB-encoded
(sampled back as linear); the normal (G1) is the tangent-space-perturbed world normal in a signed
float format; ORM (G2) packs occlusion (R), roughness (G), and metallic (B) — the
metallic-roughness PBR channel set — with **the alpha unused and available** (the lighting pass
reads only `orm.rgb`); velocity (G3, `RG16Sfloat`) is the per-object screen-space motion vector
(`curUV - prevUV`) the TAA resolve reprojects through, written by the shared `ComputeMotionVector`
helper from the vertex stage's unjittered current/previous clip positions; emissive (G4,
`B10G11R11Ufloat`) is the **linear HDR radiance** the surface fragment authors per pixel —
procedural, textured, animated on the frame clock, whatever the material writes — which the
lighting pass **adds into the outgoing light before the sky composite** (geometry pixels are
foreground, so the skybox composite that discards foreground leaves the emissive term intact, and
the pre-translucent refraction grab captures it unchanged). Depth is the depth attachment, also
sampled by the lighting pass for world-position reconstruction (one of the depth targets read as
textures in the engine — the directional cascade atlas and the punctual shadow atlas are the
others). The g-buffer layout (channels, formats, usage) is fixed in `Renderer/GBuffer.h`, agreed
on by the geometry pass's `RenderingInfo` and every material pipeline. It is the **opaque**
contract — a transparent/forward material outputs final color through a separate fragment entry,
not a change to this one.

The **5-MRT opaque contract is unconditional.** Folding velocity and emission into the surface
output means motion vectors and per-pixel emission each cost **no second geometry pass** — the
single g-buffer rasterization fills them — but every opaque pixel pays the G4 attachment
(allocation, clear, write, and on a TBDR GPU its tile-memory footprint + store bandwidth) whether
or not the scene authors any emission, exactly the always-on shape the velocity target already
has. There is **no per-scene opt-out** — no setting inserts or removes the channel — so a
bandwidth-constrained consumer must count G4 as a fixed tax it cannot drop. Set-0 bindless, the
material parameter block, and texture handles work identically for an opaque material; only the
fragment shader's outputs are g-buffer channels.

### The PostProcess fullscreen-material path

A `PostProcessScenePass` runs a PostProcess material as a fullscreen effect: it builds a
`GraphicsPipeline` from the material's fragment shader against a renderer-supplied color format
(fullscreen triangle, one color target, no vertex inputs), binds set-0 bindless, runtime-binds an
upstream target as a material handle field (`Material::SetTextureHandle`/`SetSamplerHandle`, no
resident asset), and drives the material's authored params. The loader builds the pipeline
*layout* for both domains but the `GraphicsPipeline` only for Surface — a PostProcess material's
pipeline is built by the pass, which alone knows the color format. **Tonemap is the PostProcess
material** (core `tonemap.vmat`): the HDR (or bloom-composite) target is runtime-bound each frame
and the per-frame `Exposure` from `SceneView` is written into the ring-buffered block each
`Execute`. The fixed plumbing composites stay hardcoded engine passes — `SwapChainCompositePass`
(scene behind, ImGui over) and the `DebugView` blits (albedo/normal/depth, the packed-ORM
channels, the emissive channel, the SSAO target, the bloom pyramid, the directional and punctual
shadow maps) have no
authorable surface; a PostProcess material is for *tunable effects with exposed parameters*, not
plumbing.

### Point fields

**The point-field draw pipeline batches submission and runs per-point work once.** A `PointField`
(`Veng/Renderer/PointField.h`) is a large, GPU-resident set of positioned, colored, sized points
with a screen-density LOD; `PointFieldScenePass` accumulates every scene field into the linear HDR
scene color ahead of bloom/tonemap. Per field it CPU-frustum-culls the field's spatial cells
against the camera, then per surviving cell routes to one of two draws by on-screen point density:
individual camera-facing sprites (the resolved LOD) below the `PointFieldLod::AggregateThreshold`,
or one additive density splat (the aggregate LOD) above it — the two paths deliver the same
integrated light, so the LOD transition holds brightness.

- **The pass reports a per-frame cull/draw funnel.** `SceneRenderer::GetPointFieldStats()` returns
  a `PointFieldStats` block — fields walked, cells total / in-frustum / measured, resolved sprite
  draws issued, sprite points submitted, points the compute pass compacted out, the draw source,
  and aggregate splats drawn — summed across every field. It sits beside the mesh cull-funnel
  getters (`GetLastVisibleCount` / `GetFrustumSurvivedCount` / `GetLastDrawnCount` /
  `GetDrawBudgetStats`): a consumer
  profiling a heavy field reads the sprite/splat split here instead of GPU timestamps.
  `CellsMeasured` is tracked apart from `CellsInFrustum` (a fixed-outcome threshold skips the
  density measure), and `ResolvedDraws` apart from `SpritePoints` (the run-merge collapses draws
  without changing the point total).
- **Submission batches by buffer contiguity.** A cell's points are a contiguous run of the
  resident buffer, and `Bucket` tiles cells in ascending `FirstPoint` order, so the walk merges
  adjacent resolved cells into `{FirstPoint, PointCount}` draw runs: a run extends while the range
  continues and breaks only where a cell was culled or aggregated. A wide view of a
  never-aggregating field collapses from one draw per cell to a single run over its whole
  in-frustum range. No sorting or spatial hierarchy — the batching is buffer contiguity alone.
- **The resolved sprites take one of two per-field paths, and the pass reports which drew.** The
  **compute** path runs a per-frame expansion dispatch over the run table that does the per-point
  work once — project, pixel-clamp, flux-gain, opacity-fold — writing one compact
  `GpuSpriteRecord` per point into a ring-buffered record buffer through an atomic append cursor,
  **compacting out zero-contribution points** (behind the eye, sub-epsilon folded color, fully
  offscreen) and finalizing an indirect draw command; the resolved sprites then draw through **one
  `DrawIndexedIndirect` per field**, the sprite vertex stage a record fetch plus a corner FMA. The
  **direct** path is the fallback: it expands every point in the vertex stage from the resident
  point SSBO, issued as one indexed draw per run. It is selected automatically when the compute
  pipeline's device features are absent, and is also the A/B verification reference and the
  first-frame-after-rebuild path. Selection is per-field and honestly reported through
  `PointFieldStats::DrawSource` (`Compute`/`Direct`/`None`), mirroring `GetActiveCullMode()`;
  `SetPointFieldForceDirect` forces the direct path for the A/B comparison.
- **`PointFieldLod::DepthFade` gates the per-fragment occluded fade** (default on). On, a sprite
  fragment samples the g-buffer depth and dims an occluded point; off skips that sample, the
  sub-rect remap, and the compare entirely — right for a field composited over background with no
  occluding geometry (a sky-scale backdrop, a map), where the fade can never trigger. Two sprite
  fragment permutations are built once and selected per field by the knob. The point-field
  fragments index the depth texture/sampler **uniformly** (the indices are per-draw push
  constants, uniform by construction).
- **Both draw paths index quads through one pass-owned index buffer.** A sprite or splat expands
  to a quad of **4 unique vertices** (indexed `0,1,2, 1,3,2`, `SV_VertexID`-driven — no vertex
  input, no per-instance attribute, no base-instance capability); the shared `u32` index buffer
  grows on demand to the largest quad count any draw has needed and rebuilds only on growth,
  retiring the old buffer through the per-frame deferred-destruction path.

The pass is inserted only while a live field exists, so a fieldless scene runs the plain deferred
path and the smoke golden is unaffected; no example consumes a `PointField`, and the GPU suite
(`tests/gpu/point_field.cpp`) is the conformance surface — it runs the cull, the LOD switch, and
both sprite paths (compute and forced-direct) against the same brightness assertions.

### Volume fields

**A volume field ray-marches a bounded emissive, light-absorbing medium into the lit scene color.**
Where a point field sums discrete flux, a volume field integrates a *density function over a region
of space* — a nebula, a dust bank, a glowing gas cloud — so the medium's shape holds up under camera
orbit: its dust lanes silhouette, its bright core glows through its own haze, and it occupies scene
volume rather than reading as a flat screen-space glow. The capability is deliberately bounded to
**emission + extinction**: emitted radiance density in RGB, light-absorption density in A, both per
world-unit, packed into one 3D texture and sampled once per march step. There is no in-scattering
from scene lights, no self-shadowing, and no phase function — that is lit/shadowed participating
media, a named future increment, not this.

- **Resource / component split, the point-field model verbatim.** `Renderer::VolumeField`
  (`Veng/Renderer/VolumeField.h`) is the GPU resource: a `Type3D` emission+extinction texture + its
  view + sampler + a **world-space AABB**, `Build`/`BuildSync`-constructed from CPU voxel data
  (worker-legal creation, no bindless registration — the dedicated-set decision below). The
  reflected **`VolumeField` scene component** (`Veng/Scene/Components.h`) carries the authored,
  live-tunable knobs — `Opacity` (an overall fade scaling emission and extinction toward zero),
  `EmissionScale`, `ExtinctionScale`, `Steps` (the fixed march step count, the quality knob,
  default 64) — plus a **runtime-only `Ref<VolumeField> Field`** (no `VE_FIELD`: never reflected,
  cooked, or serialized; a system or app builds the resource and assigns it). **World-space bounds
  live on the resource; the entity `Transform` is not applied** — the `PointField` contract exactly.
- **Presence-driven, no settings toggle.** `VolumeScenePass` (`Renderer/Passes/VolumeScenePass.h`)
  exists in the compiled graph **iff a live `VolumeField` component (non-null built field) exists**,
  resolved by the renderer itself each `Execute` (`View<VolumeField>`, the lights model — no
  `SceneRendererSettings` arm). With no field the frame is **byte-identical** and the smoke golden
  does not move; no engine example authors a volume, so the golden is unmoved across the whole
  capability.
- **The lit scene-color slot, after the sky composite, ahead of the refraction grab.** The pass
  draws into the lit scene color **after deferred lighting and the sky composite** (so it attenuates
  the backdrop behind it) and **before the refraction grab and the translucent pass**. Everything
  downstream then treats it as scene content: the pre-translucent **refraction grab captures it**, a
  **translucent blends over it**, **SSR reflects it**, and — the load-bearing one — the **TAA
  resolve is the march jitter's integrator** (the per-pixel start offset is dithered and temporally
  rotated precisely so TAA converges it to a smooth result; without TAA the march reads as
  per-pixel noise that a consumer must hide with a high step count).
- **One fullscreen draw per field, blended `(ONE, SRC_ALPHA)`.** The fragment marches the view ray
  front-to-back and returns `float4(accumulated emission, surviving transmittance)`: with the blend
  `srcColor = ONE, dstColor = SRC_ALPHA`, **one blend both adds the medium's glow (`+ L`) and
  attenuates the background (`× T`)**. The march clips the ray to the field's AABB (a textbook slab
  intersection, mirrored on the CPU by `Renderer::ComputeMarchSegment` in `VolumeMarch.h`) and to
  the reconstructed g-buffer scene depth — so opaque geometry in front of the field shortens or fully
  occludes the march — steps a fixed `Steps` count with a per-pixel interleaved-gradient-noise start
  jitter (Jiménez, SIGGRAPH 2014), and **early-outs once transmittance falls below ~0.003**
  (effectively opaque). The pure-math half — the segment clip, the far-to-near ordering predicate,
  the resolved per-field draw record — lives device-free in `Veng/Renderer/VolumeMarch.h`,
  unit-testable with no ICD.
- **The 3D texture binds through a dedicated per-pass set, not set-0 bindless.** The pass binds set 0
  (view constants + the bindless depth texture) plus **its own volume set** (the `Texture3D` +
  sampler) — the IBL-cubemap / shadow-atlas precedent: a non-2D descriptor inside set 0's Metal
  argument buffer is a MoltenVK mistranslation risk the engine refuses once, and a closed
  producer→consumer resource needs no global registration.
- **Overlapping fields composite independently — a documented approximation.** Live fields draw
  **far-to-near** (`VolumeFieldFartherFirst`, by camera distance to bounds center), so each nearer
  field's `(ONE, SRC_ALPHA)` blend attenuates whatever the farther fields already composited behind
  it. Two *overlapping* fields, though, each attenuate the other's **entire** contribution or none,
  by draw order — not their true interleaved optical depth. This is acceptable at the "a handful of
  volumes" scope the capability targets; correct interleaving would need a single merged march.

**Named future increments** (none built here): **lit / shadowed media** (in-scattering from scene
lights, self-shadowing, a phase function — sun shafts, shadowed fog); a **froxel grid / global fog**
system (this is bounded fields, not a scene-wide volumetric); **cooked volume-texture assets** (a
`VolumeField` is runtime-`Build`-only today — an imported/cooked 3D-texture asset, with its format
questions, is future and moves nothing in the cooked formats); **per-sprite attenuation through a
volume** (a point-field star dimmed by the dust in front of it — today points composite over the
medium unattenuated); **shader-side detail-noise modulation** (adding sub-voxel structure beyond the
baked resolution); and **half-resolution marching** (marching into a half-res target and upsampling,
trading the per-pixel march cost for a bilateral resolve). No example consumes a `VolumeField`; the
resource's own upload path and the pure march math are the conformance surfaces.

## Viewport: a region + a renderer + a role

A `Viewport` (`Veng/Renderer/Viewport.h`) is *"a renderable view into a world"* made first-class:
it owns a `SceneRenderer`, carries a **`ViewportRegion`** (its rectangle in window framebuffer
pixels — an `Offset` and an `Extent`, the extent driving the render resolution), takes a per-frame
**`ViewState`** *pushed* by its owner, and exposes a **`ViewportRole`**. It is **`Unique`,
single-owner**; `Create(const ViewportInfo&)` is the factory. Owning the region is what makes the
name correct — a viewport is classically a rect of the render target — and it is what lets the
engine drive a list of them: the `Viewport` owns once the trio every consumer of a rendered scene
otherwise hand-wires (a `SceneRenderer`, a sampler, an ImGui texture, the `Execute` + `Sample`
barrier).

- **The role gates engine compositing, nothing else.** `ViewportRole::Presented` — the engine
  compositor places the viewport's texture into its region (a fullscreen game is one viewport
  covering the window; a splitscreen quadrant is one of N). `ViewportRole::Offscreen` — a consumer
  samples the texture (an ImGui panel, a material). Both roles render *identically*: every
  viewport renders into its own target at its region's resolution, and the deferred pipeline never
  scatters into a swapchain sub-rect. The region is universal state — an `Offscreen` editor panel
  still owns a region (for resize + picking); the role only decides whether the **engine** places
  it.
- **Push the per-frame source.** The owner sets a `ViewState` each frame (the `Scene` to render,
  the resolved `CameraView`, `Delta`, and the tone/bloom knobs — the input subset of the
  renderer's internal `SceneView`); the viewport never reaches into the scene for a camera. A null
  `World` renders nothing (a closed document is a no-op, not a null deref). The viewport retains
  the camera for screen-to-world mapping.
- **`Render(cmd)` does Execute + the Sample barrier.** It applies any pending region resize,
  builds the internal `SceneView` from the bound `ViewState`, calls `SceneRenderer::Execute`, then
  `PrepareForAccess(Sample)` — so the output is sampleable when the frame's later consumers read
  it. Its product is a sampleable `Ref<ImageView>` (`GetOutput()`) and a bindless `TextureHandle`
  (`GetOutputHandle()`) for the compositor, `ImGuiLayer::CreateTexture`, or
  `Material::SetTextureHandle`. Both invalidate on an extent change applied in `Render` and on
  `Configure` — re-fetch after, exactly as the underlying `SceneRenderer::Resize`/`Configure`
  invalidate `GetOutput()` (see the `SceneRenderer` section's lifetime split).
- **Central driving, local ownership, RAII cleanup.** The engine drive-list holds raw `Viewport*`
  (registration order = render order), not the viewports; the owner constructs and registers —
  `m_vp = Viewport::Create(info); app.RegisterViewport(*m_vp);` — keeping the owning `Unique`, and
  `~Viewport` removes the engine's pointer through the stored back-reference. So registration is
  explicit but dropping the `Unique` is the whole of cleanup, and "0..N viewports including zero"
  is the list length.
- **The render-phase order is render-all → `OnRender`/ImGui → gather + composite.** The engine
  renders every registered viewport first (so every output is in `Sample` layout), then `OnRender`
  builds the ImGui frame (an `Offscreen` panel draws `UI::Image(vp.GetOutput())`), then — when
  ImGui is on — the overlay records and the managed tail gathers the `Presented` viewports and
  composites. The managed primary viewport is the game's plug-and-play path; the editor registers
  no `Presented` viewport, so the gather assembles **zero placements** (a cleared target) and the
  composite is ImGui-only.

**A gather pass assembles; the composite encodes.** `GatherPass` (`Veng/Renderer/GatherPass.h`)
scissor-blits each `Presented` viewport's texture (a `CompositePlacement` = its `Ref<ImageView>` +
its `ViewportRegion`) into its region on one full-window linear-HDR (RGBA16F) **assembly target**,
in list order, clearing the area no placement covers; `SwapChainCompositePass` then consumes that
single target *unchanged* (ImGui over, the display-transfer encode once). One window-covering
placement is the fullscreen-game case (a point-sampled same-resolution copy, so the assembled
values are bit-identical to sampling the source directly); zero placements is the editor (a
cleared target); N quadrant placements is splitscreen — the same gather + composite tail for all
three, the HDR/color-space encode left untouched. `SetPlacements` registers exactly one bindless
slot per placement (`MaxPresented` is the budget, asserted at register time). **Splitscreen falls
out** as "register N `Presented` viewports with quadrant regions"; it needs no bespoke compositing
path.

**A presented frame is read back through a mirror, never off the swap chain.** The finished
composite — scene plus whatever overlay was drawn over it — exists only in the swap chain image, and
`vkQueuePresentKHR` hands that image to the presentation engine: it is not the application's again
until it is re-acquired, so transitioning it for a readback afterwards is a **write-after-present
hazard** the synchronization validation layer reports as an error (and, on MoltenVK, a readback that
can stall on the held drawable). So `Context::ArmPresentedFrameCapture()` arms a mirror instead:
`EndFrame` blits the composite into an engine-owned image **immediately before the present
transition**, the last point at which the frame still owns the swap chain image, and
`GetPresentedFrameMirror()` hands that copy back to be downloaded as an ordinary owned image. The
mirror follows the swap chain's format and extent (rebuilt when either moves), needs the surface to
have granted transfer-source usage (`IsSwapChainCaptureSupported()`), is inert headless, and costs
one full-window blit per frame — which is why it is armed on demand rather than always on, and why
nothing arms it unless a consumer asks (`veng::mcp`'s `render.screenshot_window` is the one that
does).

**Owning the region yields a window↔view mapping.** `WindowToViewport(windowPoint)` hit-tests a
window point against the region and, on a hit, remaps it to normalized `[0,1]` across the region
(nullopt outside); `ScreenToWorldRay(windowPoint)` composes that with the camera retained from the
last `ViewState` — mapping the point to NDC and unprojecting it through
`glm::inverse(camera.ViewProjection())` into a world-space `Ray` (`Veng/Math/Ray.h`, a glm-only
origin + direction value type) whose origin is the camera and whose normalized direction passes
through the pixel (nullopt outside the region or before any `ViewState`). These are
**gameplay-agnostic** primitives — the viewport imports no `Viewer`/`PlayerInput`; it supplies the
ray, and what the ray hits (a scene raycast) is editor or gameplay code. Editor entity-picking and
multi-seat pointer routing (the `InputRouter`'s `PointerRouting` hit-tests `WindowToViewport` to
decide which quadrant a click landed in) consume these primitives.

**Adaptive resolution eases a per-frame sub-rect over a fixed allocation.**
`SetDynamicResolution(settings)` engages it; it runs inside `Render`, before the pending-resize
apply. Each `Render` reads `Context::GetLastGpuFrameTimeMs()` and steps
`ComputeDynamicResolutionScale` (`Veng/Renderer/DynamicResolution.h`) toward a GPU-frame-time
budget, rendering into a `round(allocExtent · RenderScale)` sub-rect of the allocated targets that
the terminal tonemap upscales. It is **free**: a sub-rect change moves no allocation, only the
per-frame `SceneView::RenderScale` fraction — so it adapts **cost**, never the allocation
footprint, and never hitches. `GetAllocationScale()` reports the fixed allocation scale
(`MaxScale` while dynamic resolution is on, else the static `RenderScale`). The allocation is
sized **once** to the region's native extent (capped by `MaxAllocationScale`), and the expensive
`SceneRenderer::Resize` — which retires every target, re-registers bindless, and recompiles the
graph — fires only on a genuine region/window extent change or an explicit
render-scale/`MaxScale` change, never from frame-time pressure.

**`MaxAllocationScale` is a fixed ceiling on the allocation relative to the backing extent,
defaulting to `1.0` (full native).** It caps the allocation to a fraction of the region's pixels
and is the **outer** of two multiplicative scales — the allocation extent is
`round(region · MaxAllocationScale · GetAllocationScale())` (`ExtentForScale`), then the sub-rect
rides inside it as `GetViewRenderScale()`. A managed viewport tracks the full swapchain
framebuffer extent — 2× the logical window on a HiDPI display — and the default `1.0` renders at
those backing pixels: **native resolution on a HiDPI display, not supersampling**. A value below
`1.0` is a deliberate lower ceiling for an app that wants a fixed perf budget; it is not the
default posture. The managed primary viewport exposes this through `ManagedViewportInfo`
(`RenderScale`, `MaxAllocationScale`, `DynamicResolution`).

**The registration-order RTT contract.** An `Offscreen` viewport's `GetOutputHandle` can be bound
into a material (`Material::SetTextureHandle`) so one viewport samples another's output. Because
registration order is render order, a producer registered **before** its consumer ends its
`Render` with the output in `Sample` layout before the consumer's `Render` reads it. Both halves
record on the single graphics queue in submission order, so the handoff needs **no ring and no
semaphore** and the output stays **single-copy**; the producer's next-frame `Execute` transitions
it back to `ColorAttachment`. Registration order is the render order; the handoff is same-frame,
same-queue, and single-copy.

## SceneCapture: the probe primitive, and what it does not draw

`SceneCapture` (`Veng/Renderer/SceneCapture.h`) is the render-to-texture sibling of the viewport: it
owns one small `SceneRenderer` and, each frame a fresh `CaptureView` is pushed (`SetView`), renders
the scene through one of **six 90° face cameras** (round-robin, so a full refresh spans six pushed
frames), tiles the HDR result into a persistent 3×2 face atlas, and resamples that atlas into an
**octahedral 2D map** a material samples by direction (`OctahedralUV`, `Veng/octahedral.slang`). The
output is **pre-tonemap linear HDR** and a plain **2D** bindless texture — a cube view cannot ride
the set-0 bindless array — so it binds onto a material through `Material::SetTextureHandle`. It is
**push-to-render**: a frame with no fresh `SetView` records nothing, so an idle capture costs
nothing. `ViewportCompositor` drives the registered captures ahead of every viewport — within the
view budget it can leave those viewports, round-robin across frames when they do not all fit — so a
material sampling one reads this frame's result. `CaptureSurface` (the reflected component, see
[../Gui/CLAUDE.md](../Gui/CLAUDE.md)) is the authoring front end.

**A capture in a world nothing presents is not driven.** A capture feeds a material sampled by a mesh
drawn in some view, so a world no view shows has nowhere its capture could be seen — and worlds are
flat peers of which several are live at once in the ordinary case, so driving every live world's
captures multiplies the per-frame view budget by the number of worlds held warm. `WorldRunner`'s
per-frame drive therefore asks presentation first (`Application::IsWorldPresented` — a managed or
bound viewport's binding, a viewport a consumer drives itself, or an in-flight rebind's destination
for its whole wait, so a make-before-break swap presents a warm probe) and skips an unpresented world
whole, re-arming its already-materialized captures (`CaptureSurface::MarkDirty`) so a world that
becomes visible again rebuilds its maps instead of resuming from what it saw before it went dark.

**A capture never draws the mesh it feeds — a surface is not part of its own environment.**
`CaptureView::Exclude` names one entity the face renders skip, and `CaptureSurface` sets it to the
entity it is driving for, so the rule has no authoring surface and cannot be misconfigured. Two
distinct defects are what it removes, and the geometric one is the worse:

- **It would compound.** A material that adds a term sampled from capture *N−1* appears in capture
  *N*, so the authored reflection weight sits inside a feedback loop — not divergent at realistic
  weights, but a shimmer tracking camera motion that no amount of authoring can tune out.
- **It would occlude.** The probe sits at the entity's world position, which for a pane, mirror or
  monitor is *on or inside its own surface* — so the mesh does not merely add light, it hides the
  environment across whatever share of the sphere it subtends. `CaptureView::Near` is `0.05` and
  cannot be relied on to clip a surface the probe sits on.

**The exclusion is by entity, applied once, in the gather.** `SceneView::Exclude` carries it into
`SceneRenderer::Execute`, which passes it to `SceneBroadphase::Sync` — so `GatherMeshes` drops the
entity and it is absent from the candidate list, the per-submesh BVH leaves, and both scene bounds.
Every consumer downstream therefore misses it in **every domain**: opaque, translucent, colour and
depth alike. Excluding per-pass would be the same defect wearing a different hat (a mesh dropped
from colour but left in depth still carves a hole), and excluding by *mesh* or *material* would be
wrong outright — a `MaterialInstance` is shared by many entities, so it would blank every other user
of it out of the capture. Because the exclusion is a property of the caller's view and not of the
scene, it moves no spatial version, so the broadphase treats a **changed** exclusion as its own
rebuild trigger. `Entity::Null` (the default) excludes nothing and gathers exactly what it gathered
before, so no other view is affected.

**A capture's batteries are lean, and shadows are the one an interior probe asks back.** Bloom, AO,
SSR and TAA are dropped unconditionally — the capture samples pre-tonemap HDR, so the post chain
never reaches its output, and the rest is cost multiplied across the faces. Shadows are dropped by
default for the same reason, but they are the one battery with a case where the omission is
*visible*: an **enclosed interior** captured without them is lit by the directional source as though
its own walls did not occlude, so a cabin or a room renders uniformly flooded — brightest where it
should be deepest, and with no contact darkening to give the space its shape. A probe reflecting
that interior then shows a lit box, and the defect reads as the consuming material's fault.
`CaptureSurface::Shadows` (default off) turns both shadow batteries back on for that case. The cost
is **one depth-only pass per driven frame, not six** — a capture renders one face per frame, so the
shadow pass rides that single face render. Both flags move together: an interior wants its
enclosure's occlusion whichever kind of light casts it. They are topology changes in the face
renderer, so the field is read when the runtime materializes and is not live-tunable.

Deliberately **not** here: a general per-entity or per-layer visibility mask (this is one nominated
entity in a closed producer→consumer pair, with no authoring story to get wrong), **recursive
probes** (another capture-consuming surface in the map reads a one-frame-old result, invisible at a
reflection's contrast), and **parallax correction**, whose math belongs to the consuming material —
`CaptureSurface::CenterSlot` publishes the world position the map was rendered from, plus a validity
flag, and `CaptureSurface::OrientationSlot` the frame the faces were oriented in as a quaternion, so
the material has the two inputs it cannot derive for itself.

## Generated textures: compute something expensive once, then sample it

`GeneratedTextureService` (`Veng/Renderer/GeneratedTextureService.h`) is the engine's answer to
"fill a persistent texture with GPU work too expensive to pay per frame". It is **`Context`-owned**
(`Context::GetGeneratedTextures()`) and **pumped once per frame from `BeginFrame`**, before any pass
records, so a job's result is sampleable by the passes of the frame that finished it.

A **job** is `{target images, tick count, tick callback, priority}`, keyed by a caller-chosen `u64`:

- **A target is an `ImageInfo`** — any format, extent, **layer count and mip count**, or an already
  created image to `Adopt`. So a 2D map, a 6-layer cube and a mip chain are all just targets. Each
  declares the **`ProducerAccess`** its ticks write through (`StorageWrite` for compute,
  `ColorAttachment` for a face render), which is also what decides the image usage the service ORs
  in beside `Sampled`. `Bindless` registers the sampled view into set 0 and is legal only for a
  single-layer 2D target — the array is strictly 2D; everything layered binds through the
  consumer's own descriptor set, as the sky and IBL paths do.
- **A tick is the amortization quantum.** The tick callback is
  `void(CommandBuffer&, const GeneratedTextureTickContext&)` and may record compute dispatches,
  raster passes, or both — the six face renders of a cube are six raster ticks, an octahedral
  downsample chain is one tick per mip. `GeneratedTexture::GetView(mip, layer)` hands back the
  subresource views a tick binds, created on first ask and cached; a view is a view, so the same
  one serves as a storage image or as a color attachment.
- **The service inserts the barriers around the ticks and nothing else.** Before each tick every
  target is prepared for its `ProducerAccess` — the transition out of `Undefined` on the first tick,
  a write-after-write barrier ordering tick N+1 behind tick N after that — and on the tick that
  exhausts a job every target is transitioned to `Sample`, the completion fires, and the job becomes
  queryable as **resident** (`IsResident` / `Find`, held until `Release`).
- **Scheduling is priority-then-FIFO, re-evaluated per tick**, spending a tick budget per frame
  (`SetTickBudget`, `UnlimitedTickBudget` for none). Raising a queued job's priority therefore
  preempts a running one at the *next tick*, not the next job. Requests are **idempotent on the
  key**, so re-requesting every frame while the result is still wanted is the intended usage;
  `Cancel` tears an unfinished job down and simply releases its targets.

**Nothing ever waits.** No immediate submits, no fences on the render thread, no job started outside
the pump. A consumer whose approach outruns its bake gets "the result lands a moment later", never a
hitch — it keeps drawing whatever it drew before. And the service makes **no policy decisions**:
what to generate, when, at what resolution, and how long to hold it belong to the caller entirely.

**Allocation is not on the frame thread either.** `Request` is called from the pump, and a target
pair at the top of the size range is hundreds of megabytes of VMA allocation — so the images, their
views and their samplers are created on a task-system worker (`VolumeField::Build`'s precedent: all
three are worker-legal) and the job is **held** until they land. Only the set-0 registration a
`Bindless` target asks for stays on the main thread, in the continuation that adopts the targets. An
**adopted** target is not the service's to allocate, so a job made entirely of them takes no hop and
runs at the next pump; so does any job when no task system is attached, which is the device-free
posture the unit and gpu fixtures run in. The hold is one flag on the queue record derived from
*every* reason a job is not selectable (`Allocating || Probing || Restoring`), so the allocation
hold and the cache-probe hold **compose**: a cached job goes from one to the other without becoming
selectable at the seam. `GeneratedTextureStats::Allocating` reports it beside `Probing`.

The **scheduling core is device-free**. `GeneratedTextureQueue` (`src/Renderer/GeneratedTextureQueue.h`,
renderer-internal) holds the job records and the selection rule and knows nothing about images, so
the whole policy surface — idempotent keys, priority ordering, budget accounting across mixed jobs —
is pinned by `tests/unit/generated_texture_queue.cpp` against a mock tick recorder with no ICD, the
`FrameTopology` / `DrawBudget` precedent. `tests/gpu/generated_texture.cpp` carries the GPU half.

**It records around the render graph, not through it.** `TransientDesc` is 2D single-layer only and
a compiled graph is static between `Rebuild`s, neither of which suits a per-frame-varying set of
layered, mipped targets — so the service records into the frame command buffer with explicit
barriers, exactly as `AtmospherePrecompute` already does, just budgeted. `AtmospherePrecompute`,
`EnvironmentIbl`, `SkyCubemapBake` and the picking readback are four in-tree hand-rollings of its
parts; they keep their current shapes, and are named here because they are what the API was shaped
against rather than because anything migrates.

### The frame-deferred readback

`AsyncReadback` (`Veng/Renderer/AsyncReadback.h`, `Context::GetAsyncReadback()`) gets a finished
image's bytes to the CPU without blocking: `Request` allocates a host-mapped staging buffer, the
copy rides the frame's command buffer at the same pump point, and the completion is delivered on the
main thread once `GetMaxFramesInFlight()` frames have passed — the point at which the staging
frame's fence has provably been waited. It is the picking system's pattern
(`PickingSystem::ServiceRequest` / `PollPickId`) promoted to a public utility, and it is usable with
or without the service: it reads any image carrying `TransferSrc` plus a view-compatible usage, at
any mip and any array layer (`CommandBuffer::CopyImageSubresourceToBuffer` is the copy underneath —
the region-less `CopyImageToBuffer` is its mip-0, layer-0 case, and the multi-region overload beside
it reads a whole mip chain into one buffer). The subresource is left prepared for
`AsyncReadbackRequest::RestoreTo`, `Sample` by default, because a bindless-sampled image left in
`TransferSrc` would be read in the wrong layout.

**There is no wait path to call.** The class never submits, never waits a fence, and never idles the
device. `Image::Download` stays the synchronous sibling for tooling that genuinely wants the bytes
now (a screenshot); nothing on a frame path should reach for it.

### A result can outlive the process

`SetCache(cache, tasks)` attaches a `DerivedDataCache` (`Veng/Persistence/DerivedDataCache.h`) and
the task system its file I/O runs on, and a job carrying a `CacheKey` is then answered from disk
when an earlier run computed the same thing. **The cache is transparent**: a hit is a texture that
arrives without the job's ticks running, a miss is the job running exactly as if no cache existed,
and a deleted cache directory is a valid state at any moment. A consumer sets a `CacheKey`
unconditionally — with no cache attached the field is inert.

The round trip in both directions, and where each half runs:

- **The probe.** `Request` **holds** the job in the scheduling queue — the key is live, so a
  re-request is still idempotent and the job still reads as pending, but no tick is spent on work
  the cache may already hold — and submits the read to a worker. The answer lands on the main
  thread through the continuation pump: a miss releases the hold and the job runs; a hit reads the
  payload's **header only**, checks the shapes, and hands the payload to a worker that copies its
  texels into a host-mapped buffer, still held (a **tail** job — below — reads the header on its own
  and has its worker read the levels it wants straight into that buffer). The next pump after that copies the buffer into the
  targets ahead of the tick loop, marks the job resident, and fires its completion. A restored
  job's texels are therefore sampleable by the same frame's passes, exactly as one whose last tick
  ran that pump, and `TicksLastPump` never counts a restore.
- **The store.** A cached job that completes the ordinary way is read back into **one** host-mapped
  buffer — one `CopyImageToBuffer` per target, one region per mip covering every layer, at the
  offset that target's levels occupy — recorded into the same pump's command buffer and readable
  `framesInFlight` pumps later. The pending store holds **no reference to the job**, so releasing
  the result while its levels are in flight neither strands the store nor dangles. When the bytes
  are readable a worker builds the payload and writes it.

**No copy the size of the payload runs on the frame thread**, in either direction. That is what the
shapes above are for: the store's readback is one buffer rather than one per `(mip, layer)`, its
encode reserves the header in a buffer already sized for the texels so they are copied once, on a
worker, and the restore parses a header instead of decoding a payload and runs its host copy on a
worker too. The service's own hold is what makes both safe — a job is unselectable for as long as
*anything* it waits on is outstanding, so a restore's ticks can never race the texels landing.

Two constraints follow from the copies. A cached target the service creates has `TransferSrc` and
`TransferDst` folded into its usage; an **adopted** target carrying neither is simply not cached and logs why, since the service does not own its
creation. And the **stored shape must match**
— format, type, extent, layers, mips — or the entry is a miss: texels uploaded into an
image they do not describe are worse than no cache at all.

**A target may hold the coarse tail of a chain rather than the whole of it.**
`GeneratedTextureTargetInfo::CacheMipOffset` names the mip level of the *stored* shape that this
target's own mip 0 restores from, so the shape test becomes "the stored shape reduced by this many
levels is exactly this target's shape" — the plain equality above being its zero case. It exists for
the consumer that generates a chain at full resolution and holds only its coarse levels in memory:
without it, holding N resolutions means storing N entries of overlapping texels, keyed per
resolution, and re-deriving whichever one the cache last evicted. The tail's levels are contiguous
within the entry (levels run mip-major), so it is one byte range per target, read through
`DerivedDataCache::ReadRange` — the restore's I/O is the levels the target holds, not the levels the
entry does, and the staging buffer is sized the same way. Three properties bound it:

- **A tail restores but never stores.** It holds less than the entry it read, and writing that back
  would replace the entry with a fragment of it — so a tail target is only ever useful against an
  entry some other job wrote at full shape.
- **It buys the cache's digest check for the range read** (see
  [../Persistence/CLAUDE.md](../Persistence/CLAUDE.md)); a whole-shape restore still verifies it.
- **The offset is checked, not assumed.** A declared level whose reduced shape is not the target's is
  a miss like any other, and a miss runs the ticks — so a tail target's tick callback must be able
  to fill it at its own shape, which is also what covers an entry evicted between the probe and the
  levels.

The byte layout is `src/Renderer/GeneratedTextureBlob.h`, a renderer-internal, device-free codec
(the `FrameTopology` precedent): shapes, then every target's levels **mip-major, layer-minor**,
which is the order a per-mip, all-layers copy reads and writes them in both directions.
`tests/unit/derived_data_cache.cpp` pins the codec beside the cache that carries it — including
that the header-plus-appended-texels the store assembles is byte-for-byte the payload a whole
encode produces; the end-to-end transparency property is in `tests/gpu/generated_texture.cpp`.

## The fluid solver: advecting whatever it is handed

`FluidSim` (`Veng/Renderer/FluidSim.h`) is a **2D stable-fluids solver over caller-supplied
images**. It renders nothing, owns no meaning, and makes no aesthetic decisions: what a dye channel
*is*, what the initial fields look like, and how many steps are worth running belong entirely to the
caller, and the solver **never fills initial conditions**. The scheme is Jos Stam's, reimplemented
from his published papers — the mathematics, not anyone's code.

- **The fields are the caller's.** A velocity image (`RG16Sfloat` or `RG32Sfloat`, in grid cells per
  unit of simulated time, its extent *is* the grid) plus up to `MaxFluidDyes` dye images
  (`R16Sfloat` / `RG16Sfloat` / `RGBA16Sfloat`), optionally a relaxation target, a damping mask and
  a per-row metric. The solver allocates only its own transients — one `RGBA32Sfloat` advection
  scratch, a curl field, a divergence field, and two ping-ponged pressure images — at the caller's
  resolution.
- **One step, six-ish dispatches plus the Jacobi count.** Advect the velocity through itself; take
  its curl; apply vorticity confinement, relaxation toward the target, and the damping mask; take
  the divergence; run `JacobiIterations` (default `DefaultFluidJacobiIterations`, 20) pressure
  relaxations from zero; subtract the gradient; then advect each dye through the projected velocity
  with its own dissipation. `RecordStep(cmd)` records exactly that into whatever command buffer it
  is handed, leaving every field in a sampled layout.
- **Per-axis wrap and a per-row metric are the only geometry.** `FluidWrap::Periodic` folds a
  coordinate around; `FluidWrap::Clamped` clamps it and has the projection zero the wall-normal
  velocity at the outermost texels (free slip). The metric scales the x-derivative and the
  x-advection step per row — a **stretch on the grid**, documented as nothing more; a caller reads
  whatever it likes into rows being shorter here than there.
- **Nothing rides bindless and nothing touches the render graph.** Every kernel binds its own set 1
  and the solver records its own barriers, the `AtmospherePrecompute` pattern. A caller amortizing a
  long spin-up wraps `RecordStep` in `GeneratedTextureService` ticks; **the solver holds no reference
  to the service**, so the dependency points one way and the solver is equally usable with a bare
  command buffer.

**Why the force, gradient and store stages are shader families.** A storage image's format qualifier
must match the image it writes, and those three stages write the *caller's* fields, so each has one
variant per accepted format (`engine/assets/core/shaders/fluid/`, a body header plus a two-line
per-format includer). Advection escapes that: it cannot run in place, so it writes the solver's own
fixed-format scratch and a store pass — which also applies the dissipation — moves the result back
out. That is why one advection kernel serves the velocity field and every dye.

**Two properties the tests pin, and one they deliberately do not.** The solve restarts from zero
pressure each step (iteration 0 reads no neighbours), so a step is a pure function of its inputs and
`tests/gpu/fluid_sim.cpp`'s digest case runs the same configuration twice and gets the same bytes;
that digest is **pinned on the reference host and re-pinned freely**, since cross-device bit-identity
is not promised. The projection's residual has a floor that is **not** a convergence failure:
divergence and the pressure gradient are two-texel-wide differences while the Jacobi stencil is the
compact Laplacian, so a collocated grid keeps a `sin²(k/2)` share of each mode (~2.4 % on that
case's seed) however many iterations run — a staggered grid is what removes it, and this solver is
deliberately the collocated scheme. The device-free half of the configuration check
(`src/Renderer/FluidSimShape.h`, the `FrameTopology` precedent) and the CPU reference for one
advection tap are pinned in `tests/unit/fluid_sim_config.cpp` with no ICD.

## Pipeline cache

`Context` owns a `vk::PipelineCache` created at device init and threaded into both the graphics
and compute pipeline factories — every pipeline build in a run reuses it. Persistence is
**opt-in** via `ApplicationInfo::PipelineCachePath`: set → seed from the file at startup + write
it back at shutdown; `nullopt` (default) keeps it in-memory only. A stale/foreign/truncated cache
file is safe — Vulkan validates the cache header and starts cold on a mismatch; veng feeds the
bytes as `pInitialData` and never parses them. The cache is touched only on the single render
thread, so it needs no external sync (off-thread pipeline creation would).

## Bindless: set 0 is the engine's

The engine provides a global `BindlessRegistry` (owned by `Context`, reachable via
`Context::GetBindlessRegistry()`): a few large arrayed, `partiallyBound` + `updateAfterBind`
bindings (sampled images, samplers, storage images, and the per-material parameter-block SSBO)
living in **set 0**. `Register(...)` allocates a free-list slot and returns a typed `u32` handle
(`TextureHandle`, `SamplerHandle`, `StorageImageHandle`, `MaterialHandle`); `Release` defers the
slot reclaim through the same per-frame retire window. **`PipelineLayout` reserves set 0 in every
pipeline** for the registry, bound once per pipeline bind (`registry.Bind(cmd)`), not per draw —
draws select array elements via push-constant indices. Author-declared descriptor sets shift to
**set 1+**.

**Samplers are shared, not registered per resource.** A sampler is pure state — nothing about it
varies with the image it reads — so `AcquireSampler(const SamplerInfo&)` is the way in: it keys a
cache on every field the GPU acts on (the debug `Name` excluded, floats compared by bit pattern)
and hands back the same `Ref<Sampler>` + `SamplerHandle` for a description already seen. The
occupancy of the sampler array is therefore the handful of filtering/addressing/LOD combinations a
build actually uses, not one slot per texture asset and five or six more per `SceneRenderer`. The
registry keeps a shared sampler for its own lifetime and its slot is **never released** — a caller
cannot know it is the last — so `Release(SamplerHandle)` asserts when handed one. The plain
`Register(const Ref<Sampler>&)` overload remains for a caller that genuinely wants a slot of its
own; a sampler bound only into an author-declared set wants no set-0 slot at all and stays on
`Sampler::Create`. A description that names the whole mip chain writes `MaxLod = LodClampNone`
rather than the image's level count, so an otherwise identical description does not fork per
texture — the image view's level range bounds the sampled mip regardless.
