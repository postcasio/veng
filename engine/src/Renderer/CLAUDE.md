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

**A pass's transitions record as one barrier command**, however many resources it declares
(`Backend::BarrierBatch`): each is still decided against the image's live tracked state; only the
recording is batched, into one command whose stage masks are the union of its barriers'.

**A read in a new pipeline stage takes a barrier even when the layout already matches.** A barrier
orders only the stages it names, so after the depth's attachment→sampled transition into fragment
reads, a compute read of the same depth is unordered against the depth writes. `DecideBarrier` (and
the buffer schedule) therefore skip a read-after-read only when the new read's stages and accesses
are already in the tracked read scope; otherwise they record a layout-preserving barrier whose source
is the earlier readers' stages — chaining after the barrier that ordered them, which is also what
orders the new read after that barrier's layout transition — and widen the scope to cover both.

**A buffer's first access in a graph waits on its last.** A buffer carries no tracked state, so its
barriers are baked at `Compile`, and each slot's tracking starts from the scope the graph ends on. A
compiled graph replays every frame and may replay more than once into one command buffer (one
renderer recording two views), so without that the second replay's first write would race the
first replay's; an image gets the same ordering from its live tracked state. It costs one baked
barrier per written buffer per replay.

**A pass with nothing to record some frames says so instead of rendering empty** —
`PassBuilder::SkipWhen(predicate)`, evaluated each `Execute`. A skipped frame records no render
pass, callback or GPU scope; its image transitions fall to the next pass using each resource
(decided against live state, so nothing is lost), and its baked buffer barriers still record. A
skippable pass must load and store every attachment it declares (asserted at `Compile`), since a
skipped clear or discard would change what the next pass reads. The debug-draw pass with nothing
queued, and the sprite, ribbon and half-resolution composite passes idling inside their
deactivation windows, are the users: on a tile-based GPU an empty render pass still loads and stores
its targets.

**A producer nothing reads some frames skips with `SkipWhenUnread`**, which may clear or discard:
the caller promises that on a frame the predicate holds every reader of the pass's outputs either
skips too or samples nothing from them, so the undefined contents a skipped clear leaves are never
observed. The graph cannot check that promise. The refraction grab and its blur chain (skipped on a
frame no draw samples the grab) and the half-resolution layer's depth reduce and layer pass (skipped
with their composite on an idle frame) are the users. `CompiledGraph::DidRecordPass(name)`, surfaced
as `SceneRenderer::DidRecordPassLastFrame`, reports whether a pass recorded on the last `Execute`.

## SceneRenderer: the deferred über-pipeline

`SceneRenderer` is a long-lived, configurable render pipeline on top of `RenderGraph`: it owns an
offscreen target, renders a `Scene` from a `Camera` through an **internal compiled `RenderGraph`**
of reusable `ScenePass` units, and hands back a sampleable result. It is **`Unique`,
single-owner** (nothing holds a `Ref` to one); `Create(const SceneRendererInfo&)` is the factory.

### File layout — the shape a new battery lands in

The renderer is split along three conventions, and a new battery follows all of them:

- **A pass lives in its own file under `Passes/`.** Every `ScenePass` — the g-buffer, deferred
  lighting, translucent, picking, TAA, the non-temporal scene upscale, scene-color copy, the
  directional and punctual shadow
  passes, SSAO, the skybox, the sky/point-field/volume/ribbon/sprite passes, the depth-of-field composite,
  the debug draw (and its companion
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
  precedent: `ShadowSystem`, `BloomPyramid`, `AutoExposureMeter`, `TaaResolve`,
  `PostResolveUpscale`, `SsrChain`, `DofChain`,
  `RefractionGrab`, `GpuCullSystem`, `LightTileCuller`, `GBufferShadingOverride`, `PickingSystem`, and `SkyResolver` (which itself owns the
  three sky radiance-cube helpers `EnvironmentIbl` / `AtmospherePrecompute` / `BakedSkyCube`).
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
- **The per-frame content-active flags are not topology.** The sprite and both ribbon passes'
  (`SpriteActive`, `RibbonActive` and `RibbonPostResolveActive`, each with its idle counter) live in
  `SceneRenderer::Internal` beside the plans they gate.
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
- **Static draws are sorted before they are laid out, and each run of equal slots is one instanced
  draw.** `GatherStaticOpaque` keys its survivors with `DrawKey` — (parent material, mesh,
  submesh, candidate) — and orders them with `SortDrawKeys` before claiming a slot, so every
  instance of one submesh takes adjacent slots. `GroupContiguousSlots` then cuts the slots into
  groups (one pipeline bind per parent material, one buffer bind per mesh) and each group into
  `InstanceRun`s — equal index range, consecutive candidate ids — and the g-buffer and picking
  passes record one `DrawIndexed(indexCount, runLength, …, firstInstance = first candidate)` per
  run. No shader changed for it: the surface vertex stage already reads its candidate id from the
  identity, instance-rate candidate-id buffer at `firstInstance + instance`. The key is the
  *parent* because a Surface instance's `Bind` binds its parent's pipeline and pushes nothing (its
  selector rides `DrawData`), so instances of one parent share a group. Pointers order the key, so
  the order between two meshes is arbitrary but fixed; the GPU-cull path consumes the same slots,
  still one indirect draw per group. Skinned slots are not sorted (their palettes are claimed in
  survivor order), but record through the same runs.
- **Per-entity frame state is flat.** This frame's and last frame's world matrices and palette
  bases live in `EntityFrameTable`s (`Veng/Renderer/EntityFrameTable.h`) — an array by entity slot
  whose entries carry the generation and frame stamp they were written under, so `Begin` retires a
  frame without clearing and a lookup is an index. A current/previous pair swaps in O(1) at the end
  of `Execute`. The normal matrix is not computed per draw at all: `VisibleMesh::NormalMatrix` is
  filled where the world matrix is (the gather and the interpolation pass).
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
  — `tests/unit/draw_budget.cpp`). The static phase triages every survivor before it claims a
  slot (it must, to sort), so the skinned and translucent lists are always complete; an exhausted
  budget counts the static survivors left unseated in `StaticDropped`, and the later phases, finding
  the budget spent, count their own.
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
- `Resize(extent)` / `Resize(extent, renderExtent)` — recreate the allocation-sized images via
  the retire path, re-register them into bindless, rebuild + re-`Compile()`. **There are two
  allocations** (see "The two allocations" below): `extent` is the **post-resolve** allocation —
  the output's size and what the whole HDR tail runs at — and `renderExtent` is the **render**
  allocation the scene rasterizes into. The one-argument form sizes both to `extent`. The per-frame
  `SceneView::RenderScale` then renders into a top-left `round(renderAllocExtent · RenderScale)`
  **sub-rect** of the render allocation (`GetValidExtent()`), and the promotion carries the finished
  scene colour up to the post-resolve allocation — so a per-frame resolution change costs no
  `Resize`, only a smaller rendered region. Sizing an allocation is the slow knob; the sub-rect is
  the fast one (see the `Viewport` section's two-loop model).
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

**The BRDF has one definition, and its distribution is evaluated in full.** The GGX distribution and
importance sampler, the Hammersley set, both Smith-Schlick geometry terms (the direct-lighting
`k = (r+1)²/8` and the image-based `k = r²/2`, which are different fits, not copies) and Schlick
Fresnel live in the binding-free `Veng/brdf.slang`, which the lighting core and both split-sum bakes
(`ibl_prefilter.comp`, `ibl_brdf_lut.comp`) include. `D` takes the GGX width `α` (perceptual
roughness squared), so a caller widening the lobe works in `α`, and it is Filament's stable form:
`1 − (n·h)²` is taken as `|n × h|²`, which keeps its precision toward the peak where the textbook
`(n·h)²(α² − 1) + 1` cancels. It carries **no clamp**, so a glossy highlight peaks at the full
`1/(πα²)` — about 124 000 at the lighting core's 0.04 roughness floor — and has the shape the
distribution gives it.

**The guard sits on the output, where the overflow is.** `GuardLightingOutput` (`Veng/lighting.slang`)
clamps a lighting entry point's radiance per channel to `LightingOutputMax` (6·10⁴), below the RGBA16F
scene colour's 65504. A full-strength lobe on a mirror metal reaches about 3·10⁴ times the light's
radiance at the floor, so a light brighter than the reference sun could write `inf`, which bloom's
Karis weight then turns into a `NaN` that spreads. A clamp on `D` would reshape the lobe and still not
stop the output overflowing; a clamp on the output leaves every representable value exact. The
deferred pass guards its final colour (direct, ambient and emissive together); the forward loop
guards each component it returns (see "Forward lighting for translucent surfaces").
`tests/gpu/specular_lobe.cpp` pins both properties on a mirror plane: the highlight's peak rises
strictly as roughness falls to the floor, and a floor-roughness mirror under a light a hundred times
the sun's stays finite and within the bound.

**Geometric specular anti-aliasing widens the lobe by how far the normal varies across a pixel.** A
full-strength lobe on a glossy surface is narrower than the normal's change across one pixel on a
normal-mapped panel or a small curved surface, so a highlight lands on a pixel one frame and misses
it the next, and TAA is not always on to hide it. `MakeLightingSurface` therefore takes the shading
normal's screen-space variance `|∂N/∂x|² + |∂N/∂y|²` (`NormalScreenVariance`, fine derivatives) and
applies Kaplanyan and Tokuyoshi's widening as Filament's `normalFiltering` does, once per surface:
`α² = saturate(α² + min(2·σ²·variance, κ))`, converted back to perceptual roughness. `σ²` is
`SceneView::SpecularAntiAliasingVariance` (default 0.15) and `κ` `SpecularAntiAliasingThreshold`
(default 0.2), Filament's defaults, carried in the view block's `AmbientParams.yz` (see "View
constants"); a zero `σ²` adds exactly nothing, which is how the feature is off. Because it changes the
surface's roughness it reaches every light's `D` and G, a sized light's widening (which composes in
`α²`), the LTC lookups and the image-based reflection's mip and BRDF-LUT lookups: a reflection blurs
where the normal varies. The deferred pass takes the derivatives in uniform control flow, ahead of its
background return, and zeroes the variance for any pixel whose 2×2 quad holds a background pixel — a
second fine derivative of the "has surface" edge reaches the quad's diagonal — since the cleared
normal would read as a huge variance and roughen a silhouette against the sky. **The deferred form's
known trade**: a quad straddling two different surfaces reads their normal difference as variance, so
those edge pixels roughen, bounded by `κ`. Detail finer than about two pixels is beyond what the
derivatives resolve and is a normal map's to pre-filter. `tests/gpu/specular_lobe.cpp` pins that a
flat mirror renders identically with it on and off, that a rippled mirror's reflected energy holds
under a half-pixel move, and that a sphere's silhouette texels shade as with it off.

### Anti-aliasing

**`Settings.AntiAliasing` is one mutually-exclusive `AntiAliasingMode`** — `None` (default), `FXAA`,
`TAA`, `CMAA2`, or `TAAU` — resolved by `FrameTopology` into at most one wired resolve. `TAA` and
`TAAU` are the HDR-space temporal resolve below; `FXAA` and `CMAA2` are the post-tonemap **spatial**
resolves under "Spatial anti-aliasing" further down. **Supersampling (SSAA) is not one of these
modes** — it is orthogonal, driven by the viewport's `MaxAllocationScale` (see "Adaptive resolution"
and the `Viewport` section), and composes with every mode. `Settings::UsesTaa()` is the named
predicate the jitter, the history-reset, and which resolve occupies the temporal anchor key on (true
for both temporal modes); the post-resolve tail runs at the post-resolve allocation in every mode and
keys on nothing. `Settings::UsesTaaUpscaling()` is `TAAU` alone, read by
`ResolveTemporalUpscalePromotes` to decide whether the temporal resolve is itself the promotion.

#### TAA

**TAA is an HDR-space temporal resolve** (`AntiAliasingMode::TAA`, off by default). It jitters the
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
prior world in `m_PreviousWorlds`, an `EntityFrameTable` swapped each frame) and the
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

**The resolve is sub-rect-aware, which is what makes it compose with dynamic resolution and act as an
upscaler.** The scene renders into the `round(renderAllocExtent · RenderScale)` sub-rect like any
dynamic-resolution frame, but the resolve **reconstructs its whole output allocation**: it maps the
output UV into the current frame's sub-rect through `ScaledSampleUV`
(`RenderScaleUV.xy`/`MaxValidUV.xy`) for the current/depth/velocity reads, while the **history and
output cover that allocation in full** (so the history read is the plain reprojected UV, and the
velocity — full-frame motion — needs no remap against it). The history is allocation-sized, so a
per-frame render-scale change never resets it. The temporal resolve is consequently **not in the
`drsSupported` exclusion** (`ResolveRenderScale`): `TAA` composes with dynamic resolution, and the
same reconstruction is what `TAAU` uses to upscale. At render scale 1.0 every map is the identity and
the frame is unchanged.

### The two allocations

**`SceneRenderer` holds two allocation extents, and every target belongs to exactly one.** A render
scale is meant to reduce the cost of rendering *the scene* and nothing downstream of it, so the two
costs are allocated separately:

- The **render allocation** — `region · MaxAllocationScale · renderScale` — holds everything the
  scene rasterizes and everything that reads the g-buffer: the five colour channels plus depth, the
  hi-Z pyramid, SSAO, the lit target, the SSR chain, the depth-of-field chain, the refraction grab
  and its mip chain, the half-resolution translucent layer, the bloom mask the translucent pass
  writes, the picking targets.
  `SceneView::RenderExtent` is this frame's sub-rect of it and `GetRenderAllocationExtent()` reports
  the allocation itself.
- The **post-resolve allocation** — `region · MaxAllocationScale`, no render scale — holds the tail:
  the promoted scene colour, the promoted bloom mask, the post-process effect ping-pong pair, the
  bloom pyramid (at half of it), the spatial-AA LDR intermediate and edge map, and the output. The
  overlay-document intermediate works in its pixels but is sized to the documents it holds, not to
  it (see "The pre-bloom GUI overlay"). `SceneView::PostResolveExtent` is **always** this, at any render scale in any AA mode.

`MaxAllocationScale` is the outer factor on **both**, which is what keeps supersampling supersampling
the tail with the scene. A debug view (`Mode != Final`) pins the render allocation to the
post-resolve one: its passes read the g-buffer at full-screen UVs while writing the output, and it
already forces the per-frame render scale to 1 for the same reason.

Between the two sits the **HDR scene colour** — what the temporal resolve, the SSR composite, the
HDR-placed point fields and the depth-of-field composite write. It is allocated in the render
allocation, except under a promoting temporal-upscaling resolve (below), where it is the
post-resolve one; `SceneView::SceneColorExtent` is its valid extent this frame.

**The promotion is the bridge, and it is a pass only when the two differ.** `SceneUpscaleScenePass`
reads the finished HDR scene colour and writes a post-resolve-allocation copy the tail reads —
one bilinear tap through the scene colour's own map, the same filter the terminal tonemap applied
when it carried this upscale, so the image is unchanged and the upscale is not performed twice. It
is declared at the HDR tail anchor, **after** the depth-of-field composite and **before** the
post-process effect chain, so every g-buffer-reading pass is upstream of it and the whole tail is
downstream. Below it the tail composes in a fixed order: the post-process effects, the post-resolve
ribbons (see "Ribbons and trails"), the pre-bloom GUI overlay, then bloom and the tonemap. It is wired only when the scene colour is not already the post-resolve allocation —
a reduced render allocation, a dynamic-resolution sub-rect, or both. The wiring is scale-driven with
the `HalfResTranslucency` shape: activated at the top of the `Execute` that first renders below the
post-resolve allocation (before the post-resolve extent is derived, so that frame runs the promoted
graph), dropped after `PostResolveUpscaleIdleFrameLimit` Executes back at it — deactivation
hysteresis, because a controller hunting across its ceiling would otherwise recompile the graph on
every crossing. `PostResolveUpscale` owns the vertical slice (the allocation-sized promoted targets
and their bindless slots, the upscale pipelines), and allocates nothing while unwired, so **a
viewport at render scale 1 carries neither target and neither pass**.
`SceneRenderer::IsPostResolveUpscaleWired()` reports which case a frame was.

**The bloom mask is the scene colour's companion channel and crosses the same boundary — as its own
step.** It has writers on each side: the translucent pass (and the scene-placed ribbon and sprite
passes after it) rasterizes it into the render allocation beside the lit colour, and at the
post-resolve allocation the post-resolve ribbons and a `SceneHdrPreBloom` overlay whose composite
material declares `"bloomMask": true` add amplitudes to it. So the mask is resampled from the
rasterized sub-rect across a promoted, post-resolve-sized mask at the same boundary
(`PromotionSource::BloomMask`, the same shader); the post-resolve writers then load *that* and bloom
reads it with an identity map, exactly like the scene colour beside it. The mask needs no content to
exist: it is imported whenever the bloom sweep is wired and its latch is scale-driven, so a frame
whose only mask writer is a post-resolve ribbon finds it already cleared (or promoted) underneath.

**Its latch is separate from the colour's, and its pass follows from both.** The colour promotion
asks whether the *finished scene colour* covers the post-resolve allocation; the mask asks whether
the *rasterized sub-rect* does — and nothing reconstructs the mask, so under TAAU, where the
temporal resolve is the colour's promotion and no spatial pass is wired at all, the mask still has to
be carried. So: **when both latches are set, one `SceneUpscaleScenePass` carries both as two
attachments** ("Scene And Bloom Mask Upscale", `scene_upscale.frag`'s `fsPair`, each target
resampled through its own mapping exactly as its own pass would); when only the mask's is, it is a
pass of its own ("Bloom Mask Upscale", against a `BloomMaskFormat` pipeline); when only the
colour's is, the colour's pass carries it alone. Both latches share
`PostResolveUpscaleIdleFrameLimit` and `UpdatePromotionLatch`; the mask's additionally requires a
wired bloom sweep, since with no sweep there is no mask.

**What a reduced render scale buys, and what it does not.** It scales the cost of rendering *the
scene* and nothing downstream of the promotion; the tail is paid in full either way. That is the
intended trade — a resolution slider or controller must not be buying frame time by softening the
interface composited pre-bloom. A dynamic-resolution controller's loop is unaffected in shape: it
still measures whole-frame GPU time and converges, with the scaled portion simply a smaller fraction
of what it measures, so it settles at a lower scale for the same budget.

**The remaining `drsSupported` exclusions are about the sub-rect, not the allocation.** SSR and the
GPU hi-Z occlusion test are not sub-rect-aware and force the per-frame render scale to 1; they still render into a *reduced render allocation* perfectly well, so a static
render-scale reduction reaches them. A composited depth-of-field chain joins them **only behind a
temporal resolve**: that resolve reconstructs the whole render allocation, so the chain would be
reading allocation-resolution scene colour through the sub-rect map its depth reads need. Without a
temporal resolve its five stages and the scene colour are all the sub-rect and it composes.

**What must stay at the render sub-rect keeps its own map, and that asymmetry is now permanent.**
A shader that samples the g-buffer maps a **logical** screen UV through the view constants'
`RenderScaleUV`/`MaxValidUV` (`ScaledSampleUV`), whose denominator is the *render* allocation — so
every such read is correct whatever resolution the pass itself runs at. The explicit maps beside it:
`PostProcessEffectScenePass` derives `DepthScaleUV` against the render allocation while
`SceneScaleUV` is the identity over the post-resolve one. The bloom bright-pass is **not** one of
these: its mask input is promoted to the scene colour's own allocation, so it reads the mask through
the same `SourceScaleUV`/`SourceMaxUV` as the colour. Bloom as a whole runs downstream of the
promotion, so the sub-rect never reaches it and neither kernel restricts the render scale.

#### Temporal upscaling (TAAU)

**TAAU is the temporal resolve reconstructing the post-resolve allocation directly**
(`AntiAliasingMode::TAAU`). It reuses the entire `TAA` path above — jitter, the lit target, the
resolve, the history-copy — and the difference between the two is now **only where the resolve's
output lands**, because the two allocations exist for every mode:

- **TAA** resolves at the **render** allocation. Its history is render-allocation-sized, it
  reconstructs what the scene rendered, and the spatial promotion then carries that up to the
  post-resolve allocation. The reconstruction is temporal; the upscale is spatial.
- **TAAU** resolves at the **post-resolve** allocation. Its history is allocation-sized, the current/
  depth/velocity reads map into the render sub-rect exactly as under TAA, and **the resolve is the
  promotion** — `IsPostResolveUpscaleWired()` is false, and no second resample of the colour
  follows. The bloom mask still takes its own promotion (above): nothing reconstructs it. The
  reconstruction *is* the upscale, which is the whole point: a jittered sub-native render
  accumulated into a native image beats a bilinear tap.

So the render scale is spent the same way in both (it sizes the render allocation, or rides in the
sub-rect under dynamic resolution) and the viewport no longer knows the mode at all — the branch that
pinned its allocation to native under TAAU is gone, along with the `Configure` resize debounce it
needed. `ResolveTemporalUpscalePromotes` (`FrameTopology.h`, a pure function of the settings) is what
decides: it is TAAU **unless** the frame also wires SSR or a composited depth-of-field chain. Those
two sit between the temporal anchor and the tail and read render-resolution depth beside the scene
colour, so a resolve that promoted at the anchor would hand them allocation-resolution colour; such a
frame resolves at the render allocation like TAA and takes the spatial promotion instead. That
degrades the reconstruction from temporal to spatial and keeps every other property, where the same
combination previously gave up scaling altogether.

#### Spatial anti-aliasing — FXAA and CMAA2

**FXAA and CMAA2 are post-tonemap fullscreen resolves** (`AaResolve`, `src/Renderer/AaResolve.{h,cpp}`,
off by default). Both read the **tonemapped LDR** the tonemap writes: under either mode the tonemap
writes an owned LDR intermediate (`AaResolve` allocates it only when a spatial mode is active) instead
of the output, and the resolve reads that and writes the output — inserted after the tonemap and
before the debug-draw pass, so gizmos composite over the resolved scene. Both run at the **full
allocation** resolution on the already-upscaled LDR, so they compose with dynamic resolution and need
no sub-rect awareness (unlike TAA, they do not force full resolution). The tonemap output is **linear**
(the swapchain composite does the display encode), so both compute a cheap perceptual (`sqrt`) luma
internally rather than keying on linear values.

- **FXAA** (`fxaa.frag.slang`, one pass) is Lottes' quality-path luma-directed edge blur: a 3×3 luma
  neighbourhood finds the edge, an end-of-edge search places the blend, and a sub-pixel term catches
  thin features. `FxaaScenePass` is the pass.
- **CMAA2** (`cmaa2_edges.frag.slang` + `cmaa2_apply.frag.slang`, two passes) is Intel's conservative
  morphological AA, implemented as **two fragment passes** rather than the reference compute /
  deferred-blend-list design: the edge pass writes an `RG8` edge map (right/bottom edges) using
  local-contrast-adaptive detection, and the apply pass (`Cmaa2ApplyScenePass`) follows each silhouette
  to its ends and blends across it by the shape's morphological coverage, leaving every non-edge pixel
  untouched. The edge pass reuses `FxaaScenePass` (same texture+sampler+rcp push) with the edge
  pipeline. The `RG8` edge map is a second owned target allocated only under CMAA2.

`AaResolve` owns the vertical slice (the LDR intermediate + its bindless slot, the CMAA2 edge map, and
the FXAA/edge/apply pipelines), releasing its handles in its own destructor — the `TaaResolve`
precedent. Nothing is allocated until a spatial mode is active, so the shipping path holds no extra
memory and the smoke golden is unmoved.

#### Supersampling (SSAA) is the render scale above 1

SSAA is not an `AntiAliasingMode` — it is the viewport allocating **above** its region resolution and
the gather/composite tail box-downsampling on the way back. **It rides `MaxAllocationScale`**, the
outer factor on *both* allocations, so the scene and the whole post-resolve tail supersample together
and a linear-filtered lookup averages the result down into the region — a proper 2×2 box at
exactly 2× (the standard SSAA factor), a bilinear approximation at other factors. `tests/gpu/viewport.cpp`
pins the supersample allocation. It stacks with every AA mode including `TAAU`, since the render scale
is a separate lever from the ceiling: `MaxAllocationScale = 2` with `RenderScale = 0.5` supersamples
the tail while the scene renders at the region's own resolution, and the temporal resolve reconstructs
the 2× image from it. The downsample happens in whichever pass samples the viewport — the gather's
blit, or the swap-chain composite when that viewport alone covers the window and the gather is
skipped (below) — through the same linear, clamp-to-edge lookup. `Viewport::SetRenderScale(scale)`
with `scale > 1` still grows the *render* allocation above the post-resolve one — the scene renders
supersampled and the promotion's bilinear tap is the 2×2 box at exactly 2× — but it leaves the tail
native, so `MaxAllocationScale` is the lever that supersamples a frame end to end.

### Shadows: directional cascades + the punctual atlas

Directional lights are shadowed by **cascaded shadow maps**, and a **bounded set of punctual
lights** (`MaxShadowedPunctual`) by a **shared punctual shadow atlas**. The directional cascades
split the camera frustum into depth slices, each cascade fit (bounding-sphere + texel-snapped) to
its slice and rendered into a depth **atlas** in **one** pass (per-cascade viewports); the
lighting pass selects the cascade by the fragment's view-space depth, remaps to the atlas tile,
and **`SampleCmp`s** it through a **hardware comparison sampler** with a boundary cross-fade.

**The cascade atlas carries `MaxCascadeSets` sets, not one.** A set is one near-parallel source's
cascades, fit to that source's direction; sets stack as further **row bands** of the same atlas, so
a scene lit by two comparable distant suns shadows from **both** rather than from whichever the
world iterated first. The atlas is always sized for the full set budget, so a second source needs
no reallocation and a scene with one leaves the upper band at its clear. Two is the number because
a set is the atlas's expensive unit — at the default 1024² tile and four cascades one set is a
2048² D32 atlas (16 MiB) and each further set costs another 16 MiB *and* a full re-traversal of
every caster through four more cascade viewports, where a punctual tile costs a sixth of the
memory and one traversal.

**A view packs only the lights that can light it, and past the cap the brightest.** One view
carries `MaxLights` (16) lights, and every pixel the lighting pass shades visits each of them its
tile has not culled, so a slot spent on a light that adds nothing costs pixels across the screen. `PackSceneLights` skips a light of zero
radiance, a positioned light of non-positive range, an area light whose emitter has no area, and —
given the camera frustum — a positioned light whose range sphere (grown by its emitter's reach)
misses it. The rest are ranked by the radiance each delivers at the camera position (inverse square
from the emitter's surface, clamped one world unit out; deliberately without the range cutoff, which
would score zero every light whose range ends short of the camera but not of what it sees), the top
`MaxLights` are packed in scene iteration order, and the excess is counted in `DroppedLightCount`.
**The loop skips what cannot light a pixel** (`EvaluateDirectLighting`, `Veng/lighting.slang`): a
light of zero radiance, a pixel at or past a light's range (for an area light, tested before the LTC
integral), a pixel outside a spot's cone, and a punctual or directional light behind the surface
each return before the shadow lookup and the BRDF. Each skip is exact — the term it skips is
zero — so it changes cost, never the image.

**Before the loop, a tile cull drops the lights that cannot reach a tile at all**
(`Settings.LightTileCulling`, on by default; `LightTileCuller`, `light_tile_cull.comp`). One compute
workgroup per `LightTileSize` (16) pixel tile — the "Light Tile Cull" pass, declared by the lighting
pass ahead of itself — reduces the tile's depth to its nearest and farthest geometry, bounds the
slice of the tile's frustum between them by the view-space box around its eight corners, and writes
one `u32` per tile: bit *i* set when the view's *i*-th packed light's influence sphere reaches the
box. The lighting pass (`EvaluateDirectLightingMasked`) then visits only the set bits, in index
order, so a full mask sums exactly what the unmasked loop does. **The bound is conservative, so the
image never moves:** a shaded pixel reconstructs its position at a pixel centre strictly inside the
tile's edges and at a depth inside its range, so it lies in the box; a point or spot light lights
nothing past its range (a spot stays sphere-bounded, its cone untested); an area light's sphere is
its range grown by its emitter's reach (a Sphere's radius, a Rect's or Polygon's farthest vertex
from its position), since its cutoff is measured from the emitter's surface; and the sphere grows
by a `1e-5` relative epsilon over the camera-relative magnitudes to cover the float error of the
lighting pass's own world-space reconstruction. A **directional light has no range and is in every
tile's mask**; a tile showing only sky (cleared depth) gets no positioned light. Which arms cull is
`FrameTopology::LightTileCullActive`: every arm whose lighting pass shades direct light (Final, Bloom,
Reflections, CoC), never the cascade-tint or IBL-only variants that discard it.

The masks live in a device-local storage buffer with **one region per frame in flight** (a renderer
executes once per frame, so a frame never writes a region an in-flight frame reads), laid out
row-major over the tile grid of the render allocation, so a dynamic-resolution sub-rect needs no
reallocation. The cull writes through a set binding its frame's region alone (one set per frame in
flight, the region stride rounded up to the storage-buffer offset alignment), so its write is scoped
to the words that frame owns — which is also the range synchronization validation checks it over.
The buffer is registered in the set-0 storage-buffer array, and the view block's
`LightTiles` names it — slot, row stride, this frame's first word, or `LightTilesNone` when no cull
ran — so any pass can find a pixel's mask through `Veng/light_tiles.slang`. It is imported into the
graph: the cull's `StorageBufferWrite` and the lighting pass's `StorageBufferRead` (which, on a
graphics pass, is `AccessKind::StorageBufferReadGraphics`, a fragment-stage scope) derive the barrier.
**The masks describe the opaque depth range**, so they serve the deferred pass; a forward-lit
translucent fragment in front of that depth is not covered by them and still loops every light — a
forward consumer would want a second word per tile bounding the near plane to the opaque depth.
`SceneRenderer::ReadbackLightTileMasks()` downloads the last frame's masks for tests and diagnostics,
and the toggle stays a setting so the two costs can be compared in a capture.

**Both budgets are spent by estimated contribution, never by arrival.** `PackSceneLights` scores
every packed shadow-casting light by the radiance the lighting pass would apply at the point of the caster
bound nearest it — a directional's unattenuated radiance, or a punctual light's radiance under the
shader's own range falloff and inverse square, clamped at its value one world unit out — then walks
the ranking from the top, handing out cascade sets and atlas slots. **Equal scores keep scene
iteration order** (a stable sort), which makes the ranking a total order that does not move between
frames, so two equal lights cannot trade a shadow. A directional the cascade budget cannot seat is
packed with the `CascadeDenied` flag, counted in `DeniedDirectionalCount`, and warned about once per
renderer: it shades unshadowed and says so, rather than borrowing a cascade fit to another light's
direction. A near-parallel *area* light denied a set falls back to its own perspective tile.
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

**A shadow view draws only what it can show.** `PackSceneLights` takes the camera frustum: a
light whose range sphere misses it is not packed at all, so takes no slot, and a point
light's cube face whose frustum misses it is left out of `PunctualFaceMask` (a visible pixel samples
the face its direction from the light falls in, so that face is never sampled) — its tile keeps the
clear. A frame where no light took a cascade set renders no cascade tile. Within a cascade, a caster
whose world bound spans fewer than `ShadowCasterMinTexels` texels is skipped; the far cascades are
where that trips. `FrustumCull` off packs every light and renders every slot, face and caster. `GetLastShadowViewCount()`
reports the views rendered (the `Render/ShadowViews` counter).

**Depth passes draw instanced.** `PrepareDraws` writes every visible mesh's world and normal matrices
once per frame into a `CasterRecordRing` (`DepthInstancing.h`), record *i* belonging to
`SceneView::Visible[i]`. A depth pass's static casters go through a `DepthInstanceBatch`, which
groups them with `DepthCasterGrouping`: every view opens itself and adds the casters it keeps, a
caster any view keeps joins **one shared list** and the view only sets its bit in that caster's
mask, and `Build` orders the shared list once with `SortDrawKeys` (no pipeline in the key), cuts it
once with `GroupContiguousSlots`, and walks it once to write every view's instance ids, view after
view, into one per-frame buffer. A view's ids keep the shared order, so its draws are exactly the
ones its own casters would group into alone (`tests/unit/depth_caster_grouping.cpp`), and the pass
records one instanced draw per submesh per view at the view's offset. The ids ride per-instance
vertex binding 1 as `a_CandidateId` (`Veng/depth_caster.slang` reads the record at set 3), and the
view's view-projection rides the push block, so it changes per view, not per draw. Skinned casters
keep one draw each, posed through `SkinnedPaletteBases`.

**Both shadow passes draw from one set of views, built before the graph runs.** The renderer owns a
`ShadowCasterViews` (`ShadowCasters.h`); each `Execute`, after every rebuild, it opens it, has the
cascade pass add each granted set's cascades and the punctual pass each rendered face
(`AddViews`), and builds it — so the cascades and the punctual faces share one grouping and one
instance-id upload, and each pass records its views at their indices. `AddView` culls the view
through the broadphase tree and applies the cascade's minimum-texel skip; whether a candidate casts
at all (`MeshRenderer::CastsShadows`, an opaque material, a posed skinned mesh) does not depend on
the view, so it is decided once per candidate per frame. The work is scoped `Shadow/Cull` (per view),
`Shadow/Instance`, `Shadow/Upload` and `Shadow/Record` (per pass). The depth+normal prepass keeps a
batch of its own with its one view.

**Both arms are opt-out per light, through `Light::CastsShadows`.** It defaults true, so a light
shadows unless it says otherwise; cleared, the light scores zero and is passed over for every arm.
The reason it exists is that the slots are scarce: only `MaxShadowedPunctual` lights get one and
the rest silently get none, and the contribution ranking does not know intent — a bright light that
wants no silhouette still outranks a dimmer one that does, so a light that does not need a shadow
and cannot say so takes the arm from one that does. The case it is for is a **fill
light** — one standing in for the emission of a surface that is already drawn, or filling a volume
with no occluder worth resolving — which wants its contribution and not its silhouette. Such a
light is also the worst case for the arm it would take: a perspective tile fit to the whole scene
bound has least to spend exactly where a near light needs most.

**A light's specular is scaled per light, through `Light::SpecularScale`.** It defaults to 1, the
physical response; 0 leaves the light diffuse-only. The packed light carries it in its last vec4
(`Response.x`), and `AccumulateLight` multiplies the light's specular by it — every light type, in
the deferred pass and the forward light loop alike — while the diffuse term is never touched. A fill
light wants it for the same reason it wants no shadow: its specular is a reflected image of an
emitter nobody sees, and on glossy surfaces — glass above all — that reads as a bright copy of the
light's shape beside the real, already-drawn surface it stands in for.

**A punctual light's size widens its specular lobe.** A Directional carries `Light::AngularRadius`
(radians, defaulting to the Sun's 0.004675 — a directional light most often stands for one), packed
as its sine into `Area.x`; a Point or Spot reuses its source `Radius` (the same lane, already the
near-field clamp), and `AccumulateLight` takes `sinθₛ = min(Radius / distance, 1)`. `EvaluateLight`
widens the GGX width for that light's `D` alone, `α'² = α² + c·sin²θₛ` with `c = 1/(4(√2 − 1)) ≈
0.6036` (`SourceLobeWidening`), which sets a mirror's highlight half-maximum radius in reflected angle
to the source's angular radius (in the plane of incidence; across it, the half-vector lobe is
foreshortened by the cosine of half the view-to-light angle, as any GGX highlight is); `G` and `F`
keep the surface's `α`. Widening a normalised
distribution keeps it normalised, so the reflected energy is unchanged and no correction factor is
applied. Without it, a full-strength lobe at the roughness floor reflects a sun as a sub-pixel glint.
It is a **small-source approximation** that leaves `L` aimed at the source's centre (no
representative point): accurate for a sun or a bulb, while a source spanning more than a few degrees
should be a Sphere area light, which LTC integrates exactly at any size. The area types never reach
`EvaluateLight`, so their lobes are untouched, and nothing but the lobe reads a directional's `Area`
lane — its reach, tile cull and cascade shadow are unchanged. `tests/gpu/specular_lobe.cpp` pins it on
a mirror plane: a directional highlight's half-maximum radius tracks `AngularRadius` with its
reflected energy conserved, and a point light's highlight grows with its `Radius`.

### The scene-gizmo layer

**Most of what a scene holds draws nothing**, and `Veng/Renderer/SceneGizmos.h` is the one pass
that draws a stand-in for it: `DrawSceneGizmos(scene, debugDraw, groups, style)` accumulates into
a `DebugDraw` like any other consumer, so it is called once per frame the layer should appear and
the renderer's own pass rasterizes it. Pure scene-query plus glm — no device, no asset loads,
nothing retained — which is what makes it unit-testable and what lets the editor and a game's
debug view show the identical layer.

The families are the `SceneGizmo` bits: **Lights, Cameras, Colliders, Sockets, Interaction,
Audio, Probes, Empties, Agents** (the last marking every `BehaviorAgent` whose tree is running, at
the pawn it acts through — read through `BehaviorTree::RootStatus`, never ticked). They are selectable because a scene carrying hundreds of one kind is
unreadable while a different question is being asked of it. `SceneGizmoGroupTable()` pairs each
bit with a name and a description **so a consumer's selection interface is the engine's own list**
— a family added here appears in every consumer's checkboxes with no change on the consuming side,
and `tests/unit/scene_gizmos.cpp` holds the table to covering every family exactly once.

**A light's stand-in is the light's own packed geometry, not a second derivation of it.** A rect
area light's outline is the four corners of `Width × Height` in local XY through the entity's
world matrix — the same construction `PackSceneLights` uses for the area-vertex buffer the shader
integrates — and the same case asserts, point for point, that the two agree through a parented,
doubly-rotated chain. That is the property the layer is worth anything for: an emitter that is not
where it appears to be becomes a visible disagreement rather than a shading mystery.

**Icons are the consumer's and the engine ships none**, matching `DebugDraw::DrawBillboard`, which
takes a bindless slot rather than art. An invalid handle in `SceneGizmoStyle` draws a small
wireframe marker in the icon's place, so a consumer with no icon art still reads every position
and orientation and loses only the pictogram. `SceneGizmoStyle::Pickable` writes each billboard's
entity pick id, which is what makes an icon selectable in an editor viewport and is inert in a
viewport running no picking pass.

**The shadow set is a general shadow system**, not just the directional one: the directional cascade atlas,
the punctual shadow atlas, a **shared** immutable comparison sampler (hardware `SampleCmp`), the
per-frame `ShadowConstants` block (per cascade set: the matrices, splits, texel sizes and depth
ranges; plus the shared params) bound as a **dynamic uniform**, and the `PunctualShadowBlock` (the per-light shadow records — view-proj(s),
tile rects, type) ringed beside it. The whole set is held **off the set-0 bindless registry**,
where a comparison sampler mistranslates inside the Metal argument buffer on MoltenVK and a closed
producer→consumer resource needs no global registration. The set-0 view-constants block stays
trimmed to material-facing camera/view state. A `GpuLight`'s shadow **slot** (an index into the
punctual record array, or `-1` for unshadowed) rides `Cone.z` and its **flags word** — the
two-sided bit, the area-light cascade arm, the cascade-set index and the cascade denial — rides
`Cone.w`, keeping `LightStride` fixed; the bit meanings are `Renderer::LightFlags` and its shader
mirror `Veng/light_flags.slang`. `CascadeCount`, `CascadeSplitLambda`, and `ShadowResolution`
(default 1024) are the directional CSM knobs; `MaxShadowDistance` and `MinShadowDistance`
bound the fitted cascade range at each end (the far cap, and the near-side mirror that keeps
the fit from collapsing when a camera renders a huge depth range with a tiny reverse-Z near —
0 on either leaves that end at the camera plane); `PunctualShadows` (the on/off toggle) and `PunctualShadowResolution` (the
per-tile edge length) are the punctual knobs; `DebugView::Cascades` tints each fragment by the
cascade it selects and `DebugView::PunctualShadows` blits the punctual atlas. **A Translucent submesh casts no shadow.** Both shadow passes gate each candidate on
`Renderer::CastsShadow` (`src/Renderer/DrawGather.h`): a resident material that is not
`MaterialDomain::Translucent`. The domain writes no opaque depth, is drawn after the lighting it
would have to occlude, and is documented as never occluding another translucent, so rasterizing a
solid shadow from it contradicts every other way it behaves. Alpha-cut and stained-glass casters are
a separate capability — both need the shadow pass to *shade* rather than to rasterize depth. This
per-light shadow cull is the **prime consumer of the BVH broadphase** — one tree queried many times (`N`
spot frustums + `6N` cube faces per frame, on top of the camera and cascade queries).

### The refraction grab, and reading it blurred

**`Settings.Refraction` copies the lit scene ahead of the translucent pass**, into a full-extent
`HdrFormat` colour target and an `R32Sfloat` opaque-depth target beside it, and publishes both in the
view block's `SceneColor` slot. A Translucent fragment reaches them through
`Veng/translucent.slang` — `SampleSceneColor` for what is behind it, `SampleSceneDepth` to reject a
distorted sample whose geometry stands in *front* of the refractor. The copy predates the translucent
pass, so one translucent surface never refracts another.

**The grab runs only on a frame something samples it.** A material whose fragment reads it declares
`"readsSceneColor": true` in its `.vmat.json` (Translucent domain only, a cook error elsewhere;
`Material::IsSceneColorReader`), and the gather sets a per-frame flag when any draw in the
translucent or half-resolution plan comes from such a material. The copy, every halving pass and the
coarse-tail dispatch skip every other frame (`SkipWhenUnread`, below), and the view block's
`SceneColor.z` is patched to 0 after the gather, so `SceneColorAvailable` is false that frame and a
fragment that samples without declaring reads black — never a stale grab, nor a level left in an
undefined layout. Most frames of most scenes draw no refractor, and pay nothing for the setting.

**`Settings.RefractionBlur` gives that copy a mip chain**, so the same fragment can read the scene
behind it *blurred* — `SampleSceneColorBlurred(vc, uv, blur)`, with `blur` in [0,1] across whatever
chain the frame has rather than in texels, so a material authors an appearance that holds at any
resolution and any render scale. Frosted glass, ground glass, a backdrop blur behind an interface.

- **A halving chain rather than a wide kernel**, and the reason is what is usually behind glass. A
  handful of taps blurs a smooth image acceptably and turns a field of small high-contrast points —
  a star field, a city at night, specular glints — into visible duplicates of each one. A chain
  averages every texel exactly once per level, so a point spreads instead of repeating. It is also
  far cheaper at a wide radius: the whole chain costs about a third of one full-resolution pass, and
  the radius is then free.
- **A chain averages locally, which is the thing it is.** A level covers `2^level` fine texels and no
  more, so light reaches a neighbourhood rather than the far corner — the coarsest level holds
  roughly an 8-pixel edge, which at 1080p is a very wide blur and is still not a global one.
- **Graph resources, not a manual barrier sweep.** Each level is its own `Import`, written by its own
  fullscreen halving pass and sampled by the next, so the transitions between them are the graph's.
  Level 0 *is* the grab, so it is that id rather than a second import of it. **The coarse tail is
  one compute dispatch** (`SceneColorDownsampleTailScenePass`, `scene_color_downsample_tail.comp`):
  the smallest levels that fit one workgroup's shared memory (`MipTailFirstLevel`) are halved in
  order by a single workgroup, each exactly as the per-level pass would — clear outside the sub-rect
  included — so the image carries `Storage` usage whenever the chain has such a tail.
- **Every level renders the sub-rect its parent occupied, halved**, and clamps its reads inside the
  parent's valid region — the copy's dynamic-resolution discipline applied at every level, so the
  cleared area outside the sub-rect never works its way inward. The valid fraction is therefore the
  same at every level and one sub-rect mapping serves a sample at any of them; the *clamp* still
  widens with the level, because a coarse texel covers `2^lod` fine ones.
- **The chain carries its own sampler.** The renderer's shared g-buffer sampler has the default
  `MaxLod` of 1, which would silently pin every blurred sample to the top two levels; the grab's
  sampler lifts it with `LodClampNone` and filters *between* levels, so a material sweeping its blur
  crossfades rather than stepping.
- **Off by default and separate from `Refraction`**, because the levels are generated whether or not
  anything reads them, and a material that only distorts its samples needs none of them. With the
  setting off the chain is one level and `SampleSceneColorBlurred` returns the sharp copy at any
  `blur` — a degradation rather than a read of a level that was never generated.

**Replacing what is behind a surface is the fragment's job, not the blend's.** The blend can only
reach the sharp scene already in the target, so a fragment wanting a *different* backdrop fetches it,
composites itself over it, and emits at full coverage. At `blur` 0 that is the surface over the scene
it was already over, which is what lets a material mix the two states and fade the effect in with no
seam and no second pipeline.

### The half-resolution translucent layer

**A Translucent material may opt into rendering at half resolution** — `"resolution": "half"` in
its `.vmat.json` (default `"full"`; Translucent-domain only, and exclusive with `bloomMask`, both
cook errors) — and the renderer then routes its draws into a **reduced-resolution layer** instead
of the full-resolution translucent pass. The trade is a quarter of the fragment cost against a
softened image, which a smooth, screen-filling volumetric surface (an atmosphere shell, a fog
bank) makes gladly and a crisp-edged surface (glass with geometry detail behind it) does not — so
it is authored per material, never a renderer setting.

The layer is **content-driven, the volume-field model**: the translucent gather routes each draw
by `Material::IsHalfResolution()` into its own plan, and the renderer activates the layer the
first Execute that plan is non-empty (that frame's routed draws fold back into the full-res plan
— rendered full-resolution once, correctly sorted, never dropped — while the activation edge
allocates the targets and rebuilds the pass set) and drops it only after the gather has stayed
empty for `HalfResTranslucentIdleFrameLimit` consecutive Executes — deactivation hysteresis,
because a capture probe renders one cube face per frame and a scene whose opted-in material sits
in some faces and not others would otherwise recompile the graph every frame. An idle wired
layer draws nothing on an empty plan: the depth reduce, the layer pass and the composite all skip
that frame together — the composite under `SkipWhen`, and the two producers it alone reads under
`SkipWhenUnread`, since they clear their targets — so the window costs only the targets' memory. A
renderer that never sees an opted-in material carries no targets and no passes. `HalfResTranslucency`
(`src/Renderer/HalfResTranslucency.h`) owns the vertical slice; `HalfResExtent` is the one
rounding rule every consumer derives the half extent through.

Three passes, wired immediately ahead of the full translucent pass in both compositing arms so
the layer composites **under** every full-resolution translucent draw (a cockpit pane over an
atmosphere shell):

1. **A depth reduce** writes a half-res D32 target as the farthest (reverse-Z minimum) of each
   texel's 2×2 full-res opaque depths, so the layer's draws depth-test conservatively — a
   fragment survives wherever any of its four full-res pixels would show it.
2. **The layer's own `TranslucentScenePass`** (the same class, its half-resolution option):
   half-extent viewport into a half-res HDR target cleared to transparent, the same back-to-front
   sort, per-parent pipelines, and straight alpha blend — which over a transparent clear leaves
   exactly (premultiplied color, coverage). No bloom-mask attachment: the layer lands in the lit
   color ahead of the bloom bright-pass, so its glow rides the scene like an unmasked
   translucent's.
3. **A depth-aware composite** upsamples the layer to full resolution — each 2×2 tap's bilinear
   weight collapsed in proportion to its reduced depth's distance from the pixel's own opaque
   depth, so the layer hugs geometry edges instead of haloing across them — and blends it into
   the lit scene color with `(One, OneMinusSrcAlpha)`.

**The layer renders through a second view-constants region.** The one block field a half-res
fragment must read differently is `ExtentParams`, which carries the half extents so an
`sv_position` mapped through it lands on the same UV a full-resolution draw's would — every other
field (matrices, `RenderScaleUV`, the refraction-chain handles) is the full view's, so UV-space
reads like `SampleSceneColor` work unchanged. The region is claimed **before** the render's own
slot, because every pass reading `GetCurrentViewConstantsIndex()` at record time must land on the
full region; a frame whose view budget refuses the extra claim folds the layer's draws back into
the full-res plan for that frame, the same fallback as the activation edge.

### Additive translucent materials

**Translucent draws order by priority group, then back to front within a group.** A draw's priority
is its material's `sortPriority` plus its entity's `MeshRenderer::SortPriority`: the material's is a
statement about every surface drawn with it (an overlay over everything), the renderer's the same
statement about one entity — glass a viewer always sits behind, which is nearer than everything
translucent around it however its centre sorts — so one shared material need not be split into a
second parent to order one use of it. Within a group the key is each submesh's own view-space centre.

**A Translucent material chooses how its colour composites: `"blend": "alpha"` (the default) or
`"additive"`** in its `.vmat.json` (Translucent-domain only, a cook error elsewhere), carried to
`Material::GetTranslucentBlend()` as a `TranslucentBlend`. `TranslucentScenePass` builds an additive
material's pipeline with `BlendState::AlphaAdditive()` — `src·a + dst`, destination alpha kept — so
the fragment still returns straight colour and coverage and the coverage scales how much light it
adds. An additive surface needs no order among its own kind, but it still draws in the one
back-to-front sort the pass keeps, since it shares the pass with alpha surfaces it may sit behind.
In the half-resolution layer the kept alpha is what matters: an additive draw adds colour into the
transparent-cleared layer without claiming coverage, so the composite's `(One, OneMinusSrcAlpha)`
adds it onto the scene.

### Flipbook sprites

**A `FlipbookSprite` component draws a flipbook as a camera-facing HDR quad** (the component and the
`Flipbook` asset are in [../Scene/CLAUDE.md](../Scene/CLAUDE.md) and
[../Asset/CLAUDE.md](../Asset/CLAUDE.md)). `SpriteScenePass` (`Passes/SpriteScenePass.h`) is wired
**after the full-resolution translucent pass and the ribbon pass that follows it**, in both
compositing arms, into the same lit target and bloom mask — so sprites composite over translucent
surfaces and ribbons, resolve under TAA with the scene, bloom, and tonemap.

- **Content-driven with deactivation hysteresis.** `GatherSprites` walks `View<FlipbookSprite>` each
  `Execute` (skipping a sprite whose flipbook or atlas is not resident, whose opacity is zero, or
  which has finished) into a `SpriteDrawPlan`; the pass is wired the first `Execute` that gathers
  one and unwired only after `SpriteIdleFrameLimit` consecutive empty gathers, because transient
  effects leave a scene momentarily empty between bursts and a recompile per burst is the cost the
  hysteresis avoids. A scene that never carries a sprite has no pass, so the smoke golden is
  unaffected.
- **One record per sprite, no vertex input.** Each gathered sprite is an 80-byte `GpuSprite` (anchor
  position and width, tint and opacity, the frame's atlas rectangle, the anchor fraction, height,
  roll, the atlas's bindless texture and sampler, its alpha mode) in a host-mapped ring, one region
  per frame in flight bound through a per-frame set at set 3. The vertex stage indexes the record by
  `VertexIndex / 6` and places the quad corners about the anchor along the camera's right and up
  axes, rolled in the screen plane, projected through the jittered `Proj`.
- **Two draws.** The alpha set, sorted back to front on view-space depth, draws first through
  `BlendState::PremultipliedAlpha()`; the additive set follows, unsorted, through a `One, One` blend
  that keeps destination alpha. The fragment brings each atlas alpha convention (premultiplied,
  coverage, luminance, opaque) to premultiplied colour before the tint and opacity apply, so one
  stage serves both blends.
- **The bloom mask is written by luminance.** When the frame wires a mask (bloom on), the masked
  fragment variant adds `saturate(luma(colour))` into it, so a bright sprite glows whatever the
  bloom threshold; the translucent pass ahead of it cleared the mask, and this pass loads it.
- **Budget: `MaxSpritesPerFrame` (4096).** Past it the rest are dropped for that frame and the
  renderer warns once for its lifetime. Depth is tested against the opaque depth without being
  written, and there is no soft-particle depth fade.

`tests/gpu/sprite_pass.cpp` renders a white premultiplied flipbook through each blend and checks the
composited centre pixel against the tint, the additive sum, and the bloom-masked path.

### Ribbons and trails

**`Ribbon`, `Trail` and `RibbonPath` components draw as camera-facing HDR bands** (the components,
`RibbonSystem` and `SpawnTransientBeam` are in [../Scene/CLAUDE.md](../Scene/CLAUDE.md), "Ribbons and
trails").
`RibbonScenePass` (`Passes/RibbonScenePass.h`) is wired **after the full-resolution translucent pass
and immediately ahead of the sprite pass**, in both compositing arms, into the same lit target and
bloom mask. **Why ahead of sprites:** a beam or a trail is a long element that sprite effects stand
on — a flash at a muzzle, a burst at an impact, a puff at a trail's head — so an alpha sprite
composites over a ribbon rather than under it; additive content is order-free either way. Neither
pass writes depth, so neither occludes the other.

**A `RibbonPath` chooses its placement; ribbons and trails are always scene-placed.**
`RibbonPlacement::Scene` (the default) is the pass above. `RibbonPlacement::PostResolve` draws the
same bands through a second instance of the pass, **"Post-Resolve Ribbons"**, declared at the HDR
tail anchor **after the post-process effects and immediately before the pre-bloom GUI overlay** (so
a nearer overlay composites over it, and bloom reads both), into the effect chain's output at the
**post-resolve allocation**, in the Final arm only (a debug arm has no pre-bloom scene-colour chain,
as for the overlay). It exists for crisp world-space linework — gizmos, projected orbits, wireframe
holograms, measurement guides — that the temporal resolve would soften and shimmer and the upscale
would resample: it projects through the camera's **unjittered** projection (pushed, since the view
constants carry the jittered one), floors its width in post-resolve pixels, and is never resolved or
resampled. What differs from the scene placement:

- **Occlusion is per fragment, by discard.** The post-resolve allocation has no depth buffer, so the
  fragment samples the render-allocation g-buffer depth at its pixel through the sub-rect remap
  (`ScaledSampleUV`, as the point-field and debug-draw fades do), turns it back into a view depth
  through the projection's depth rows (which the TAA jitter leaves alone, so the unjittered matrix
  serves), and **discards** a fragment farther than it by more than a 0.1 % relative tolerance. A
  discard rather than a fade matches the scene placement's depth test, so moving a path between
  placements changes its sharpness, not what hides it. The silhouette it is cut against is the
  depth's, so at a reduced render scale an occluder's edge is as coarse as the render allocation.
- **It writes the post-resolve bloom mask** (the promoted one when the mask crosses the boundary, the
  render allocation's when that already is the post-resolve size), by the same luminance rule.

**A path may draw through the scene (`RibbonPath::Occluded` false)**, in either placement — a hologram
or a gizmo that must stay readable inside the geometry it stands in. `GatherRibbons` splits each plan's
alpha and additive sets again by it, and the pass draws the unoccluded sets after the occluded ones:
the scene placement through pipelines without its depth test, the post-resolve placement with its
fragment discard switched off by the push block's `Occluded` word.

- **Content-driven with deactivation hysteresis**, exactly the sprite pass's shape: `GatherRibbons`
  runs every `Execute` into one `RibbonDrawPlan` per placement, each pass is wired on the first
  non-empty gather of its own plan and unwired after `RibbonIdleFrameLimit` consecutive empty ones,
  so a scene with no post-resolve path never wires the tail pass, one with only scene-placed content
  is unchanged, and a scene that never carries any has no pass (the smoke golden is unaffected).
- **One record per segment or dot, no vertex input.** A `Ribbon` is one segment; a `Trail` is a
  segment per consecutive pair of its samples plus one to its entity's drawn position while
  `Emitting`, so a trail contributes **at most `MaxSamples` segments**; a `RibbonPath` strip of N
  distinct points is N − 1 segments open, N closed, and a strip of **one** distinct point (all its
  points coincident counts) is one **dot record** — a disc of diameter `Width`, the polyline analogue
  of a round cap on a zero-length subpath. A dot rides the segment record with both ends on its
  centre and `StartTangent.w` set to 1; the vertex stage spans it with a square parallel to the image
  plane (so the disc projects to a circle), floors it at the same pixel width with the same opacity
  compensation, and the fragment's `(1 − r²)²` falloff over the disc coordinate is the band's
  cross-section turned about its centre, so the dot is full at its centre and anti-aliased at its
  rim. A trail of one point still draws nothing. Trails and strips run through one joiner
  (`JoinStrip`): coincident
  consecutive points merge, and each point's tangent is taken across its neighbours — wrapping on a
  closed strip, falling back to the segment's own direction at a hairpin whose neighbours coincide
  — so the two segments meeting at a joint share its edge and a curve draws without gaps or notches,
  the closing joint included. Each 96-byte `GpuRibbonSegment` carries both ends' positions, widths,
  colours and opacities and the per-end tangents. The records sit in a host-mapped ring sized at the
  budget below, one region per frame in flight at set 3, as the sprite pass's do — so a frame's
  ribbons, trail samples and path segments all draw from one fixed region and never grow it.
- **A path stands where its entity's meshes draw.** A `RibbonPath`'s local points go through its
  entity's *drawn* world transform — interpolated by the view's alpha while the scene carries motion
  history, the current pose at alpha 0, with any `PredictionError` offset applied — the same pose
  the mesh gather resolves, so a path parented under a moving body rides it without sliding. Its
  strip widths scale by the length of the world X axis, as a sprite's size does. A trail's head is
  placed by the same pose.
- **Positions are rebased to the eye on the CPU, in double.** The gather subtracts the camera's
  position from every point before upload and the vertex stage rotates the eye-relative point by the
  View matrix's rotation alone, so no large world coordinate is pushed through the view translation
  — the far-from-origin precision loss `InvViewRotProj` exists to avoid for rays.
- **Camera-facing about its own axis, floored at a pixel.** Each end is pushed out along the
  direction perpendicular to both the ribbon's tangent and the eye ray, so the band turns about its
  axis to face the camera. An end narrower than `MinPixelWidth` (1.5 px) widens to it and scales its
  opacity by the ratio, so a distant thin beam keeps its energy instead of breaking into a
  shimmering dotted line. The cross-section falls off as `(1 − v²)²`, full on the centre line.
- **Two draws, the translucent pass's presets.** The fragment returns straight colour and coverage;
  the alpha set, sorted back to front on each segment's midpoint view depth, draws through
  `BlendState::AlphaBlend()`, and the additive set through `BlendState::AlphaAdditive()` (coverage-
  weighted, destination alpha kept) — no blend of its own.
- **The bloom mask is written by luminance** of the contributed light (`colour × coverage`) when the
  frame wires one, so an HDR beam glows whatever the threshold.
- **Budget: `MaxRibbonSegmentsPerFrame` (8192) records, shared by both placements** (a dot is one).
  Past it the rest are dropped for the frame and the renderer warns once for its lifetime. The scene
  placement is depth-tested against the opaque depth, never written.

`tests/gpu/ribbon_pass.cpp` checks a ribbon's centre-line colour through each blend, a trail
lighting its path, the bloom-masked path, a post-resolve path drawing through its own pass only and
glowing through the post-resolve mask promoted or not, a dot's roundness in either placement, and
(cooker-gated) a post-resolve path hidden behind a cube at full and reduced render scale;
`tests/unit/ribbon.cpp` checks the trail ring's bounds, the stationary trail's empty gather, the
packing bound, a pooled beam's fade and return, a path's segment counts, shared joints, merging,
and placement by its entity's (interpolated) pose, the one-point dot, the routing by placement, and
the shared budget.

### Forward lighting for translucent surfaces

**A Translucent fragment can run the deferred pass's own light loop.** The lighting math — the
Cook-Torrance BRDF, the typed-light loop with its LTC area lights, the cascade / punctual / PCSS
shadow lookups, and the three ambient arms — lives in one shared core, `Veng/lighting.slang`, which
`deferred_lighting.frag` evaluates per g-buffer pixel and `Veng/forward_lighting.slang` per
translucent fragment. A material includes the latter in place of `Veng/translucent.slang` (it
re-exports that contract), fills a `ForwardSurface` (world position, normal, view vector, albedo,
roughness, metallic, occlusion) and calls `EvaluateForwardLighting`, which returns `Diffuse` and
`Specular` radiance apart in the deferred output's exposure-scaled HDR units;
`ForwardWorldPosition(sv_position)` reconstructs the position through the same `InvViewProj` the
deferred pass uses. The split exists because straight alpha weights the whole returned colour by
coverage, and a clear surface is mostly reflection — a material raises its coverage by the
specular term, or divides it out, rather than letting the blend dim both alike.

- **The inputs are the view block's, which is why the light state lives there.** Every consumer of
  a view already reads that block, the half-res layer's region carries its own light bases, and a
  translucent pipeline's push layout is reflected per material — the deferred push could not carry
  the state to it. One producer (`SceneRenderer::Execute`'s pack) feeds both paths.
- **Shadows are supported.** The shadow system and the IBL maps are descriptor sets, not bindless
  entries (a comparison sampler and a cube mistranslate in a Metal argument buffer), so a
  forward-lit fragment declares them at **set 4** (IBL) and **set 5** (shadows, through
  `VE_SHADOW_SET`) — set 3 is the per-draw `DrawData` in a surface-drawn layout, where the lighting
  layout keeps its shadow set. `TranslucentScenePass` recognises a forward-lit material by its
  reflected fragment interface declaring either set (`IsForwardLit`) and builds its pipeline against
  its own `DrawData` layout plus the renderer's IBL and shadow layouts — the reflected shadow layout
  cannot carry the immutable comparison sampler, and only an identically-defined layout accepts the
  renderer's set — then binds the renderer's sets there, with the frame-slot dynamic offsets the
  lighting pass uses. Sets 4 and 5 are therefore reserved to that include in the Translucent domain;
  a translucent material that does not include it binds nothing extra. The pass declares the shadow
  atlases sampled, as the lighting pass does. **The shadow maps hold only opaque casters** (a
  Translucent submesh casts no shadow), so a translucent surface is shadowed by opaque geometry and
  never by another translucent one.
- **What differs from the deferred result.** Screen-space AO is not applied — it is computed from the
  opaque depth, which describes what lies behind the surface — so only `ForwardSurface.Occlusion`
  scales the ambient. Emission is not part of the result. The g-buffer quantises albedo (sRGB8) and
  roughness/metallic (8-bit) where the forward path shades exact floats, so the two agree to that
  quantisation rather than bit for bit; `tests/gpu/forward_lighting.cpp` renders an opaque cube and
  its full-coverage forward-lit twin under a directional and a point light and holds them within it.
- **It must be called in uniform control flow.** `EvaluateForwardLighting` takes screen-space
  derivatives of `ForwardSurface.Normal` for specular anti-aliasing, so a material calls it before
  any `discard` or early return some fragments of a quad take, and outside any branch that varies
  across the quad; derivatives in divergent flow are undefined.
- **The output guard bounds what the loop returns, not what a material writes.**
  `EvaluateForwardLighting` passes `Diffuse` and `Specular` each through `GuardLightingOutput`. A
  material that scales either up — dividing the specular by its coverage, as the straight-alpha
  advice above suggests, multiplies it by the coverage's reciprocal — or sums them with other
  radiance can pass the bound again, so it passes its final colour through `GuardLightingOutput`
  before returning it. The gpu fixture `forward_lit.frag` does, since even its unscaled
  `Diffuse + Specular` can sum past the bound.

### Bloom

**Bloom is a compute mip-pyramid battery**, a fixed engine pass like SSAO and the shadow atlas —
not a PostProcess material. The lit HDR target is bright-passed (a soft-knee `Threshold` with
**Karis-average** firefly suppression on the first downsample, the firefly-stability mechanism
that holds whether or not the optional TAA resolve is on) into a single `HdrFormat` mip-chain
image's mip 0, **progressively downsampled** through the chain, then **upsampled** with an
accumulating dual filter (`mip[i] += upsample(mip[i+1]) * Radius`). **The pyramid begins at half
resolution**: mip 0 is half the post-resolve allocation (floor-halved, `BloomPyramidBase`), so the
bright pass is the chain's first genuine 2:1 downsample and no level is a full-resolution surface.
The chain is the full-extent chain less its finest level (`BloomMipCount`), so its coarsest level —
and the widest glow it reaches — is the same at every extent. Every pyramid-side sub-rect map is taken
over the pyramid's valid base (half the frame's valid extent); only level 0's source, the scene colour
and the mask read with its taps, maps over the scene's. The arithmetic is device-free in
`BloomMips.h`, pinned by `tests/unit/bloom_mips.cpp`. The **tonemap** composites it: it samples mip 0
beside the scene colour, through the extra-input seam of its `PostProcessScenePass`, magnifying it 2×
in one bilinear tap, and adds `mip0 * Intensity` in linear HDR ahead of exposure — so no
full-resolution intermediate holds the sum, and with bloom inactive the intensity is written 0 and
the add is skipped. The whole sweep is **compute**: per-level dispatches with a barrier between
levels, mirroring the hi-Z reduction's mip-chain shape (one image with N mip levels, one single-mip
view per level serving as storage destination and sampled source, a clamp-to-edge linear sampler for
the bilinear taps, per-level descriptor sets, all off bindless). **The coarse tail is one dispatch**
(`bloom_tail.comp`): the smallest levels — the longest suffix, never level 0, that fits one
workgroup's shared memory (`MipTailFirstLevel`, `MipTail.h`) — are down-swept and then up-swept by
a single workgroup that synchronizes between levels instead of dispatching per level, each level
held at the precision its RGBA16F mip stores and filtered by the same kernels with the bilinear taps
evaluated by hand, so the result matches the per-level sweep to within a unit in the last place. **The pyramid exists only while
bloom is active** (`ResolveBloomActive` — the setting on the Final path, or `DebugView::Bloom`): a
renderer with bloom off allocates no chain, and a reconfigure at an unchanged extent keeps it.
The filter kernel is a `BloomKernel { Cod, Kawase }` topology knob — the COD/Jimenez 13-tap-down /
tent-up dual filter (the default and the golden's kernel) or the bandwidth-optimized **Dual
Kawase** filter for the TBDR GPUs veng primarily targets. `Bloom` (on/off) and `Kernel` are
`SceneRendererSettings` topology knobs (a `Configure` recompile); `Threshold` / `Intensity` /
`Radius` are per-frame `SceneView` values — the first and last ride the compute push, `Intensity`
the tonemap's material block — so tuning them never recompiles. `DebugView::Bloom` blits pyramid
mip 0 after the up-sweep — the accumulated bloom contribution the tonemap would add.

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
resident `AssetHandle<EnvironmentMap>` rides the per-frame `SceneView` (resolved by the renderer
from the scene's `Sky` component each `Execute`, never pushed by a consumer); a renderer-owned **`EnvironmentIbl`** helper
(`engine/src/Renderer/EnvironmentIbl.{h,cpp}`) generates the maps it derives — a **radiance
cubemap** (the skybox source), a **diffuse irradiance cubemap**, a **GGX-prefiltered specular
cubemap** (roughness mip chain), and the environment-independent **BRDF integration LUT** — all
through compute (the four `ibl_*.comp` core shaders), mirroring the bloom/hi-Z
compute-with-manual-barriers pattern (per-face/per-mip storage views, cube sampled views, explicit
`PrepareForAccess` barriers, all off bindless). Generation is recorded **once when the bound
environment changes** (a `m_LastEnvironment` gate in `Execute`, into the same command buffer
before the graph runs); the BRDF LUT is generated once on first use. The four sampled maps + a
linear sampler reach the deferred lighting pass as **one dedicated descriptor set bound at set
4** — **off the set-0 bindless registry**, mirroring the shadow-atlas "closed producer→consumer"
precedent (a cubemap in a Metal argument buffer is a MoltenVK risk, and a closed resource needs no
global registration). The lighting fragment replaces its flat hemispheric ambient with
`kD · irradiance · albedo` diffuse + `prefiltered · (F · brdf.x + brdf.y)` specular when an
environment is bound. IBL is a **runtime ambient arm** in the view block (`LightState.w`), not a
pipeline variant: the
set is always bound and valid (the maps are transitioned to a sampled layout at first `Execute`
even before an environment arrives), so a scene **without** a lighting sky falls back to the exact
flat-ambient path and renders unchanged.

**The sky is one component, and every sky source is a radiance-cube producer.** The scene carries
one author-opt-in **`Sky` component** (`Veng/Scene/Components.h`): a **source** (`SkySource`
variant — `EnvironmentSky` an environment map, `AtmosphereSky` the procedural atmosphere,
`MaterialSky` an authored Sky-domain material, `CubeSky` a caller-owned already-baked radiance
cube), an `Intensity`, and a **lighting tier**
(`SkyLighting` — `None` display-only, `SH` a spherical-harmonic diffuse ambient, `IBL` the full
split-sum). The renderer **resolves this component itself each `Execute`** (`ResolveSky`,
`TryGetFirst<Sky>` — the lights model, no consumer mapping call or topology toggle) and recompiles
its own pass set at the frame boundary when the resolved source-kind / tier / bake-mode changes.
Every source produces the **same radiance cube** the skybox samples and the IBL convolution reads,
so **what you see and what lights the scene agree by construction**: an environment is a cube
(equirect→cube), a `MaterialSky` or `AtmosphereSky` in `SkyMode::Baked` bakes to a cube
(`BakedSkyCube`, six fullscreen face renders over a fixed per-face basis + a 1×1 far-plane
stand-in depth, re-baked on the source's dirty signal — amortized through `GeneratedTextureService`
one scissor-clipped **tile** of a face per tick, so the per-frame GPU cost is bounded by tile area
rather than a whole heavy face, the previous cube standing until the new one lands), and both display through the one
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

**A baked cube is a `BakedSkyCube`, and it is ownable so one bake can be shared.** For a
`MaterialSky`/`AtmosphereSky` in `SkyMode::Baked` the resolver owns its own `BakedSkyCube` and bakes
it on the source's dirty signal — the ordinary path. The **`CubeSky`** source instead points the
`Sky` component at a `BakedSkyCube` **a caller owns and bakes itself**: the resolver samples it for
the skybox and derives its IBL/SH from it, but **bakes nothing**. This is the shared-sky path — one
bake serves a main viewport *and* its capture probes (a canopy reflection), and the several worlds a
system spans (two flight regimes), so a world swap costs no re-bake. `BakedSkyCube::GetRevision`
advances each time a fresh bake lands (in the resolver's own cube, or — for a shared cube — another
renderer's), and each resolver re-derives its IBL/SH when the revision it last saw moves; the copy of
a completed bake into the displayed cube (`RecordAmortized`) is idempotent, so several renderers
sharing one cube drive it and the first with a landed bake does the copy. There is no engine-side
registry or content-key dedup: sharing is the caller owning one cube and pointing several `CubeSky`
sources at it — a consumer that wants one sky per some content key owns the cube and the key.

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
matrix is jittered with `Proj`, so it still agrees with what was rasterized. `BakedSkyCube` writes
its face basis into both fields — that basis is already a pure direction mapping with no translation
in it.

### View constants: the ring-buffered set-0 block

Per-view data rides a **ring-buffered view-constants buffer**, not push constants: the
`InvViewProj`/`CameraPosition` (for world-position reconstruction), the view/projection, and the
SSAO view/projection live in a set-0 buffer selected by an index fold (a dynamic-offset descriptor
mistranslates in set 0 on MoltenVK). This buffer is **shared across every `Viewport`** (it lives
in the `Context`-owned `BindlessRegistry`), so it is ringed `framesInFlight * MaxViewsPerFrame`
deep and each `SceneRenderer::Execute` claims its own slot (`BindlessRegistry::TryBeginView`, reset
per frame, and before each `ImmediateCommands` recording made outside a frame, so a host rendering only
through those is not capped at `MaxViewsPerFrame` renders in all): two viewports rendering in one frame write distinct regions rather than the second's
camera clobbering the region the first's draws still read at submit. The shared per-frame light
buffer rings the same way.

**Per-frame shading values ride the block, not a recompile.** `AmbientParams.x` carries the SH
skylight arm's intensity, and `.y` / `.z` the geometric specular anti-aliasing variance scale and
threshold (`SceneView::SpecularAntiAliasingVariance` / `SpecularAntiAliasingThreshold`, clamped to
non-negative), read by `MakeLightingSurface`; `.w` is unused. A view block a pass writes without them
(zero) shades with the widening off.

**The per-material parameter arena is not in this ring** — it rings by frame-in-flight alone, so a
material's block sits at one offset whichever view is recording and a value written between two
`Execute`s of one frame is what *both* viewports' draws read at submit. Ringing it per view would
multiply the arena by `MaxViewsPerFrame`, and would still not cover a consumer-owned instance written
per view from outside the renderer. So a pass writing a **per-view** value into a material — the
recording view's extents, the bindless handle of a target the pass owns per viewport — draws through
a **per-view instance**: `PerViewMaterial` builds one over the shared instance's parent and keeps it
current with `MaterialInstance::CopyParamsFrom`. `PostProcessEffectScenePass` and
`GuiHdrOverlayScenePass`'s composite each hold one; `PostProcessScenePass` is handed a per-renderer
instance by its constructor instead. A pass whose writes derive only from the scene —
`CaptureSurface`'s output handle, centre and orientation — keeps the shared instance, because every
viewport writes the same bytes.

**`MaxViewsPerFrame` (32) is a budget spent against, not a contract.** Its consumers are the
registered viewports (one slot each), one face per driven scene capture, and one slot per amortized
sky-bake tick — a single tile of a face (the display cube fills through `GeneratedTextureService`, so
a dirty sky claims one slot per tick spread across frames, not six in one) — so ordinary content can want more than one
frame holds, and the ceiling is sized by memory the whole
ring pays (`framesInFlight * MaxViewsPerFrame` regions of ~6 KB). The SH ambient tier reads its
coefficients back off the amortized display cube through `AsyncReadback` (a copy, not a view render),
so it claims no view slots of its own — only the display bake's ticks do. `TryBeginView` therefore
**returns false rather than asserting** when the budget is spent, warning once, and each consumer
degrades: `SceneRenderer::Execute` records nothing and its target keeps the last frame's content,
and `ViewportCompositor::RenderRegistered` reserves one slot per registered
viewport before driving the captures at all — so **captures give way before viewports do**, a missing
reflection over a stale window. An over-budget capture set is driven **round-robin** across frames
from a retained cursor (`CaptureRotation.h`, the device-free arithmetic), which costs each map refresh
latency instead of starving whichever captures registered last. Its stride is **720 bytes**. The shadow system's own state — each cascade
set's matrices and splits, and the shared params — rides the shadow set's `ShadowConstants` block
instead, so set 0 stays a lean, material-facing view block (shared by materials, lighting, and SSAO).

**The block carries the view's light state**, not the lighting pass's push: the light-buffer and
area-vertex bases of the region the view claimed, the live light count, the ambient arm
(`AmbientArm` — flat floor, SH skylight, or split-sum IBL, resolved once per view by
`ResolveAmbientArm`), the arm's parameters (the flat floor, the IBL and skylight intensities), and
the LTC LUT handles with the prefiltered cube's mip count, and where the view's per-tile light masks
live (`LightTiles`). Everything that lights a surface in this view reads it from there — the deferred pass and a forward-lit translucent fragment alike (see
[Forward lighting for translucent surfaces](#forward-lighting-for-translucent-surfaces)) — so the two
cannot be handed different lights. The bases are the claimed region's, filled after `TryBeginView`,
so the half-resolution layer's second region carries its own. The deferred push is left with the
g-buffer slots, the shared sampler, and the view-constants index; the typed lights ride a separate
ring-buffered light buffer the lighting core loops over.

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
`SceneBroadphase::Sync`: **only on a frame the scene's spatial version moved** (or a still-loading
mesh became resident) it re-gathers the candidates (the pure `GatherMeshes` pass,
`Veng/Scene/Visibility.h`, over every resident `(Transform, MeshRenderer)` entity — world matrix +
world-space `AABB` + resident mesh) and brings the tree current: a **refit** when the candidates are
the same entities and meshes as before (only their bounds moved), a rebuild when the set changed or
refits have degraded the tree's surface-area cost past `SceneBroadphase::RefitCostLimit`. A static
scene touches the tree not at all and queries a stable one. The gathered list rides `std::span<const VisibleMesh> Visible` on
`SceneView`; `SceneBroadphase::Cull` descends the tree once per view — the g-buffer geometry pass
with the **camera** frustum, the cascaded shadow pass once per cascade of every cascade set with
**each cascade's** light frustum, each punctual light's view with its own — so the many-view shadow workload queries
**one tree, many times** rather than re-scanning the list per view. A query returns exactly the
linear scan's per-submesh survivor set (a node wholly outside a frustum rejects its subtree; a
leaf is accepted on its tight box), so the cull is conservative (an extra draw, never a dropped
visible submesh) and the rendered image is **byte-identical** — only the draw calls issued differ.
The survivors come back in ascending id order without a sort: the descent marks them in a bitset
over the candidate ids (`BVH::QueryBits`) and `Cull` walks its words.

`SceneRendererSettings::Cull` selects how those survivors are submitted. Under **`CullMode::CPU`**
(the default) the renderer records the camera-frustum survivors directly, one instanced draw per
run of equal slots (see `PrepareDraws` above).
Under **`CullMode::GPU`** the same frustum survivors are uploaded to a GPU buffer, a **compute**
pass runs a **hi-Z occlusion test** over each candidate's screen-space AABB against the
**previous-frame depth pyramid** and writes each `VkDrawIndexedIndirectCommand`'s `instanceCount`
(1 for a survivor, 0 for an occluded candidate, which executes as a no-op), and the geometry pass
issues the whole fixed buffer through a single `vkCmdDrawIndexedIndirect` per mesh group. The
compute does **not** re-run frustum culling — the BVH already did; it adds only occlusion. The
hi-Z pyramid is a **min-Z mip chain** (the farthest depth per texel under the engine's reverse-Z)
reduced from the depth target by compute into a
renderer-owned, cross-frame-persisted resource (temporal hi-Z: the test reads last frame's chain,
so a history-invalid frame — frame 0, the frame after a `Resize`/`Configure`, or a large view
delta — is frustum-only, never a stale false-cull). Its coarse tail — the smallest levels that fit
one workgroup's shared memory (`MipTailFirstLevel`) — is reduced by one dispatch
(`hi_z_reduce_tail.comp`), bit-identical to the per-level reduction. (The SSR chain's own max-Z
pyramid takes the same treatment — `ssr_hiz_reduce_tail.comp` over the borrowed tail layouts, from
level 1 on, since level 0 ingests the full-resolution depth through a different footprint — and is
likewise bit-identical: a max over the same footprints.) **The reduction runs only while
the test reads it** — under `CullMode::GPU` with `Occlusion` on (`GpuCullSystem::IsHiZReduced`); any other setting
declares none of its per-mip passes, and the frame that starts reducing again counts as
history-invalid, since the pyramid is allocated afresh. **The pyramid is allocated only while it is
reduced**, about `1.33 × W × H × 4` bytes otherwise held for nothing: entering the reduction (a
`Configure` to `CullMode::GPU` with `Occlusion` on — `GpuCullSystem::ResolveActiveCullMode` reports
the reshape and `Configure` re-runs `ResizeHiZ`) allocates it afresh with its history invalid;
leaving it releases it. Under `CullMode::GPU` with `Occlusion` off the cull set binds a one-texel
stand-in nothing samples; under `CullMode::CPU`, and on the depth+normal path, there is no pyramid
and the graph imports an empty chain. `SceneRendererSettings::Occlusion` gates the
occlusion test within the GPU path; with it off the GPU path issues every camera-frustum survivor.
The submission shape is the **`drawIndirectCount`-free** form MoltenVK supports
(`multiDrawIndirect` + `drawIndirectFirstInstance`, the candidate id carried in each command's
`firstInstance` and read as an instance-rate vertex attribute); both modes drive the **same
buffer-indexed surface shader**, differing only in submission. The vertex stage reads that candidate
id to fetch its `DrawData`, and it also **passes the id through to the fragment** as a
`nointerpolation` interpolant on `SurfaceFragmentInput` (`v_CandidateId`), so a surface or
translucent **fragment** can call `LoadDrawData(v_CandidateId)` and reach the same per-draw record —
its `World` model matrix among other fields — rather than only the vertex stage reaching it; a
fragment that ignores the field renders identically. `CullMode::GPU` is gated on
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
`DidBroadphaseRefitLastFrame()` / `GetBroadphaseNodeCount()` reporting whether the tree rebuilt or
refit and its size. Tree maintenance is refit-or-rebuild on a version move, never insertion or
removal of single leaves; culling granularity is per-submesh, not meshlet;
occlusion is temporal hi-Z, not two-pass; shadow views cull on the CPU BVH only.

### ScenePass and PassIO

A `ScenePass` is a reusable, self-contained pipeline stage (`Configure` / `Resize` /
`Declare(RenderGraph&, const PassIO&)`) that **contributes** one or more `RenderGraph` passes into
the renderer's single internal graph — it is not a `RenderGraph::Pass`. The renderer owns the
**wiring** (which pass reads whose target, via the named-slot `PassIO`); each pass owns **itself**
(sizing, declared reads/writes, recording). It knows only how to record, never what feeds it.

The renderer's pipeline images (g-buffer albedo / world-normal / ORM / velocity / emissive, depth,
HDR, the bloom mip pyramid, output) are **renderer-owned `Image`/`ImageView`s
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

### Queue submission is asynchronous

**`vkQueueSubmit` and `vkQueuePresentKHR` return before the driver has encoded the frame.**
`ContextInfo::SubmitMode` (forwarded from `ApplicationInfo::SubmitMode`) defaults to
`QueueSubmitMode::Asynchronous`, which the context passes to MoltenVK at instance creation through
`VK_EXT_layer_settings` (`MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS = false`), so MoltenVK encodes each
submitted command buffer into Metal on its own serial queue while the render thread records the next
frame. A user's `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` environment variable wins over the request (the
context then passes nothing, since a layer setting would override it), and a driver offering no choice
runs its synchronous default; `ResolveQueueSubmitMode` (`Veng/Renderer/QueueSubmitMode.h`) is the
device-free precedence, `Context::GetQueueSubmitMode()` reports what took effect, the log names it at
init, and the `Render/AsyncSubmit` counter samples it every frame.

What the mode changes is only *when the driver reads what a command buffer references* — execution
order, fences and the device wait are unaffected (MoltenVK's `vkQueueWaitIdle`/`vkDeviceWaitIdle`
drain its encode queue before they wait). So the rule every path keeps, and the one asynchronous
encoding makes unforgiving, is the Vulkan one: **nothing a submitted frame references changes until
that frame's fence has been waited.** How each kind of state keeps it:

- **Destruction** goes through the retire bins, keyed to frame fences — never to a submit returning.
  Descriptor set layouts retire too.
- **Slot-keyed deferrals** — a bindless slot's `Release`, a material range — file under
  `Context::Native::GetReleaseSlot()`: the recording slot inside a frame, the last *submitted* slot
  outside one. Between `EndFrame` and the next `BeginFrame` (asset finalizes, the world tick,
  `OnUpdate`) `GetCurrentFrameInFlight()` already names the slot about to be waited, whose frame is
  the *older* one; filing a release there reclaimed the slot while the newest frame could still read it.
- **Host-written per-frame rings** write only the recording slot's slice, from inside the frame. A
  material update outside a frame skips the direct write while the current slot's frame may still run
  (`IsSlotWritable`) and lands at that slot's acquire instead, or at an `ImmediateCommands` recording
  made outside a frame, which waits the slot's fence first. A `SceneRenderer` executes at most once
  per frame (asserted against `Context::GetFrameSerial()`), because its rings hold one slice per frame
  in flight. The skinning palette is one region deeper, ringed per Execute, since a draw also reads the
  previous Execute's region for velocity. A single-copy buffer a frame reads is replaced, not
  rewritten (`PointField::Write`).
- **Non-bindless descriptor sets** are written at creation, ringed per frame slot, or rebuilt fresh
  with the old set retiring — never rewritten while a submitted frame may have bound them
  (`EnvironmentIbl::BindConvolveSource`).
- **Readbacks** are read only after the staging frame's fence: `AsyncReadback`, the picking readback
  (more than frames-in-flight Executes after staging), and the timestamp queries (read at the slot's
  next `BeginFrame`).
- **Every queue user takes `SubmitMutex`**, including the ImGui backend's own texture uploads.
- **The drawable.** MoltenVK acquires the `CAMetalDrawable` when it encodes the frame's first use of
  the swap chain image, which is now on its own thread. The swap chain holds more images than frames in
  flight (warned otherwise), so an image acquired again has finished its last encode and no queued
  encode waits on this thread. A resize is reconciled at `BeginFrame` before acquire, behind a device
  wait that drains the queued encodes, and a minimized window parks `BeginFrame` in
  `Window::WaitUntilPresentable` — pumping events, before any fence wait — so a frame queued before
  the minimize is never waited on while the window cannot be restored.

`tests/gpu/async_submit.cpp` churns these paths windowed (bindless slots and textures created and
released every frame, inside and between frames, a host-written ring, an output and window resize,
and a minimize and restore) and requires every frame to read back identically in both modes.

### The deferred opaque material g-buffer contract

An opaque (Surface-domain) material's **fragment shader outputs** are **g-buffer channels**, not
final swapchain color, written through a single engine-provided `GBufferOutput` struct
(`float4 Albedo : SV_Target0; float4 Normal : SV_Target1; float4 ORM : SV_Target2;
float2 Velocity : SV_Target3; float3 Emissive : SV_Target4;`). Albedo (G0) is sRGB-encoded
(sampled back as linear); the normal (G1) is the tangent-space-perturbed world normal in a signed
float format; ORM (G2) packs occlusion (R), roughness (G), and metallic (B) — the
metallic-roughness PBR channel set — with **the alpha the surface-flags channel**, whose values and
predicates live in the core shader header `Veng/surface_flags.slang`: today one flag,
`SurfaceFlagNoShadowReception`, which a material whose drawn geometry does not stand where its
shading says it does (an impostor, a sky proxy, a matte-painting shell, a proxy drawn at a
compressed distance) writes so the lighting pass takes full visibility in place of every shadow
atlas lookup — the receiving counterpart of `MeshRenderer::CastsShadows = false`, and a material
property rather than a per-instance one because it describes how the surface shades. A material
writing 0 there is unaffected. Velocity (G3, `RG16Sfloat`) is the per-object screen-space motion
vector
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

**Written is unconditional; stored is not.** Every channel is allocated and written every frame,
but the g-buffer pass stores a channel only when a later pass reads it — on a tile-based GPU the
store at the end of the pass is the bandwidth, and a channel with no reader can end its life in
tile memory. The decision is `FrameTopology::GBufferStores`, one `GBufferChannelState`
(`Stored` / `Discarded`) per colour channel, resolved with the rest of the topology. Albedo, normal,
ORM and emissive are always stored (lighting reads them; SSR and the debug arms read some), as is
depth. **Velocity is stored only when the TAA/TAAU resolve or the `MotionVectors` arm is wired**;
under every other configuration — the default `AntiAliasingMode::None`, FXAA, CMAA2, every other
debug arm — the pass declares `StoreOp::DontCare` on it, so `GetVelocityView()` stays non-null but
its contents are undefined after the frame. On an immediate-mode GPU the discard costs and saves
nothing.

### The g-buffer shading override

**`Settings.GBufferShadingOverride` (off by default) separates what the g-buffer pass spends on
shading from what it spends on geometry.** The pass's GPU time alone cannot say which bounds it,
and the two have different remedies. With the setting on, every opaque draw — static, instanced,
skinned, CPU- or GPU-submitted — binds an override pipeline in place of its material's
(`GBufferShadingOverride`, `src/Renderer/GBufferShadingOverride.h`), built by
`Material::BuildFragmentOverridePipeline` from **that material's own vertex stage, pipeline layout
and face culling** paired with `gbuffer_shading_override.frag`: a constant albedo, the interpolated
vertex normal, a mid roughness, the motion vector, and no material block or texture read. So the
pass binds the same sets, records the same draws over the same groups and runs the same vertex work
and raster, and the "Scene GBuffer" time with the override on is the pass without material shading;
the difference is what shading costs. Under a profiling build the application samples the override, and `LightTileCulling`, as 0/1 counters (`Render/GBufferShadingOverride`, `Render/LightTileCulling`), so one capture that flips either partway splits into its two halves over the same view. Lighting and everything after it run as usual over the
untextured surfaces.

- **One override pipeline per material and layout**, not one for the whole pass, because one
  vertex stage and one cull mode would change the geometry it means to hold fixed: a material may
  author its own vertex stage and a two-sided cull. The pipelines are built the first frame each
  material is drawn with the setting on, keyed on the layout they were built against (a material's
  static and skinned layouts are its own) and holding it, and dropped when the setting goes off.
- **Coverage is the rasterized geometry's.** The opaque contract has no alpha-test mode and no
  shipped Surface fragment discards, so nothing alpha-tested exists to keep; a custom fragment that
  discards, or writes depth through `GBufferDepthOutput`, is drawn at its rasterized coverage and
  depth instead, since both are computed by the fragment stage the override replaces.
- **It overrides the g-buffer pass only.** The depth-only passes run no material fragment stage,
  and the translucent pass keeps its materials' pipelines.

It is reachable from the debug panel (beside the debug view) and over MCP (`render.configure`), so a
capture can be taken with it on and off.

### The PostProcess fullscreen-material path

A `PostProcessScenePass` runs a PostProcess material as a fullscreen effect: it builds a
`GraphicsPipeline` from the material's fragment shader against a renderer-supplied color format
(fullscreen triangle, one color target, no vertex inputs), binds set-0 bindless, runtime-binds an
upstream target as a material handle field (`Material::SetTextureHandle`/`SetSamplerHandle`, no
resident asset), and drives the material's authored params. The loader builds the pipeline
*layout* for both domains but the `GraphicsPipeline` only for Surface — a PostProcess material's
pipeline is built by the pass, which alone knows the color format. **Tonemap is the PostProcess
material** (core `tonemap.vmat`): the HDR target — and, under bloom, pyramid mip 0 as its extra
input — is runtime-bound each frame, and the per-frame `Exposure` and bloom `Intensity` from
`SceneView` are written into the ring-buffered block each `Execute`. The fixed plumbing composites stay hardcoded engine passes — `SwapChainCompositePass`
(scene behind, ImGui over) and the `DebugView` blits (albedo/normal/depth, the packed-ORM
channels, the emissive channel, the SSAO target, the bloom pyramid, the directional and punctual
shadow maps) have no
authorable surface; a PostProcess material is for *tunable effects with exposed parameters*, not
plumbing.

### The pre-bloom GUI overlay

`GuiHdrOverlayScenePass` composites each `SceneHdrPreBloom` overlay into the scene colour after the
post-process effects and the post-resolve ribbons and before bloom, at the post-resolve allocation,
so an overlay composites over world linework drawn there. Each of its passes costs what the overlays
cover rather than the frame:

- **The direct pass exists only while a direct overlay does.** Overlays naming no material merge into
  one draw list blended in place; `ResolveHdrOverlays` recompiles on whether any is conveyed, beside
  presence and the material count, so a frame of only material overlays carries no load and store of
  the scene colour for it.
- **A material overlay's intermediate holds its document rect.** `PrepareDocuments` projects each
  material overlay's document ahead of the graph; the projection's bounds, rounded outward to a
  64-pixel granule and clamped to the target, are the overlay's **document rect** for the frame. The
  document renders with the rect's origin at the intermediate's origin. The one shared intermediate
  is sized to the largest rect seen, **growing on demand and never shrinking** while material overlays
  stay active (a resize below it is the one shrink), and released when none composites. A grow
  recreates the target and re-registers its bindless slot; the graph imports it by name, so it is not
  a recompile.
- **The composite is scissored to the rect.** Its viewport stays the full target, so `sv_position` is
  a scene pixel. The scene colour's (and mask's) load and store still cover the whole attachment — a
  render pass's load and store actions are not scissored on a tile-based GPU.

**The composite-material contract.** A composite material declares `Document` (texture handle) and
`DocumentRect` (`float4`: origin, size, in scene pixels), both written per frame into the pass's
per-view mirror, and reads the document through `LoadOverlayDocument` in the bindings-free
`Veng/overlay_composite.slang` (included after `Veng/postprocess.slang`), which subtracts the origin
and returns transparent outside the rect. A material lacking `DocumentRect` is reported once, by
name, and not composited — a read by scene pixel would land at the wrong texels.

**A composite may also ask for the document's own frame.** Three optional fields are written when a
material declares them: `DocumentExtent` (the document's logical extent), `SceneFromDocument` and
its inverse `DocumentFromScene` (each a 3x3 as `float4[3]` rows). A world-anchored document lies on a
flat plane seen through a pinhole, so the map is an exact homography
(`ComputeGuiOverlayHomography`, unit-pinned against the per-vertex projection); a screen-space one's
is a scale. An effect that must stand still on a projected sheet, or stop at an element's edge,
works in document points through `OverlayMapPoint` / `LoadOverlayDocumentPoint` rather than in the
rect's screen pixels — the rect is the granule-rounded screen bounds of the projection, not the
document.

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
  no `Presented` viewport, so there are **zero placements**, the gather is skipped, and the
  composite is ImGui over a black stand-in.

**A gather pass assembles; the composite encodes.** `GatherPass` (`Veng/Renderer/GatherPass.h`)
scissor-blits each `Presented` viewport's texture (a `CompositePlacement` = its `Ref<ImageView>` +
its `ViewportRegion`) into its region on one full-window linear-HDR (RGBA16F) **assembly target**,
in list order with an opaque blend and a linear, clamp-to-edge sampler, clearing the area no
placement covers; `SwapChainCompositePass` then consumes that single target *unchanged* (ImGui
over, the display-transfer encode once). N quadrant placements is splitscreen; picture-in-picture
and a region short of the window (letterboxing, an absolute region after a resize) assemble the
same way. `SetPlacements` registers exactly one bindless slot per placement (`MaxPresented` is the
budget, asserted at register time). **Splitscreen falls out** as "register N `Presented` viewports
with quadrant regions"; it needs no bespoke compositing path.

**The gather runs only when there is something to assemble.** When the *last* placement's region
is exactly `{0, 0, swapChainExtent}` — the fullscreen game, or a full-window overlay drawn over
earlier placements, which the opaque blend would overwrite — the gather's full-UV linear lookup
into it is the very lookup the composite makes when sampling it, so `ViewportCompositor` hands the
composite that viewport's output directly and skips the gather's full-window clear, read and
write. At `MaxAllocationScale == 1` the result is bit-identical; at any other scale the same
filtering happens in the composite instead, minus one RGBA16F re-quantisation. With **zero
placements** (the editor) the composite samples a renderer-owned 1×1 black image — clamp-to-edge
makes it black everywhere — instead of a gathered clear. The decision is a device-free function
(`CompositeSource.h`, pinned by `tests/unit/composite_source.cpp`, since no gpu case can drive the
windowed tail), and the composite's scene source is re-pointed (a bindless re-registration, no
recompile) only when it changes: a viewport's output identity moves on its `Resize`/`Configure` and
when documents attach or detach. A world under a full-window overlay is still rendered, just not
sampled — as it was overwritten before.

**An empty ImGui frame costs nothing.** `ImGuiLayer::Render` ends the ImGui frame before recording
and, when the draw data is empty (no command lists or no vertices), records no pass and no
transition — the full-window RGBA16F layer image is neither cleared nor stored — and
`HasDrawnOutput()` reports it. The compositor then blends a 1×1 transparent image in its place
(`SwapChainCompositePass::SetOverlaySource`), so `lerp(scene, ui, ui.a)` returns the scene with no
shader or graph change, and swaps back when the layer draws again. A frame the app declares it draws
no immediate-mode UI in (`Application::IsImGuiFrameWanted`) goes further and runs no ImGui frame at
all — `ImGuiLayer::SkipFrame` stands in for NewFrame and Render, and reports no drawn output the
same way.

**A presented frame is read back through a mirror, never off the swap chain.** The finished
composite — scene plus whatever overlay was drawn over it — exists only in the swap chain image, and
`vkQueuePresentKHR` hands that image to the presentation engine: it is not the application's again
until it is re-acquired, so transitioning it for a readback afterwards is a **write-after-present
hazard** the synchronization validation layer reports as an error (and, on MoltenVK, a readback that
can stall on the held drawable). So `Context::RequestPresentedFrameCapture()` asks for a mirror
instead: the next `EndFrame` blits the composite into an engine-owned image **immediately before the
present transition**, the last point at which the frame still owns the swap chain image, and
`GetPresentedFrameMirror()` hands that copy back to be downloaded as an ordinary owned image. A
request is **one-shot** — serviced by one frame end, with any requests before it coalescing into it
(`IsPresentedFrameCapturePending()` is true until then) — so the full-window blit is paid only by a
frame something asked to read, never by every frame once some consumer exists. The mirror follows
the swap chain's format and extent (rebuilt when either moves), needs the surface to have granted
transfer-source usage (`IsSwapChainCaptureSupported()`), and is inert headless. `veng::mcp`'s
`render.screenshot_window` is the consumer: it requests at the pump that receives the call and reads
at the next (`McpTool::BeforeFrame`).

**A `CaptureSink` composites the frame a second time, into an image somebody else owns.** The mirror
above copies the display's pixels; a consumer that wants the *frame* — at its own encoding, without
the app's overlay, into memory a platform encoder reads — wants the composite run again rather than a
copy of its result. `ViewportCompositor::SetCaptureSink(CaptureSink*)`
(`Veng/Renderer/CaptureSink.h`) installs that consumer: each `Composite`, after the presented
composite, the sink is asked for a `CaptureTarget` (`{ Ref<Image>, DisplayColorSpace,
IncludeOverlay }`) for this frame's in-flight slot, and a supplied one is written by a **second
`SwapChainCompositePass`** the compositor owns — the same scene source as the presented composite,
the target's format and colour space, the overlay bound only when the target asks for it. So the
file's encoding is a setting rather than a consequence of the display, and the presented frame is
untouched: the mirror and `render.screenshot_window` keep working during a capture.

Four properties of the seam:

- **The target is the sink's, and the sink is asked every frame.** Returning nullopt skips the
  capture composite for that frame, which is how a sink idles. The compositor caches one `ImageView`
  per target image, so a recycled pool of targets is viewed once each rather than per frame.
- **Extent is a contract, not a resample.** The sink is handed the presented extent; a target of any
  other extent is a fatal assert and the composite is skipped rather than stretched into.
- **The pass is rebuilt only when its shape moves** — the overlay choice changing, or a swap-chain
  invalidation (which re-sources the presented composite the same way). A format or colour-space
  change is a `SetSwapChainTarget` on the existing pass, and every change of the presented
  composite's scene or overlay source is applied to it too, so it never samples a source the
  display has moved off.
- **Readability rides the frame fence.** The capture composite is recorded into the frame's command
  buffer, so its target is readable when that slot's fence has been waited —
  `GetMaxFramesInFlight()` frames later, the contract `AsyncReadback` already rides. With a sink
  installed the compositor registers `Context::AddFrameRetiredCallback` and forwards it as
  `CaptureSink::OnSlotRetired`; the engine never waits a fence on the sink's behalf.

**The composite into a sink cannot run in the test band.** Every context in the `gpu` tier is
headless and the compositor's tail exists only windowed, so what the band proves is the import
(`tests/gpu/external_texture_import.mm`) and the retirement contract
(`tests/gpu/frame_retired.cpp`); that the second composite writes the right pixels is a live look.

**An externally-owned platform texture can be the image a sink hands over.**
`Renderer::Backend::ImportExternalTexture` (`Veng/Renderer/Backend/MetalInterop.h`) wraps one as a
managed `Image` whose texels live in the external memory — the engine renders straight into it and
never copies out. `Context::IsExternalTextureImportSupported()` answers whether the device has the
capability (Apple, with the Metal-object extension enabled — headless included, since the import
needs a device and not a swap chain) and `Context::GetExternalDevice()` hands back the platform
device a consumer must create its texture on. Three things about it are load-bearing:

- **The import reads the format and extent off the texture and takes neither from its caller.** The
  implementation does not validate an import: a wrong format or extent is accepted and the channels
  silently swap. The mapping is one closed table —
  `BGRA8Unorm_sRGB → BGRA8Srgb`, `BGR10A2Unorm → A2R10G10B10Unorm`, `RGBA16Float → RGBA16Sfloat` —
  and anything else is refused. Plain `BGRA8Unorm` is deliberately absent: a consumer wanting 8-bit
  standard range wants the sRGB store.
- **The imported image is managed, and its teardown is fence-deferred.** Unlike a swap chain image,
  the `VkImage` here is one veng created, so it retires like any other; its `Native` carries a
  teardown callback that drops the engine's reference to the platform texture, run by the retire bin
  after the handles are destroyed. Dropping the last `Ref` mid-frame is safe.
- **Validation reports "used with no memory bound" on every use of one, and only there is anything
  bound.** The message is the extension's gap, not a defect: the import binds a throwaway device
  allocation under `VE_ENABLE_VALIDATION_LAYERS` alone, which the layer accepts and which changes
  nothing — the render still lands in the external memory, which the GPU case asserts with the
  allocation bound. A shipped build pays neither the allocation nor the call.

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
budget, rendering into a `round(renderAllocExtent · RenderScale)` sub-rect of the render allocation
that the promotion carries back up. It is **free**: a sub-rect change moves no allocation, only the
per-frame `SceneView::RenderScale` fraction — so it adapts **cost**, never the allocation
footprint, and never hitches. `GetAllocationScale()` reports the fixed **render** allocation scale
(`MaxScale` while dynamic resolution is on, else the static `RenderScale`); it names the scene side
alone, and `GetPostResolveAllocationExtent()` is the tail's, which no render scale touches. The
allocations are sized **once**, and the expensive `SceneRenderer::Resize` — which retires every
target, re-registers bindless, and recompiles the graph — fires only on a genuine region/window
extent change or an explicit render-scale/`MaxScale` change, never from frame-time pressure.

**`MaxAllocationScale` is a fixed ceiling relative to the backing extent, defaulting to `1.0` (full
native), and it applies to both allocations.** The post-resolve allocation is
`round(region · MaxAllocationScale)`; the render allocation puts `GetAllocationScale()` inside it as
`round(region · MaxAllocationScale · GetAllocationScale())` (`ExtentForScale`), and the sub-rect then
rides inside *that* as `GetViewRenderScale()`. A managed viewport tracks the full swapchain
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

**The probe binds into a per-entity clone of the sibling material, not the shared mesh instance.**
The drive writes its output — the octahedral map handle and the `ProbeCenter` validity flag a
consumer gates the reflection on — into `materials[0]` of the sibling `MeshRenderer`. That instance
belongs to the **mesh asset**, shared by every entity drawing that mesh, so writing it directly
would make every sharer sample this probe (the classic case: two ships of one model, only the
player's cockpit should reflect). So `WorldRunner::DriveCaptureSurfaces`, on a captured entity's
first drive, **clones the sibling `materials[0]` (`MaterialInstance::Clone`) and installs it as that
entity's `MeshRenderer::InstanceMaterials` override** — the per-entity material seam the render
gather honours (`GatherMeshes` → `VisibleMesh::Materials`, see [../Scene/CLAUDE.md](../Scene/CLAUDE.md)
and [../Asset/CLAUDE.md](../Asset/CLAUDE.md)). The probe then writes only the clone: the capturing
entity draws the reflective copy and every other sharer keeps the untouched asset instance, sampling
nothing (its `ProbeCenter.w` stays 0 → the material's no-probe path). The install is one mutable
component write per capturing entity — it bumps the scene's spatial version once, which the
broadphase re-gathers on — and the clone is held by the override for the entity's lifetime, so a
consumer authors no second mesh or material to keep an NPC's copy of a reflective surface flat.

**A capture optionally publishes an octahedral distance map beside the radiance one**
(`SceneCaptureInfo::CaptureDistance`, off by default). The same six faces are rendered with a depth
buffer either way; when asked, their depths tile into a second (3×2) atlas and resample into a
single-channel `R32Sfloat` octahedral map — in the *same* parameterization, so one
`OctahedralUV(direction)` indexes both — holding the **radial** distance from the probe centre to the
nearest surface in each direction (world units), with `SceneCapture::DistanceSkySentinel` where a
direction saw only the far-plane sky. Radial, not the face-axis distance the depth linearizes to: the
map is direction-indexed, and dividing by the direction's cosine with the face axis (the projection's
own denominator) is what a consumer cannot do afterwards, since the octahedral map hides which face a
texel came from. The distance resample point-samples the depth atlas — a bilinear tap across a depth
discontinuity yields a distance at which nothing is. Off, none of the distance path exists (no depth
atlas, no distance map, no extra pipelines or slots). Nothing in the engine consumes it; a consuming
material walks it (see the parallax-correction note under "Deliberately not here").

**A capture in a world nothing presents is not driven.** A capture feeds a material sampled by a mesh
drawn in some view, so a world no view shows has nowhere its capture could be seen — and worlds are
flat peers of which several are live at once in the ordinary case, so driving every live world's
captures multiplies the per-frame view budget by the number of worlds held warm. `WorldRunner`'s
per-frame drive therefore asks presentation first (`Application::IsWorldPresented` — a managed or
bound viewport's binding, a viewport a consumer drives itself, or an in-flight rebind's destination
for its whole wait, so a make-before-break swap presents a warm probe) and skips an unpresented world
whole, re-arming its already-materialized captures (`CaptureSurface::MarkDirty`) so a world that
becomes visible again rebuilds its maps instead of resuming from what it saw before it went dark.

**A capture is built at most one per frame, and a released one is reused.** A `SceneCapture` owns a
whole face renderer, and a world presented for the first time can arrive with several capture
surfaces at once — so the world drive builds at most `WorldCaptureDriveInfo::MaxNewCaptures` (one)
new capture per pass, leaving a surface past the budget unmaterialized and undriven until a later
frame. A capture whose surface is destroyed — its entity, its component, or its whole world closing
— goes to the runner's `SceneCapturePool` (`WorldRunner::GetCapturePool`) instead of being freed:
detached from the drive-list and reset (`SceneCapture::ResetForReuse` — the pushed view, the
round-robin, the first-render atlas clears, and the scene its face renderer last gathered, through
`SceneRenderer::ReleaseScene`), it is handed to the next surface asking for an identical
configuration (`SceneCapture::IsConfiguredFor`: face resolution, renderer settings, distance and cube
paths) before any build is considered, and that reuse costs no budget. The pool holds a bounded
number (`SceneCapturePool::DefaultCapacity`) and drops the longest-held past it. A surface driven
directly through `CaptureSurface::Drive`, outside the world drive, still builds its own on first use
and frees it when it goes.

**A surface can be switched off without losing its settings.** `CaptureSurface::Enabled` false makes
the world drive release the surface's runtime — the capture to the pool, the material slots it bound
cleared — and build nothing for it while it stays off (`WorldCaptureDriveResult::SurfacesDisabled`
counts them); `IsRefreshing` reads false. Re-enabling it materializes a capture on a later pass, from
the pool when one matches. It is the switch for a probe wanted only some of the time — one that
matters only while a viewer is inside what it captures — so its owner flips one flag instead of
storing the authored settings and recreating the component.

**A renderer built mid-frame waits on nothing.** A face renderer's one-time setup — its shadow
atlases' first clears, its LTC tables' upload, a baked sky cube's first clear — is handed to
`Context::RecordSetupCommands` rather than an immediate submit, which would wait behind the frame in
flight: it records into the frame's own command buffer at the point of construction (or into the
open `ImmediateCommands` buffer, or, outside any recording, at the head of the next one), ordered
before every later use of what it initializes.

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

**A capture also draws a subset of the render layers, and defaults to the environment set.** Beyond
the one nominated entity, a capture filters by `RenderLayer` (`Veng/Scene/RenderLayer.h`): every
drawable sits on a layer (`MeshRenderer::Layer`), and `CaptureView::VisibleLayers` names the layers
the faces draw. `CaptureSurface::VisibleLayers` defaults to `DefaultEnvironmentCaptureLayers` — every
layer but `RenderLayer::ViewAnchored` and `RenderLayer::Display` — because a probe records the
environment around its position, and neither camera-anchored decoration (a near-field particle
shell, a billboard slaved to the eye) nor content presented to a viewer (a world-space readout, a
holographic instrument) is part of it: drawn into the map, it would appear in every reflection or
lens sampling the capture, floating at a distance it was never at or reflected as though it were
scenery. **Ribbons, trails and ribbon paths honour the same filter and the same exclusion**: each
carries a `Layer`, and `GatherRibbons` takes the view's mask and excluded entity and gathers neither
an off-mask one nor the entity a capture feeds, exactly as `GatherMeshes` does. The filter is the same closed producer→consumer machinery the
entity exclusion rides — `SceneView::VisibleLayers` carried into `SceneBroadphase::Sync`, applied by
`GatherMeshes` beside the `Visible` test, a changed mask its own rebuild trigger — and the ordinary
camera view keeps its default `AllRenderLayers`, so it draws every layer and is unaffected.

Deliberately **not** here: an *arbitrary* per-entity visibility mask (the entity filter is one
nominated entity in a closed producer→consumer pair, and layer membership is a closed table, not a
free-form set — neither has an authoring story to get wrong), **recursive probes** (another
capture-consuming surface in the map reads a one-frame-old result, invisible at a reflection's
contrast), and **parallax correction**, whose math belongs to the consuming material —
the engine publishes the *data* a correction needs and applies none. `CaptureSurface::CenterSlot`
publishes the world position the map was rendered from, plus a validity flag;
`CaptureSurface::OrientationSlot` the frame the faces were oriented in as a quaternion; and
`CaptureSurface::DepthTextureSlot` (opt-in) a second octahedral map, in the same parameterization as
the radiance one, holding the **radial distance** from the probe centre to the nearest surface in
each direction (world units, with `SceneCapture::DistanceSkySentinel` where a direction saw only the
sky). So a material can walk the recorded distance along a reflected ray rather than intersect a
hand-authored stand-in volume — but the walk, and the correction, are the material's; the engine
records how far away what it saw was and stops there.

## Generated textures: compute something expensive once, then sample it

`GeneratedTextureService` (`Veng/Renderer/GeneratedTextureService.h`) is the engine's answer to
"fill a persistent texture with GPU work too expensive to pay per frame". It is **`Context`-owned**
(`Context::GetGeneratedTextures()`) and **pumped once per frame from `BeginFrame`**, before any pass
records, so a job's result is sampleable by the passes of the frame that finished it.

A **job** is `{target images, tick count, tick cost, tick callback, priority}`, keyed by a
caller-chosen `u64`:

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
- **Scheduling is priority-then-FIFO, re-evaluated per tick**, spending a **total cost budget** per
  frame (`SetCostBudget`, `UnlimitedCostBudget` for none): each request declares a per-tick **cost**
  (`GeneratedTextureRequest::Cost`, a relative weight, default 1), and the pump runs ticks until
  their summed cost reaches the budget — so an expensive fragment amortizes over many frames and a
  cheap one runs many ticks a frame, without the engine ever pricing a fragment it did not write. A
  single tick dearer than the whole budget still runs one a frame (the one-tick minimum), so it
  makes progress rather than stalling; at the default cost of 1 the budget is a tick count, the
  schedule this replaced. Raising a queued job's priority preempts a running one at the *next tick*,
  not the next job. Requests are **idempotent on the key**, so re-requesting every frame while the
  result is still wanted is the intended usage; `Cancel` tears an unfinished job down and simply
  releases its targets.

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
`EnvironmentIbl` and the picking readback are in-tree hand-rollings of its parts; they keep their
current shapes, and are named here because they are what the API was shaped against rather than
because anything migrates. `BakedSkyCube` is the exception that does route through it: its display
bake fills a scratch cube one face per tick as a service job, so a dirty baked sky never blocks a
frame on six fullscreen sky evaluations and the previous cube stands until the new one lands.

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
  ran that pump, and `TicksLastPump` never counts a restore. **A restore spends the budget**: it is
  charged one tick of its job's cost, in the order ticks are selected and under the same
  first-one-always rule, and the tick loop spends what the restores left. So a world's worth of
  cache hits staged together lands over as many frames as its ticks would have spread over, never
  as one frame of copies; a restore past the budget stays staged, its job still held. Inside the
  pump's `Generated Textures` GPU scope, the restores record under `Generated Texture Restores` and
  each tick under its request's `Name`, and the pump samples the `GeneratedTextures/Ticks` and
  `GeneratedTextures/Restores` counters, so a capture says which job a frame's generation went to.
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
- **Nothing rides bindless and nothing touches the render graph.** Every kernel binds its own set 3
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

A caller that wants a texture to *flow* along a velocity field with no pressure solve wants
**`FlowField`** below, not the solver — the advection kernel is shared, but none of the pressure
solve is paid.

## FlowField: transporting a dye along a caller-owned velocity field

`FlowField` (`Veng/Renderer/FlowField.h`) is the lean, art-directed cousin of `FluidSim` with the
whole pressure solve removed: a caller supplies a velocity field, and `FlowField` carries one or
more dye images along it in a feedback loop. It reuses `FluidSim`'s semi-Lagrangian advection kernel
(`fluid_advect.comp`) and its per-format store family verbatim, adding one kernel of its own — the
clamped unsharp mask (`flow_sharpen.comp`). It renders nothing and owns no meaning; a dye's channels,
its seed, and how many steps are worth running are the caller's.

- **What it is:** advection-only transport of a dye along a caller-owned velocity field.
- **What it is not:** no solve — the primitive performs no velocity *solve*, which is what makes it
  a flow field rather than a fluid; no seeding, no reinjection, no colour management. It never writes
  the velocity itself, but the field is not frozen: the caller may rewrite it between advects (below).

The surface is the intersection of what a flow effect needs:

- **The velocity is the caller's and FlowField never writes it.** `RG16Sfloat` or `RG32Sfloat`, in
  grid cells per unit of flow, its extent *is* the grid. One or more dye images (`R16Sfloat` /
  `RG16Sfloat` / `RGBA16Sfloat`, up to `MaxFlowDyes`) are advected in place through the one
  internally owned `RGBA32Sfloat` scratch — the *only* thing the primitive allocates, since
  advection cannot run in place. There are no pressure, curl or divergence transients.
- **`RecordAdvect(cmd)`** records one semi-Lagrangian advection of every dye along the current
  velocity, honouring the shape's per-axis `FlowWrap` and per-row metric; **`RecordAdvect(cmd,
  steps)`** is the barriered loop. The advance-per-step is the shape's `StepScale`, so a caller tunes
  how far the dye moves without re-timing anything.
- **The field is static to the primitive, but a caller may evolve it between advects.** `FlowField`
  writes no velocity, yet the advect reads the velocity live off the GPU, so a caller may rewrite
  the velocity image between advects and the next advect reads the new field — the sanctioned way to
  build an animated or turbulence-perturbed flow. The caller records its own write pass (transition
  to the write layout, write with a compute or transfer, leave the write registered in the image's
  tracked state); the advect transitions the velocity back to a sampled layout from that tracked
  state itself, so the write is visible to the advect and the advect's read is ordered ahead of the
  caller's next write. Because a caller owns the image and records its barriers, the primitive needs
  no behavioural change for this — it always read live.
- **`RecordSharpen(cmd, strength)`** records a clamped unsharp pass: a dye minus a blurred copy of
  itself, then **clamped to its local 3×3 neighbourhood's range**. The clamp is not optional — an
  unclamped sharpen in a feedback loop amplifies each pass's overshoot and diverges; clamping to the
  neighbourhood means the result is never brighter than the brightest neighbour, so the global
  maximum is non-increasing under any advect/sharpen sequence. `strength` 0 records nothing.
- **Nothing rides bindless and nothing touches the render graph.** Every kernel binds its own set 3
  and the primitive records its own barriers, the `FluidSim` / `AtmospherePrecompute` pattern.

**A long unbroken advect turns any dye to mush.** Each step samples with interpolation, so a feedback
advection homogenises over many steps — the dye's contrast bleeds toward the local mean. `RecordSharpen`
fights the blur, but a caller holding a specific look must interleave its **own** reinjection of source
content between advects, because the source is the caller's. The idiom is the one `FluidSim` documents:
**wrap `RecordAdvect` in `GeneratedTextureService` ticks and interleave your own reinjection**, the
primitive holding no reference to the service.

**The device-free core carries the correctness.** The wrap fold, the step-scaled back-trace, the
bilinear tap and the clamped-sharpen bound are pure functions (`FoldFlowTexel`, `FlowBackTrace`,
`SampleFlowBilinear`, `FlowSharpen` in `FlowField.h`), and the configuration validator
(`src/Renderer/FlowFieldConfig.h`, the `FluidSimShape` precedent) is device-free too — both pinned in
`tests/unit/flow_field.cpp` with no ICD, including the property that a static field keeps a dye's mass
bounded across many advects. `tests/gpu/flow_field.cpp` is the mirror check: a dye advected along a
known field lands where the back-trace predicts, and a sharpened feedback advection carries more local
contrast than the un-sharpened control while staying bounded.

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
draws select array elements via push-constant indices. Sets 1 and 2 are the registry's volume and
cube arrays, so author-declared descriptor sets shift to **set 3+** (`BindlessRegistry::FirstUserSet`).

**The registry reports its occupancy, not only its headroom.** `GetFreeSlots()` gives the seven
arrays' free counts (`BindlessCapacity`); `DescribeSlots(BindlessArray)` gives one array's slots in
index order, each carrying its `BindlessSlotState` — **`Free` / `Occupied` / `PendingRelease`**, the
third being a slot whose `Release` window has not expired, which is neither allocatable nor idle and
is what explains a free count trailing the unoccupied count — plus what the slot holds:
`BindlessSlot`'s union of the fields the arrays can describe (an `ImageView`'s name, format, the
image's extent, the mips and layers the view exposes, and their tightly-packed `ImageBytes` from
`FormatInfo`'s block geometry; a `Buffer`'s name and size; a `Sampler`'s name). **Nothing is
recorded per slot for it and no registration site changed**: each `SlotArray` is homogeneous in the
type its own `Register` took, so the type-erased `Ref` the registry already keeps to stop a resource
dangling casts back and the description is read off it at call time. The question it answers is the
one a free count cannot — an array at 80 % of its capacity is either holding what it needs or
holding one atlas nine times, and only the occupants tell those apart.

**Materials are the one entry denominated in bytes**, because they are suballocated from the
parameter arena rather than drawn from a slot table (see
[../Asset/CLAUDE.md](../Asset/CLAUDE.md)): `BindlessCapacity::Materials` is free **bytes** of
`MaterialArenaBytes`, beside `MaterialBlocks` (the live allocation count) and
`MaterialLargestFreeRun` — free bytes far above the largest run is fragmentation rather than
occupancy, which is the reading a slot count cannot give. `CapacityOf(Materials)` answers the arena
size in bytes to match, and `DescribeSlots(Materials)` answers one entry per **range** — a live
block, a free run, or a run still inside its release window — with `Index` the range's byte offset
and `SizeBytes` its length, ascending, so the listing reads as a map of the arena.
`BindlessArrayName` / `ParseBindlessArray` / `BindlessSlotStateName` are the text vocabulary a
diagnostic reports and accepts through (`veng::mcp`'s `render.bindless_slots` is the consumer);
`tests/unit/bindless_slots.cpp` pins the round trip and the capacity mapping device-free.

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
