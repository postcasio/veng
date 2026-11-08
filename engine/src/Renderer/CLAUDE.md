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
queued, and the scene layers pass (ribbons, sprites, particles) and the half-resolution composite
idling inside their deactivation windows, are the users: on a tile-based GPU an empty render pass still loads and stores
its targets.

**A producer nothing reads some frames skips with `SkipWhenUnread`**, which may clear or discard:
the caller promises that on a frame the predicate holds every reader of the pass's outputs either
skips too or samples nothing from them, so the undefined contents a skipped clear leaves are never
observed. The graph cannot check that promise. The refraction grab and its blur chain (skipped on a
frame no draw samples the grab) and the half-resolution layer's depth reduce and layer pass (skipped
with their composite on an idle frame) are the users. `CompiledGraph::DidRecordPass(name)`, surfaced
as `SceneRenderer::DidRecordPassLastFrame`, reports whether a pass recorded on the last `Execute`.

**A pass may leave a colour slot unbound** — `PassBuilder::UnboundColor()`, declared in order with
its other attachments. The schedule bakes an attachment with no view there (`ScheduledAttachment::
UnboundSlot`), `BeginRendering` binds a null view, and every pipeline drawn in the pass declares
`Format::Undefined` at that location (`PipelineAttachmentInfo`), whose writes are discarded —
Vulkan's dynamic rendering supports exactly this pairing with no extension. It keeps a later
attachment's location fixed whatever the frame wires before it: the translucent passes leave the
bloom mask's location unbound when bloom is off, so a reactive material's output is location 2
either way.

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
  passes, SSAO, the screen-space reflection chain's placement, the skybox, the
  sky/point-field/volume/ribbon/sprite passes, the depth-of-field composite,
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
  precedent: `ShadowSystem`, `BloomPyramid`, `AnamorphicStreak`, `AutoExposureMeter`, `TaaResolve`,
  `PostResolveUpscale`, `DofChain`,
  `RefractionGrab`, `GpuCullSystem`, `InstancedMeshSystem`, `SphereTerrainSystem`, `LightTileCuller`, `SsrChain`, `ParticleSimulation`, `GBufferShadingOverride`, `PickingSystem`, and `SkyResolver` (which itself owns the
  three sky radiance-cube helpers `EnvironmentIbl` / `AtmospherePrecompute` / `BakedSkyCube`).
  A subsystem owns its full vertical slice — its `Create`/recreate path, its `Declare*`
  contribution, its per-frame work — and **releases its own bindless handles in its own
  destructor**, so `~SceneRenderer`'s hand-list holds only the spine handles.
- **A renderer-internal header lives in `src/Renderer/`, never `include/Veng/`.** `SkyResolver.h`,
  `DrawPlan.h`, `SkySourceKind.h`, `FrameTopology.h`, `DrawGather.h`, `LodSelection.h`,
  `DebugBlitPipelines.h`, and `SceneRendererIds.h` are private headers beside the sources that consume them. Two properties
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
  (`SpriteActive`, `RibbonActive`, `RibbonPostResolveActive` and `ParticleActive`, each with its
  idle counter) live in
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
  draw.** `GatherStaticOpaque` keys its survivors with `DrawKey` — (parent material, mesh, level of
  detail, submesh, candidate) — and orders them with `SortDrawKeys` before claiming a slot, so every
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
radiance at the floor, so a light brighter than the reference sun could write `inf`, which a later
filter (the TAA resolve, a blur) then turns into a `NaN` that spreads. A clamp on `D` would reshape the lobe and still not
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
(default 0.2), Filament's defaults — authored on a scene's `RenderLook` like the other per-frame
knobs — carried in the view block's `AmbientParams.yz` (see "View
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

**Normal-map specular anti-aliasing widens the lobe by what a normal map's mips averaged away.** The
geometric term sees only how the shading normal changes *between* pixels. Minify a finely detailed
normal map and its mips average the detail inside each texel into one smoother normal, so the normal
the pixel shades with varies little between pixels while the surface it stands for is rough — a
glossy lobe narrower than that hidden spread lands on a pixel one frame and misses it the next, and
the classic runtime repair (Toksvig's, from the shortened length of a mip-averaged normal) is not
available, since a cooked normal map stores only X/Y. So the spread is measured at cook time: a
`Normal`-role texture carries a **normal-variance companion**, one single-channel value per texel of
every level — the alpha² a von Mises-Fisher lobe fitted to the normals that texel averages must gain
(`Veng/normal_variance.slang`, `Veng/Asset/NormalVariance.h`), stored as its fourth root — and the
registry writes it into the **normal-variance array** (`g_NormalVariance`, set 0 binding 8) at the
normal map's own slot. A material samples it with the normal map's handle, sampler and coordinates
(`SampleNormalVariance` / `SampleNormalVarianceGrad`, `Veng/surface.slang`), so the hardware picks
the same level, and widens the roughness it writes or shades with once,
`alpha² += SceneView::NormalMapAntiAliasingScale · variance` (`WidenRoughnessForNormalMap`; default
1, zero off, carried in `AmbientParams.w`), scaling the variance by the square of any deflection
strength it applies. **The two widenings sum in alpha² and do not double-count**: the companion holds
the spread *inside* the sampled texels, the geometric term the variation *across* the pixel, so a
magnified map (level 0, no companion spread) leaves the geometric term alone and a strongly minified
one (smooth between pixels) leaves the companion's term carrying the widening; where both see some,
they are the two halves of the law of total variance. The companion's term is bounded only by a fully
rough lobe, the geometric one by `κ`. It costs one fetch per normal-mapped surface, and a texture with
no companion — any non-`Normal` texture, a runtime-built one, a single-level map — reads zero, so its
material shades exactly as without it. `tests/gpu/specular_lobe.cpp` pins that a flat normal map
renders identically with it on and off, and that a minified glossy plane under a noisy normal map
holds its reflected energy across sub-pixel moves (measured at landing: 0.008 % against 5.5 %
unwidened).

### Anti-aliasing

**`Settings.AntiAliasing` is one mutually-exclusive `AntiAliasingMode`** — `None` (default), `FXAA`,
`TAA`, `CMAA2`, `TAAU`, or `MetalFXTemporal` — resolved by `FrameTopology` into at most one wired
resolve. `TAA`, `TAAU` and `MetalFXTemporal` are the HDR-space temporal resolve below; `FXAA` and `CMAA2` are the post-tonemap **spatial**
resolves under "Spatial anti-aliasing" further down. **Supersampling (SSAA) is not one of these
modes** — it is orthogonal, driven by the viewport's `MaxAllocationScale` (see "Adaptive resolution"
and the `Viewport` section), and composes with every mode. `Settings::UsesTaa()` is the named
predicate the jitter, the history-reset, and which resolve occupies the temporal anchor key on (true
for every temporal mode); the post-resolve tail runs at the post-resolve allocation in every mode and
keys on nothing. `Settings::UsesTaaUpscaling()` is `TAAU` and `MetalFXTemporal`, read by
`ResolveTemporalUpscalePromotes` to decide whether the temporal resolve is itself the promotion.

#### TAA

**TAA is an HDR-space temporal resolve** (`AntiAliasingMode::TAA`, off by default). It jitters the
projection by a Halton(2, 3) sub-pixel offset each frame (`Renderer/TaaJitter.h`, a pure
device-free helper), routes the lighting pass into a separate **lit** target, and inserts a
**resolve** pass (lit + reprojected history + velocity/depth → the HDR target the bloom/tonemap
tail already samples) and a **history-copy** pass (the resolve's output → the persisted history for
next frame). Under a composited depth of field the resolve writes the scene intermediate the chain
composites from, not the HDR target, and the copy reads that intermediate by its own bindless slot:
read back from the HDR target, which the composite writes later in the frame, the history would
carry the previous frame's composite.

**The jitter is an NDC translation, published, and taken back out by the resolve.** Each period's
offsets have their mean subtracted (`TaaJitterPeriodMean`), so the eight phases sum to zero and the
integrated image is not displaced. The offset, a fraction of the rasterized sub-rect, becomes an NDC
translation (`TaaJitterNdc`) that is **left-multiplied** onto the projection (`JitterProjection`):
every projected point moves by exactly that much at every depth, under a perspective and an
orthographic camera alike, and depth is untouched. It is published as `SceneView::JitterNdc` and in
the view block's `JitterNdc` (xy this frame, zw the previous one), with `Proj`, `InvViewProj` and
`InvViewRotProj` carrying it and `CurViewProj`/`PrevViewProj` not. **Anything rasterized into the
scene colour ahead of the resolve must project through the jittered matrix** — the surface stage
(`Proj · View`), the sky (`InvViewRotProj`), the SceneColor-placed point fields (which add the
pushed jitter to their unjittered clip positions) — because the resolve removes the jitter from
the whole lit target: an unjittered draw there comes out moving by the jitter. A pass after the
resolve (the HdrTail point fields, the post-resolve ribbons, debug draw) projects unjittered.

**The resolve reconstructs the current frame at each output pixel's unjittered position.** It finds
where that position fell in the jittered render (`uv + JitterNdc / 2`, in render pixels), loads the
3×3 render texels around it, and weights each by `exp(-2.29 · d²)` — the Gaussian fit of a
one-pixel Blackman-Harris window — with `d` the texel's sample distance from the pixel centre **in
output pixels**. At render scale 1 that is a symmetric filter about the pixel's own unjittered
sample. Under upscaling (`TAAU`, or `TAA` over a dynamic-resolution sub-rect) the window narrows
with the output/render ratio, capped at 2.5 so the period's samples still reach every output pixel,
and the **summed weight scales this frame's blend share**: an output pixel a sample landed on takes
the full share, one no sample landed near leans on its history. The same nine texels build the
YCoCg clip box and give the **nearest depth** (reverse-Z: the largest); a pixel with geometry
anywhere in the 3×3 reprojects by the motion vector at that texel (point loads throughout — depth
is never filtered). A pixel with only the cleared depth around it reprojects as a **direction**
through the view block's `PrevFromCurDirection` — the previous unjittered `Proj × rotation-only
View` times the inverse of this frame's, composed in double on the CPU — so it is unjittered on
both sides, carries no translation to lose precision to, and is the identity under a camera that
has not turned: a static camera fetches the background's history from exactly the pixel it writes.
The Karis luminance weights are taken on the **exposed** luminance (the tonemap's exposure, pushed),
so their damping does not depend on the scene's absolute HDR scale.

**Sprites, particles, ribbons and reactive translucent materials mark their pixels in a reactive
mask, and the resolve trusts history less there.** They write no motion of their own and change as
they play or move, so history reprojected under them is other content; a growing, animating
explosion blended into it smears, and a thin, fast feature — a tracer, a field of drifting motes —
is dropped from it. Under every temporal mode `TaaResolve` owns a render-allocation
`ReactiveMaskFormat` (R8) image beside the lit target, the first translucent pass clears it at
begin as it clears the bloom mask — that pass records every frame, a frame drawing no translucent
included, so it is the clear's owner in every topology — and each writer adds its coverage — or its luminance where
an additive draw carries light with little coverage — into it as one more attachment: the sprite,
particle and ribbon passes through their `*Reactive` fragment entries (`SpriteReactiveCoverage`, and
the ribbon and tube stages' own), and a Translucent material declaring `"reactive": true` at its
SV_Target2 (see "Additive translucent materials"). Post-resolve placements write none: they draw
after the resolve and never enter its history. The engine resolve takes the largest value of its 3×3 render texels and moves
its result toward this frame's reconstruction by `ReactiveCurrentShare` (0.9) of it, so a fully
covered pixel keeps a tenth of its history and its jitter still averages; MetalFX takes the same
image as its reactive mask. Nothing changes with anti-aliasing off, where no mask exists.
`tests/gpu/sprite_pass.cpp` holds that a sprite appearing over a settled history reads most of its
tint on that frame; `tests/gpu/ribbon_pass.cpp` and `tests/gpu/translucent_reactive.cpp` read the
mask back (`SceneRenderer::GetReactiveMaskView`) to hold that a ribbon and a reactive material write
it where they draw — under TAA, TAAU and MetalFX, with and without the bloom mask — and a plain
material does not.

`tests/gpu/taa_stability.cpp` holds the result: under a static camera, at render scale 1 and 0.5 in
both modes and under TAA with a depth-of-field composite after it, the converged resolve changes by under a few
half-float steps per frame inside geometry and in the open sky, a star keeps its place to 0.05
output px, and an edge to 0.1.
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
vector for geometry (camera **and** object motion) and the direction reprojection above for the
cleared background. Because velocity is a g-buffer channel it is **always
written and always allocated** (the cost is one extra `RG16Sfloat` target plus a clip-position
write, not a second geometry pass); with TAA off it is written but unread unless the reflection
denoiser runs, and the `MotionVectors` debug arm blits it directly. The opaque material contract is therefore **G0/G1/G2/G3/G4** —
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
output UV into the current frame's sub-rect in render pixels (`ExtentParams.xy`) for the
current/depth/velocity loads, while the **history and output cover that allocation in full** (so
the history read is the plain reprojected UV, and the velocity — full-frame motion — needs no remap
against it). The history is allocation-sized, so a
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
  hi-Z pyramid, SSAO, the lit target, the depth-of-field chain, the refraction grab
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

Between the two sits the **HDR scene colour** — what the temporal resolve, the HDR-placed point
fields and the depth-of-field composite write. It is allocated in the render
allocation, except under a promoting temporal-upscaling resolve (below), where it is the
post-resolve one; `SceneView::SceneColorExtent` is its valid extent this frame.

**The promotion is the bridge, and it is a pass only when the two differ.** `SceneUpscaleScenePass`
reads the finished HDR scene colour and writes a post-resolve-allocation copy the tail reads —
one bilinear tap through the scene colour's own map (or MetalFX's spatial scaler under
`UpscaleFilter::MetalFXSpatial`, see "MetalFX"), the same filter the terminal tonemap applied
when it carried this upscale, so the image is unchanged and the upscale is not performed twice. It
is declared at the HDR tail anchor, **after** the depth-of-field composite and **before** the
post-process effect chain, so every g-buffer-reading pass is upstream of it and the whole tail is
downstream. Below it the tail composes in a fixed order: the post-process effects, the post-resolve
ribbons (see "Ribbons and trails"), the post-resolve translucent layer (see "The post-resolve
translucent layer"), the pre-bloom GUI overlay, then bloom and the tonemap. It is wired only when the scene colour is not already the post-resolve allocation —
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
post-resolve allocation the post-resolve ribbons, a post-resolve translucent material declaring
`"bloomMask": true`, and a `SceneHdrPreBloom` overlay whose composite material declares it add
amplitudes to it. So the mask is resampled from the
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

**The remaining `drsSupported` exclusions are about the sub-rect, not the allocation.** The GPU
hi-Z occlusion test is not sub-rect-aware and forces the per-frame render scale to 1; it still
renders into a *reduced render allocation* perfectly well, so a static render-scale reduction
reaches it. A composited depth-of-field chain joins it **only behind a temporal resolve**: that resolve reconstructs the whole render allocation, so the chain would be
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
decides: it is TAAU **unless** the frame also wires a composited depth-of-field chain. That chain
sits between the temporal anchor and the tail and reads render-resolution depth beside the scene
colour, so a resolve that promoted at the anchor would hand it allocation-resolution colour; such a
frame resolves at the render allocation like TAA and takes the spatial promotion instead. That
degrades the reconstruction from temporal to spatial and keeps every other property, where the same
combination previously gave up scaling altogether.

#### MetalFX

**`MetalFXTemporal` is TAAU with Apple's MetalFX temporal scaler as the resolve.** It sets
`TaaActive` like the other temporal modes, so the jitter, the lit target, the stored velocity and
both allocations are TAAU's unchanged, and `FrameTopology::MetalFxTemporal` swaps only the resolve:
`MetalFxTemporalScenePass` takes a conditioned copy of the lit target, the g-buffer depth and a
motion target and
reconstructs the scene-colour allocation into the target the tail reads (the HDR target, or the
depth-of-field intermediate behind a composited chain, where it resolves at the render allocation as
TAAU does). The scaler keeps its own history, so no history target is allocated or imported and no
history copy runs; the renderer's history-reset gate becomes the scaler's `reset`. The reactive mask
(above, under TAA) is the scaler's reactive mask texture, on systems whose MetalFX takes one
(macOS 14.4 and later); elsewhere the scaler is created without it. The per-frame
parameters are derived from what the frame already publishes: the content size is the rasterized
sub-rect (`SceneView::RenderExtent`), the jitter is `JitterNdc / 2` of it in pixels with the same
signs (measured — the opposite sign leaves the jitter in the image, a pixel of edge wander), the
motion scale is minus the sub-rect, since velocity is current minus previous in viewport UV and the
scaler wants the step back in content pixels, and depth is reversed. **The scaler is handed the
tonemap's exposure** — the one the TAA resolve's luminance weights read — through its exposure
texture (one 1×1 per frame in flight), with its own metering off, and returns linear HDR. Metering a
mostly black frame itself, as space is, it picked an exposure far above the tonemap's, saturated
bright highlights in its internal tone curve and shifted their hue toward red;
`tests/gpu/taa_stability.cpp` holds a warm highlight's hue against a near-black sky.

**The scaler reads a motion target, not the velocity channel.** The velocity channel is cleared to
zero wherever the surface pass left the cleared depth, and the engine's resolve never reads it there:
it reprojects the background as a direction through `PrevFromCurDirection`. Handed the raw channel,
the scaler would take the sky — and everything drawn over it without writing motion, the
translucents and point fields — to stand still on screen however the camera turned, and smear it.
`MetalFxMotionScenePass` (`metalfx_motion.frag`) therefore writes a render-allocation target that
copies the velocity where geometry stands and fills the cleared depth with the camera's turn, in the
velocity channel's own convention, so the two temporal paths agree about the background. The
velocity channel itself is untouched for its other readers. What the fill cannot give a draw over
the background is its own motion — a translucent's parallax as the eye travels — which the camera's
turn dominates at a distance. `tests/gpu/taa_stability.cpp` holds a point-field star over open sky
under a turning camera to leaving no trail in either temporal resolve.

**The scaler reads a conditioned colour, not the lit target.** The engine's resolve damps the texels
the tone curve cannot show — it weights each sample by its exposed luminance and clips history to
the neighbourhood — so a lone specular spike, a non-finite channel or a negative one never reaches
the screen through it. The scaler does neither, and reconstructs each into a saturated pixel in one
hue (a NaN or negative green and blue become pure red through the tonemap's `max(hdr * E, 0)`). The
same pass therefore writes a second render-allocation target from the lit target: a non-finite
channel zeroed, a negative one clamped to zero, and a luminance past half of
`SceneView::BloomFireflyCeiling` (over the resolved exposure) rolled off toward it along the bloom
bright pass's shoulder, hue kept. That ceiling is the one the bloom pass already holds its taps to,
four times past the white point beyond which the tone curves resolve nothing, so ordinary content
passes unchanged to both the tone curve and bloom.

**`UpscaleFilter::MetalFXSpatial` is the promotion as MetalFX's spatial scaler.**
`Settings.Upscale` picks the filter that carries the scene colour across the boundary on a frame
whose resolve is not itself the promotion; `FrameTopology::MetalFxSpatialPromotion` replaces the
colour's bilinear pass with `MetalFxSpatialScenePass` (HDR colour processing over the scene colour's
valid extent), and the bloom mask, which MetalFX does not carry, takes its own "Bloom Mask Upscale"
pass. The filter does nothing under `TAAU`/`MetalFXTemporal`, and composes with every other mode.

**Both are owned by `MetalFxUpscale`, and both fall back where MetalFX cannot run.** It reads the
device's support once (`Backend::QueryMetalFxSupport`), holds each scaler only while its mode is
wired, keeps one across a reconfigure that moved neither allocation, and records a shape MetalFX
refused. `ClampSettings` runs every request through `ResolveMetalFxSettings`, so on a device without
MetalFX — any non-Apple platform, a system without the framework, a device the scaler does not
support, or allocations outside its scale range — `MetalFXTemporal` runs as `TAAU` and
`MetalFXSpatial` as `Bilinear`, and `GetSettings()` reports what runs. `ResolveRenderScale` floors a
dynamic-resolution frame's scale at what the temporal scaler's maximum content scale allows.

**The scalers run outside Vulkan, between two submits of the frame.** Their inputs and outputs are
created `ImageInfo::PlatformExport` (and storage-writable where a scaler writes them) only while a
MetalFX choice is configured, so no other configuration pays for the usage. Each pass calls
`Context::SplitFrame` (see "Splitting a frame for platform GPU work"), and its encoder commits a
command buffer on the scaler's own `MTLCommandQueue` that waits the split's shared event, encodes the
scaler, and signals it. The pass's declared accesses (sampled inputs, a storage-written output) place
the graph's barriers on either side as for any compute pass. `Backend/MetalFx.{h,mm}` is the one
translation unit that names MetalFX, which is weak-linked; its header is Vulkan- and Metal-free and
compiles to inert stubs elsewhere. `tests/gpu/taa_stability.cpp` holds the temporal scaler to the
TAA cases' stillness claims on bounds sized to the scaler's own residual, and the spatial scaler to
placing a half-scale render where native lies.

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
split the camera frustum into depth slices, each cascade fit (a bounding sphere on a radius ladder,
snapped to whole texels in the frame the scene rests in) to its slice and rendered into a depth **atlas** in **one** pass (per-cascade viewports); the
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
carries `MaxLights` (128) lights, and every pixel the lighting pass shades visits each of them its
tile has not culled, so a slot spent on a light that adds nothing costs pixels across the screen. `PackSceneLights` skips a light of zero
radiance, a positioned light of non-positive range, a light lighting no light channel, an area light
whose emitter has no area, and —
given the camera frustum — a positioned light whose range sphere (grown by its emitter's reach)
misses it. The rest are ranked by the radiance each delivers at the camera position (inverse square
from the emitter's surface, clamped one world unit out; deliberately without the range cutoff, which
would score zero every light whose range ends short of the camera but not of what it sees), the top
`MaxLights` are packed in scene iteration order, and the excess is counted in `DroppedLightCount`. A
Rect or Polygon light also packs its world-space vertices into the view's area-vertex buffer, capped
at `MaxAreaVertices` (eight per light, so a view of `MaxLights` Rects always fits); a Polygon that would
overflow it packs with no geometry and lights nothing.
**The loop skips what cannot light a pixel** (`EvaluateDirectLighting`, `Veng/lighting.slang`): a
light sharing no light channel with the surface (see "Light channels"), a light of zero radiance, a pixel at or past a light's range (for an area light, tested before the LTC
integral), a pixel outside a spot's cone, and a punctual or directional light behind the surface
each return before the shadow lookup and the BRDF. Each skip is exact — the term it skips is
zero — so it changes cost, never the image.

**Before the loop, a tile cull drops the lights that cannot reach a tile at all**
(`Settings.LightTileCulling`, on by default; `LightTileCuller`, `light_tile_cull.comp`). One compute
workgroup per `LightTileSize` (16) pixel tile — the "Light Tile Cull" pass, declared by the lighting
pass ahead of itself — reduces the tile's depth to its nearest and farthest geometry, bounds the
slice of the tile's frustum between them by the view-space box around its eight corners, and writes
a record per tile of two `LightMask`s, each `MaxLights` bits over `LightMaskWords` (4) `u32` words:
in the first, bit *i* is set when the view's *i*-th packed light's influence sphere reaches the
box. The lighting pass (`EvaluateDirectLightingMasked`) then visits only the set bits, in index
order, so a full mask sums exactly what the unmasked loop does. Workgroups are 256 threads and a
view holds at most 128 lights, so each light is tested by a thread of its own. **The bound is conservative, so the
image never moves:** a shaded pixel reconstructs its position at a pixel centre strictly inside the
tile's edges and at a depth inside its range, so it lies in the box; a point or spot light lights
nothing past its range (a spot stays sphere-bounded, its cone untested); an area light's sphere is
its range grown by its emitter's reach (a Sphere's radius, a Rect's or Polygon's farthest vertex
from its position), since its cutoff is measured from the emitter's surface; and the sphere grows
by a `1e-5` relative epsilon over the camera-relative magnitudes to cover the float error of the
lighting pass's own world-space reconstruction. A **directional light has no range and is in every
tile's mask**; a tile showing only sky (cleared depth) gets no positioned light. Which arms cull is
`FrameTopology::LightTileCullActive`: every arm whose lighting pass shades direct light (Final, Bloom,
CoC), never the cascade-tint or IBL-only variants that discard it.

The masks live in a device-local storage buffer with **one region per frame in flight** (a renderer
executes once per frame, so a frame never writes a region an in-flight frame reads), laid out
row-major over the tile grid of the render allocation, so a dynamic-resolution sub-rect needs no
reallocation. The cull writes through a set binding its frame's region alone (one set per frame in
flight, the region stride rounded up to the storage-buffer offset alignment), so its write is scoped
to the words that frame owns — which is also the range synchronization validation checks it over.
The buffer is registered in the set-0 storage-buffer array, and the view block's
`LightTiles` names it — slot, row stride in tiles, this frame's first word and the extent the tiles
cover, or `LightTilesNone` when no cull ran — so any pass can find a pixel's masks through
`Veng/light_tiles.slang`. It is imported into the
graph: the cull's `StorageBufferWrite` and the lighting pass's `StorageBufferRead` (which, on a
graphics pass, is `AccessKind::StorageBufferReadGraphics`, a vertex- and fragment-stage scope) derive the barrier.
**A record's first mask describes the opaque depth range**, so it serves the deferred pass. **Its
second, the forward mask, serves what is drawn in front of it**: the lights whose sphere reaches the
tile's frustum anywhere from the camera to the plane through its farthest opaque surface (with no far
plane where any of the tile's pixels shows sky, since a fragment over one is drawn at any depth).
The frustum's side planes are pushed a pixel out, so a fragment
projected through another jitter — the post-resolve layer's unjittered projection — still lands in a
tile whose frustum holds it. A forward-lit translucent fragment (`EvaluateForwardLighting`) finds its
tile by projecting its world position (`ForwardTileLightMask`), since its layer may render at another
resolution than the opaque pass the tiles were laid over; the view block's `LightTiles.w` carries that
extent. Every translucent pass declares a `StorageBufferRead` on the masks (`PassIO::LightTileMasks`),
so the graph orders the cull ahead of it. Sprites and particles are lit per sprite or per particle,
not per pixel. A sprite loops the view's lights with its own range test. A particle's update visits
the union of the forward masks over every tile its drawn extent's projection covers
(`ForwardSphereLightMask`): a light reaching any visible part of the particle is in the mask of a tile
it covers, so the union keeps it wherever the particle's centre falls. A particle wholly off screen
takes the directional lights alone; one reaching the camera plane, or covering more than 64 tiles,
takes every light. The update declares a `StorageBufferRead` on the masks, ordering it after the cull.
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
casters; the XY extent is the frustum slice).

**A cascade holds still against the scene.** A point at rest lands at the same sub-texel position in
its cascade frame after frame, while the camera translates or turns, so shadow edges do not crawl:

- **The size moves only in ladder steps.** Each slice's bounding sphere is measured in view space,
  so for given splits its radius does not depend on the camera's orientation, to the bit. The radius
  rounds up onto a fixed ladder, `2^(n/8)` world units (`CascadeStepRadius`,
  `CascadeData::RadiusStep`), with hysteresis against the set's last fit (`CascadeFrame::Previous`):
  a need that grows takes its step at once, and one hovering within a step below the held one keeps
  it. The cost is at most two steps of radius (about 19 %) of texel density; the splits and
  `SplitFar` are the true fit's, so cascade selection is unchanged. The renderer keeps each set
  index's last fit, dropped on a change of resolution (a change of count is ignored by the fit).
- **The pitch is the raster pitch.** The texel size is `2·r / (Resolution − 4)` and the box is exactly
  `Resolution` of them wide, so the sphere keeps two texels beside it on each side — half a texel for
  the snap's move and one for the guard band. `TexelWorldSize` is that pitch exactly, which the
  lighting pass's bias and `LightShadowMapAt`'s texel size read; a medium spacing its taps by it sees
  it change only in ladder steps.
- **The snap is real, in the frame the scene's content rests in.** The light basis depends on the
  light alone (`CascadeLightBasis`), and the box centre is snapped to whole texels on a grid fixed in
  the **stable frame**: scene space for a fixed-origin scene, or, for a scene carrying a
  `FloatingOrigin` (`Veng/Scene/Components/Environment.h`, read once per `Execute` through
  `SceneFloatingOrigin`), scene space displaced by its `Position`. Only the grid's phase is taken
  from the origin, in double precision; everything else stays in small render-space magnitudes. So a
  consumer that re-centres its world on the viewer each frame still sees its distant scene hold still.
- **The cascades covering the viewer's own geometry ride the viewer.** Every cascade whose slice
  begins nearer than `FloatingOrigin::ViewerFrameReach` snaps in scene space instead
  (`CascadeData::ViewerFrameCount`, always a prefix, recomputed from each fit's splits), so geometry
  the viewer carries holds still too. The split between the two frames needs no handling: the
  lighting pass already cross-fades each cascade into the next across the blend band, both samples
  are correct shadows differing only in sub-texel phase, so a receiver's slight shift fades in by the
  blend weight rather than across a line. The default reach of zero puts no cascade in the viewer's
  frame.

The punctual lights add the second arm: a **spot**
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
shadows unless it says otherwise; cleared, the light scores zero and is passed over for every arm,
and it packs `LightFlags::CastsNoShadow` — a Directional holding no cascade set would otherwise read
its set index as 0 and be shadowed by another light's cascades.
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

**A light lights only the surfaces sharing a light channel with it.** `Light::Channels` (a `u8`,
default `0x01`) is the set of channels the light lights, three of them (`LightChannelCount`); the
packed light carries it in bits 6–8 of its flags word (`LightFlags::ChannelMask`, `LightChannels` in
`Veng/light_flags.slang`), and a light naming no channel is not packed. A surface's membership is a
material property, written into its surface flags by `SurfaceLightChannels(mask)`
(`Veng/surface_flags.slang`, see "The deferred opaque material g-buffer contract") or, for a
forward-lit surface, `ForwardSurface::LightChannels`; every surface is in channel 0 alone unless its
material says otherwise. `AccumulateLight` returns before its shadow lookup and its BRDF when
`(light channels & surface channels) == 0`, so the deferred pass, the forward loop and anything else
built on the core agree, and a skipped light costs one test. Sprites and particles carry no
membership and are members of channel 0. The tile cull is per tile and ignores channels. It is the
standard tool for keeping a light off a set of surfaces — an interior fill that must not light the
exterior seen through a window, a rim light reaching one character — and the half of a self-lit
surface's contract that keeps the light it computes itself from being added twice (see "A Surface
fragment may read the shadow maps"). A light either lights a surface or does not: there is no
per-term exclusion. `tests/gpu/light_channels.cpp` pins the memberships under a channel-0 and a
channel-0|1 light, deferred and forward, and that naming every channel lights a default surface
exactly as channel 0 does.

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
`Cone.w` with its light channels, keeping `LightStride` fixed; the bit meanings are `Renderer::LightFlags` and its shader
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

**The grab copies the lit scene and its opaque depth ahead of the translucent pass**, into a
full-extent `HdrFormat` colour target and an `R32Sfloat` depth target, and publishes both in the
view block's `SceneColor` slot. A Translucent fragment reaches them through `Veng/translucent.slang`
— `SampleSceneColor` for what is behind it, `SampleSceneDepth` to reject a distorted sample whose
geometry stands in *front* of the refractor, or to find the surface a medium or a soft edge meets.
The depth readers live in `Veng/scene_depth.slang`, which `translucent.slang` includes and any other
pass drawn after the copy may include on its own (the sprite pass does), beside
`ViewDepthFromDeviceDepth`, the positive view depth of a reverse-Z sample solved from the
projection's depth and w rows.
The copies predate the translucent pass, so one translucent surface never refracts another.

**The depth has three reads, chosen by what the value must be** (all in `Veng/scene_depth.slang`,
all returning the far plane, 0, when the copy did not run):

- **`SampleSceneDepth(vc, uv)` is filtered**, through the grab's linear sampler. It is right for a
  soft fade, which wants a smooth value, and wrong for a ray march's end point: off a texel centre
  — under a render scale, and at every fragment of the half-resolution layer, whose UV falls on the
  corner of four texels — it blends neighbours, and across a silhouette the blend is a depth no
  surface has (a surface at distance D against the far plane reads about 2D).
- **`LoadSceneDepth(vc, uv)` is a point load** of the copy texel the UV falls in, in the
  `LoadSurfaceFlags` shape: always a depth some surface wrote.
- **`SampleSceneDepthNearest` / `SampleSceneDepthFarthest(vc, uv, extent)`** take the nearest or
  farthest of the copy texels whose centres lie in the footprint of a fragment on a target of
  `extent` pixels — `vc.ExtentParams.xy` for the drawing fragment's own target, which makes it one
  texel at full resolution and the 2×2 in the half-resolution layer — as four point loads (the first
  two texels per axis, so a target coarser than half the copy is under-covered). A march that must
  agree with the half-resolution composite, which depth-tests and weighs the layer by the farthest
  of each 2×2, ends at the farthest; one that must never pass through a near surface ends at the
  nearest. Over an even valid extent the footprint is exactly the reduce's 2×2; over an odd one it
  sits up to a texel short of the reduce's blocks along that axis's far half, since the layer's UVs
  divide by the rounded-up half extent while the reduce reads `2·i`.

Each feeds `ViewDepthFromDeviceDepth`, under which the far plane of an infinite projection reads as
`+inf` and that of a finite one as its far distance. The GPU case "the point and footprint depth
reads return written depths" holds each read to the copy bit for bit, at both resolutions, both
render scales and both projections.

**The two copies are separate passes behind separate gates.** The colour copy (`Scene Color Copy`)
and its blur chain exist only under `Settings.Refraction`. The depth copy (`Scene Depth Copy`) is a
single-attachment R32 pass wired wherever the translucent composite is — the Final view and the
Bloom debug arm (`FrameTopology::SceneDepthCopyActive`) — with no look setting in the way, at the
cost of one R32 target at the allocation extent. So a scene that wants depth but not refraction can
have it.

**Each copy runs only on a frame something samples it.** A material declares what its fragment
reads in its `.vmat.json` (Translucent domain only, a cook error elsewhere): `"readsSceneColor":
true` for the colour (`Material::IsSceneColorReader`), `"readsSceneDepth": true` for the depth alone
(`Material::IsSceneDepthReader`). The gather reads both off every draw in the translucent and
half-resolution plans, and a soft-edged sprite (`SpriteDrawPlan::ReadsSceneDepth`, gathered ahead of
it) reads the depth too; `ResolveSceneGrabRecording` (`FrameTopology.h`, pure) decides what
records:

- **a colour or a depth reader** records the depth copy — a refractor depth-gates its samples, so a
  colour reader gets the depth too;
- **a colour reader, under `Refraction`,** also records the colour copy, every halving pass and the
  coarse-tail dispatch — the whole grab;
- **nothing reading** records none of them.

A depth-only frame therefore costs one single-attachment fullscreen copy and no blur chain. The
passes skip through `SkipWhenUnread` (below), and the view block's two availability flags —
`SceneColor.z` and `SceneDepthCopied` — are patched to 0 after the gather for a copy that did not
record, so `SceneColorAvailable` / `SceneDepthAvailable` are false that frame and a fragment that
samples without declaring reads black or the far plane — never a stale grab, nor a level left in an
undefined layout. A depth-only material must not call the colour helpers; the cook does not police
that. Most frames of most scenes draw no reader, and pay nothing for either. A half-resolution
translucent reads the same full-resolution copies at its own UVs.

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

**The colour copy and its chain are a `SceneColorChain`** (`SceneColorChain.{h,cpp}`), which owns
the image, its per-level views and slots, the chain sampler, the copy, halving and coarse-tail
pipelines, and declares the passes that fill it. The grab owns one at the render allocation, filled
over the frame's sub-rect (`SceneColorChainRegion::RenderSubRect`); the pre-bloom GUI overlay pass is
handed another at the post-resolve allocation, filled whole (`WholeAllocation`) — see "The pre-bloom
GUI overlay".

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

**The layer composites at its place in the back-to-front order, in segments.** Every translucent
draw sorts in one order — priority, then depth, then a half-res draw ahead of a full-res one at an
equal depth (`TranslucentDrawsBefore`), so a planet's ring and its own shell, both sorted by the
planet's centre, always draw shell first — and `SegmentTranslucentDraws` (`DrawGather.h`, pure)
cuts that order into **segments**. Each run of consecutive half-res draws is one segment: drawn
into the layer and composited over everything before it, so a near atmosphere lies over a far
ring and a cockpit pane over an atmosphere. A full-res draw after a run whose screen rectangle
(its submesh bounds projected, `TranslucentDraw::ScreenRect`; the whole screen when a corner is
behind the eye) does not overlap the run's draws **ahead of** the run's composite, so a segment
begins only where a full-res draw and a half-res run genuinely overlap and alternate in depth. A
frame has at most `MaxHalfResSegments` (4); further runs fold into the last, and the full-res
draws between them draw over them (`SceneRenderer::GetTranslucentSegmentStats`). The order is
object-level, as the full-res pass's always was: each draw sorts by its centre — or, for an
entity whose `MeshRenderer::SortDistance` is set, at that view depth. That is what an impostor
states: a distant body drawn on a fixed-distance shell and scaled to hold its apparent size is drawn
nowhere near where it stands, so its drawn depth cannot order it against another such body, and the
system that placed it supplies the distance it does stand at.

The passes, in both compositing arms (`DeclareTranslucents` in `Rebuild`):

1. **The first full-resolution `TranslucentScenePass`** (`TranslucentPassKind::First`) — the draws
   behind the first run, and those clear of it. It records every frame and clears the bloom mask
   and the reactive mask.
2. **A depth reduce**, once, while the layer is wired: a half-res D32 target as the farthest
   (reverse-Z minimum) of each texel's 2×2 full-res opaque depths, so the layer's draws
   depth-test conservatively — a fragment survives wherever any of its four full-res pixels would
   show it.
3. **Per segment, the layer's own `TranslucentScenePass`** (`HalfLayer`): half-extent viewport into
   the half-res HDR target cleared to transparent, per-parent pipelines, and straight alpha blend —
   which over a transparent clear leaves exactly (premultiplied color, coverage). No bloom-mask
   attachment: the layer lands in the lit color ahead of the bloom bright-pass, so its glow rides
   the scene like an unmasked translucent's.
4. **Per segment, a depth-aware composite** upsamples the layer to full resolution — each 2×2
   tap's bilinear weight collapsed in proportion to its reduced depth's distance from the pixel's
   own opaque depth, so the layer hugs geometry edges instead of haloing across them — and blends
   it into the lit scene color with `(One, OneMinusSrcAlpha)`, **scissored to the segment's
   rectangle** grown by `HalfResCompositeMarginPixels` (`HalfResCompositeScissor`), so a far
   planet's shell costs about its disc.
5. **Per segment, a further full-resolution pass** (`Continuation`) — what lies over that run and
   before the next. It loads the bloom mask and skips a frame it draws nothing in.

Segment 0's passes keep their names; a later segment's carry its index ("Scene Translucent Half 1",
"Half-Res Translucent Composite 1", and "Scene Translucent N" for the full-res pass after segment
N − 1). The layer target is one image cleared per segment, and the full-res passes share one
pipeline cache, so a material drawn in several segments builds once. A frame whose half-res draws
all lie behind every full-res draw they overlap records the passes it did before — reduce, layer,
composite, then a full-res pass — plus the first full-res pass, which records even when it has no
draws because it clears the bloom and reactive masks. An empty half-resolution segment needs no
continuation of its own: a segment only advances past a run that holds draws, so the full-resolution
pass after an empty segment is empty too and skips, and the full-resolution work stays in the pass
before it.

**Ribbons, sprites and particles share one render pass, "Scene Layers"** (`SceneLayersScenePass`,
`Passes/SceneLayersScenePass.h`), recorded after the translucent passes in that order. Each is a
`SceneLayer` — a `ScenePass` that can also declare what its draw reads into a shared pass and record
into it — and the three load and store the same lit target, bloom and reactive masks and read-only
depth and sample none of each other's output, so one pass spares the frame a full-resolution round
trip of every attachment per layer. Work a draw depends on (the particle simulation's compute chain)
is declared ahead of the pass, and the pass skips a frame no layer draws in. The layers stay a pass
apart from the last translucent pass: those pipelines bind the translucent passes' attachment
layout, with an unbound bloom-mask location when bloom is off and the g-buffer's flags beside it
under metering exclusion, which the layers' pipelines and fragment entries do not share.

**The layer renders through a second view-constants region.** The one block field a half-res
fragment must read differently is `ExtentParams`, which carries the half extents so an
`sv_position` mapped through it lands on the same UV a full-resolution draw's would — every other
field (matrices, `RenderScaleUV`, the refraction-chain handles) is the full view's, so UV-space
reads like `SampleSceneColor` work unchanged. The region is claimed **before** the render's own
slot, because every pass reading `GetCurrentViewConstantsIndex()` at record time must land on the
full region; a frame whose view budget refuses the extra claim folds the layer's draws back into
the full-res plan for that frame, the same fallback as the activation edge.

### The post-resolve translucent layer

**A Translucent material may draw after the temporal resolve instead of with the scene** —
`"placement": "postResolve"` in its `.vmat.json` (default `"scene"`; Translucent-domain only, and
exclusive with `"resolution": "half"`, `"readsSceneColor"` and `"readsSceneDepth"`, each a cook
error). The cooked header's layer word carries it (`TranslucentLayer::PostResolve`), and
`Material::IsPostResolve` reports it. It is the mesh counterpart of a post-resolve `RibbonPath`: a
solid part of a holographic instrument, a gizmo's handle, anything drawn as geometry that must stay
as crisp as the linework around it.

- **Where it draws.** A `TranslucentScenePass` of kind `PostResolve`, **"Post-Resolve
  Translucent"**, is declared at the HDR tail anchor immediately after the post-resolve ribbons and
  before the pre-bloom GUI overlay, into the effect chain's output at the **post-resolve
  allocation**, in the Final arm only. It loads the post-resolve bloom mask (promoted or not), so a
  material declaring `"bloomMask": true` glows through it as a post-resolve ribbon does.
- **How it projects.** It renders through a **view-constants region of its own**, claimed beside
  the half-res layer's by the same rule (`ExtraViewRegionFits`: only while a slot remains for the
  render's own region) — the render's block with `Proj` and both inverse view-projections
  unjittered, `JitterNdc` zero, and the extents and UV maps the tail's own (an identity sub-rect).
  The surface vertex stage therefore projects through the unjittered matrix with no shader change,
  and a fragment mapping `sv_position` through `ExtentParams` lands on the output's UV.
- **No depth.** The post-resolve allocation carries no depth buffer, so the pass binds none and
  its pipelines test none: a post-resolve surface draws over every scene surface, sorted back to
  front among its own kind by the ordinary translucent order. That is the layer's point — an
  instrument reads whole inside the geometry it stands in — and a material wanting occlusion
  belongs in the scene placement.
- **Content-driven, with a transition frame.** The translucent gather routes a post-resolve
  material's draws into a plan of their own; the pass is wired the first `Execute` that gathers one
  and unwired after `RibbonIdleFrameLimit` empty ones. On the activation frame — and on any frame
  the view budget refused the layer's region — the draws fold into the full-resolution plan
  (`MergeTranslucentPlans`), drawn depth-tested and jittered for that one frame rather than
  dropped.

`tests/gpu/post_resolve_translucent.cpp` draws a post-resolve cube behind an opaque one and checks
that the activation frame hides it in the scene pass and every later frame draws it, through its
own pass, over the occluder at full and reduced render scale; `tests/cooker/material_cook.cpp`
covers the key and its three exclusions.

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

**A Translucent material may write the temporal resolve's reactive mask** — `"reactive": true` in
its `.vmat.json` (Translucent domain, full-resolution scene layer only: exclusive with
`"resolution": "half"` and `"placement": "postResolve"`, each a cook error), read as
`Material::IsReactiveWriter()`. Its fragment returns `TranslucentReactiveOutput` or
`TranslucentBloomReactiveOutput` (`Veng/translucent.slang`), whose reactive amount is SV_Target2
and `TranslucentReactiveAmount` the usual value. Under a temporal resolve the full-resolution
translucent passes bind the mask as their third attachment, location 1 being the bloom mask or left
unbound, and enable its writes for a declaring material's pipeline alone. It is for a thin, fast or
animating surface the history would drop or smear; a large surface should not declare it, since a
reactive pixel keeps little of its history.

**A Translucent material may keep the exposure meter from reading it** — `"excludedFromMetering":
true` (Translucent domain, full-resolution scene layer only: exclusive with `"resolution": "half"`
and `"placement": "postResolve"`, each a cook error), read as `Material::IsMeteringExcluder()`. Its
fragment returns `TranslucentMeteringOutput` (`Veng/translucent.slang`), whose SV_Target3 is
`TranslucentMeteringFlags(excluded)`: zero colour and, where it flags, `SurfaceFlagExcludedFromMetering`
in alpha. The full-resolution translucent passes then bind the g-buffer's ORM as a fourth attachment,
locations 1 and 2 being the bloom and reactive masks or left unbound, and blend it with MAX on every
channel, with writes enabled for a declaring material's pipeline alone: the zero colour leaves the
stored occlusion, roughness and metallic as they are, and the alpha raises the flags code to one
carrying the meter's bit. That can clear the two lighting flags beneath it, which only the deferred
lighting pass reads, and it has run; the screen-space reflection chain and SSAO read ORM before the
translucent passes too, so the meter is the only reader after them. It is for a light source drawn as
a translucent surface over the scene — a glowing wall, a bright medium — which the meter would expose
for, and a fragment flags only the pixels it makes bright, since a frame whose every pixel is flagged
leaves the meter nothing to read and holds the exposure. **The binding is content-driven**:
`SceneRenderer::Internal::MeteringExclusionActive` is set by the first gather that holds a declaring
draw — the rebuild lands ahead of that frame's replay, so the draw writes its flags on the frame it
first appears — and cleared after `MeteringExclusionIdleFrameLimit` gathers with none, so a frame
drawing no such material pays no load and store of the ORM target in each translucent pass. It binds
only while auto-exposure is wired. `tests/gpu/translucent_metering.cpp` holds that a bright
translucent beside a lit cube sets the exposure when plain and does not when declaring.

**An output's location is checked against its SV_Target index at cook.** Slang numbers an entry's
outputs in declaration order unless every one carries an explicit `[[vk::location(N)]]`, so an
SV_Target2 after SV_Target0 compiles to location 1; the contract structs pin every location, and the
material importer refuses an output compiled anywhere but its own index.

### Flipbook sprites

**A `FlipbookSprite` component draws a flipbook as a camera-facing HDR quad** (the component and the
`Flipbook` asset are in [../Scene/CLAUDE.md](../Scene/CLAUDE.md) and
[../Asset/CLAUDE.md](../Asset/CLAUDE.md)). `SpriteScenePass` (`Passes/SpriteScenePass.h`) records
**after the translucent passes and the ribbons**, in the shared "Scene Layers" pass, in both
compositing arms, into the same lit target and bloom mask — so sprites composite over translucent
surfaces and ribbons, resolve under TAA with the scene, bloom, and tonemap.

- **Content-driven with deactivation hysteresis.** `GatherSprites` walks `View<FlipbookSprite>` each
  `Execute` (skipping a sprite whose flipbook or the textures it draws with are not resident, whose
  opacity is zero, or which has finished) into a `SpriteDrawPlan`; the pass is wired the first
  `Execute` that gathers one and unwired only after `SpriteIdleFrameLimit` consecutive empty gathers,
  because transient effects leave a scene momentarily empty between bursts and a recompile per burst
  is the cost the hysteresis avoids. A scene that never carries a sprite has no pass, so the smoke
  golden is unaffected.
- **One record per sprite, no vertex input.** Each gathered sprite is a 160-byte `GpuSprite`
  (`sprite_record.slang` mirrors it): anchor and width, tint and opacity, frame N's and frame N+1's
  atlas rectangles, the anchor fraction, height and roll, one tile's UV extent and the two frames'
  extents, the bindless texture handles (the colour sheet or `lightA`, `lightB`, motion, the
  temperature's texture) and one sampler, the alpha mode, a `SpriteFlag` word (six-way, the
  two-bit frame blend, the sqrt temperature transfer, temperature in red of its own texture,
  emits), the ramp index, the frame fraction, the motion scale, the soft distance, the emission
  intensity and the temperature offset. They sit in a host-mapped ring, one region per frame in
  flight bound through a per-frame set at set 3. The vertex stage indexes the record by
  `VertexIndex / 6` and places the quad corners about the anchor along the camera's right and up
  axes, rolled in the screen plane, projected through the jittered `Proj`; a flipbook whose frames
  differ in extent (`frameScale`) sizes the quad by the blended extent about the frame centre.
- **The emission-ramp table** is set 3 binding 1: one 144-byte `GpuEmissionRamp` (eight
  `(r, g, b, t)` keys and `(ignition, key count)`) per distinct flipbook default, keyed by the
  flipbook, and one per sprite carrying an override (`EmissionRamp` or `Ignition`). It is bounded by
  `MaxSpritesPerFrame` too, so it cannot overflow while the records fit; an override longer than
  eight keys is cut to its first eight with a once-per-renderer warning.
- **The shading model is one module, `Veng/sprite_shading.slang`,** so a flipbook looks the same
  however it is drawn. Its binding-free core, `Veng/sprite_shading_core.slang`, is the per-texel
  arithmetic — six-way weights, the lit combine, the temperature decode, the emission ramp and its
  ignition gate, the frame blend, the soft fade, and the unlit path — written as functions of plain
  values (and, for the frame blend, a texture and sampler handed in), so a tool previewing a bake
  includes it with no binding of the renderer's. The bound half adds the per-sprite light gather and
  the ambient, and reaches the scene through `Veng/medium_lighting.slang` (which re-exports
  `Veng/light_visibility.slang`: light buffer, shadows at
  set 5, IBL at set 4) and `Veng/scene_depth.slang`.
- **Lighting is gathered once per sprite, in the vertex stage.** A six-way sprite's frame is the
  camera's rolled right and up, `Z = X × Y` toward the viewer, centred on the frame centre with a
  radius of half its larger extent. `EvaluateSpriteLighting` walks the view's lights: a directional
  light arrives along its own direction; a point or spot light is attenuated by
  `PunctualLightFalloff` (`Veng/light_buffer.slang`, the inverse square with its source-radius
  floor, the range cutoff and the spot cone, shared with the surface loop's `AccumulateLight`) with a
  distance floor of the sprite's radius, and its weights blend from isotropic to directional by
  `saturate(dist / radius)`, so a light inside the sprite lights every face alike rather than
  blowing up. An area light arrives as a small source of its power from its centre
  (`SpriteAreaLightFalloff`): a Sphere's luminance over its disc, `L·πr²/d²`, and a Rect's or
  Polygon's over its area, foreshortened by the cosine it faces the sprite at (either face when
  two-sided, none behind a one-sided one), with the same distance floor, range fade and blend.
  Visibility is `LightVisibility` at the centre through the 3×3 PCF kernel, one lookup per light; a
  sprite is a member of light channel 0, so a light not on it is skipped. Every vertex computes the same gather from the same inputs, and
  it reaches the fragment flat: seven RGB terms, six axes and the ambient. The comparison sample at
  an explicit level of 0 is legal in a vertex function, and runs there on MoltenVK.
- **The six-way weights are the squared components** of the light's direction in the sprite's
  frame, `(x+², y+², z−²)` for `lightA` and `(x−², y−², z+²)` for `lightB`. They always sum to 1, so
  a light between two axes interpolates their maps and gains no energy, where plain components
  would brighten a diagonal by up to √3. Each axis gains `radiance · attenuation · visibility · w /
  π`, the surface core's Lambert normalisation, so a map value of 1 returns what a white diffuse
  card facing that light does.
- **The ambient** is the mean, over the frame's six directions, of the view's ambient arm for a white
  diffuse surface facing each — the flat floor, the SH skylight, or the IBL irradiance, with no
  Fresnel split since a medium has no specular lobe — and it multiplies the six maps' mean. The lit
  term is `Σ map · In + ambient · mean(maps)`, times the tint.
- **Emission** is the ramp's colour at the temperature `t = saturate(decode(stored) +
  TemperatureOffset)`, gated by `saturate((t − ignition) / fwidth(t) + 0.5)` (a hard step where
  `fwidth` is 0), times coverage and `EmissionIntensity`, added to the lit term. The gate's band is
  analytic and about a pixel wide, so it needs no anti-aliasing and is zero a pixel below ignition;
  `fwidth` is taken in the six-way branch, which is flat per primitive and so uniform across a quad.
  Emission is the only term meant to go above 1, and its contrast lives in the ramp, retuned at
  runtime.
- **The frame blend needs nothing temporal.** A crossfade reads each texture at frame N and N+1 and
  lerps by the fraction (`FlipbookFramePositionAt`). A motion blend works in extent-normalised space:
  the fragment at `w = (q − ½)·lerp(sN, sN1, f)` reads frame N at `(w − mN·f)/sN + ½` and frame N+1
  at `(w + mN1·(1 − f))/sN1 + ½`, each motion read from its own frame at the unwarped coordinate,
  each tap clamped into its tile's inset rectangle and sampled with the unwarped coordinate's
  gradients, so an edge in the motion field does not spike the mip. It relies on no accumulation and
  no dither, so it holds with anti-aliasing off. A flipbook without motion crossfades, and the blend
  acts on stored values, so shading runs once on the result.
- **The soft edge.** With a positive soft distance the fragment reads the opaque-depth copy, takes
  the gap to it in view depth (`ViewDepthFromDeviceDepth` against the interpolated view depth, exact
  across a flat camera-facing quad) and multiplies the premultiplied result and the mask by
  `saturate(gap / distance)`. A frame gathering such a sprite records the depth copy
  (`SpriteDrawPlan::ReadsSceneDepth`) whatever its translucents read; with no copy nothing fades.
- **Two draws.** The alpha set, sorted back to front on view-space depth, draws first through
  `BlendState::PremultipliedAlpha()`; the additive set follows, unsorted, through a `One, One` blend
  that keeps destination alpha. One stage serves both: the shading mode is a per-sprite flat branch.
- **The reactive mask.** Under a temporal resolve each sprite adds its coverage into the reactive
  mask (see "TAA"), the attachment after any bloom mask.
- **The bloom mask.** When the frame wires a mask (bloom on), an unlit sprite adds
  `saturate(luma(colour))` into it, so a bright sprite glows whatever the bloom threshold, and a
  six-way one adds `saturate(luma(emission))` times opacity and fade, so a hot core glows and sunlit
  smoke does not; the translucent pass ahead of it cleared the mask, and this pass loads it.
- **Sets 4 and 5.** The pass binds the renderer's IBL and shadow sets with the frame-slot dynamic
  offsets, exactly as the forward-lit translucent pass does, and declares the shadow atlases sampled
  by `AccessKind::SampleVertexFragment` — a barrier into the fragment stage does not order a
  vertex-stage read. The IBL irradiance cube and the dummy and punctual shadow atlases are released
  `SampleAny`, which names the vertex stage too.
- **What a schema-1 flipbook does by default.** `ResolveSpriteShading` (`FlipbookSystem.h`) resolves
  a sprite's `FrameBlend` (its own, or the flipbook's recommendation, which is `Step` for a schema-1
  source; `Motion` without a motion sheet becomes `Crossfade`) and its soft distance (half the quad's
  width for a six-way flipbook, off otherwise, unless the sprite names one). So an unlit stepped
  flipbook takes the same sample and the same arithmetic as ever, unlit, stepped and hard-edged,
  unless its sprite opts in.
- **Budget: `MaxSpritesPerFrame` (4096).** Past it the rest are dropped for that frame and the
  renderer warns once for its lifetime. Depth is tested against the opaque depth without being
  written.

`tests/gpu/sprite_pass.cpp` renders a white premultiplied flipbook through each blend and checks the
composited centre pixel against the tint, the additive sum, and the bloom-masked path; a schema-1
sprite is unaffected by a light, stepped and hard-edged; and a six-way flipbook built in code reads
`E/π` from a light along its lit axis and nothing from the opposite one, its baseline behind an
opaque caster (the vertex-stage shadow path), its ramp only above ignition, and a fade that rises
into a wall. `tests/gpu/sprite_shading.cpp` evaluates the core's functions in one dispatch.

### Particles

**A `ParticleEffectInstance` plays a `ParticleEffect` as many sprites and streaks flying out from one
anchor, simulated and drawn on the GPU** (the component, `ParticleEffectSystem` and the pool's
`EffectDesc::Particles` are in [../Scene/CLAUDE.md](../Scene/CLAUDE.md); the asset in
[../Asset/CLAUDE.md](../Asset/CLAUDE.md)). The battery is `ParticleSimulation`
(`ParticleSimulation.{h,cpp}`), drawn by `ParticleScenePass` (`Passes/ParticleScenePass.h`), recorded
**immediately after the sprites** in the shared "Scene Layers" pass in both compositing arms, into the same lit target and bloom
mask — so alpha particles composite over sprites, and resolve, bloom and tonemap with the scene.

- **One pool per renderer, of fixed capacity.** `SceneRendererSettings::ParticleCapacity` (default
  `DefaultParticleCapacity`, 65,536; a power of two, rounded down by the settings clamp; 0 disables
  particles, for a portrait; a `CaptureSurface` sets it unless the surface asks for particles,
  `CaptureSurface::Particles`). The pool's buffers are device-local and imported into the
  graph, so every barrier between the chain's passes is derived from their declared uses: the
  particles (48-byte `GpuParticle`), the dead list, two halves of live list, the additive list, the
  alpha sort keys (`max(N, 1024)` `uvec2`s), the 112-byte draw records, the counters and the
  argument buffer. Per-frame host-mapped rings — one region per frame in flight — carry the instance
  table (`MaxParticleInstances`, 1,024 × 64 bytes), the emitter table (`MaxParticleEmittersPerFrame`,
  256 × 1,424 bytes, each holding the emitter's parameters, one flipbook's handles, grid and
  shading, the ramp in the sprite pass's form and its two 32-sample over-life tables), the spawn
  ranges (`MaxParticleSpawnRanges`, 4,096), the fed spawns (`MaxParticleFedSpawnsPerFrame`,
  16,384 × 32 bytes), a frame-extent table and a stats region. About 12 MB at
  the default capacity, allocated only while the chain is wired.
- **Each particle's state is written once, at spawn.** Its position at any age is the closed form
  of constant acceleration plus linear drag (`ParticleOffsetAt`, below a `k·t` of 1e-3 a third-order
  series), so `p₀ + v₀·t` is exact with neither, the path does not depend on the frame rate, the
  update writes nothing back to the pool, and **any number of renderers reproduce the same
  particles**: an environment capture executing every few frames, a second viewport, or a renderer
  created mid-effect shows each particle where it is. The math is twinned line for line in
  `ParticleMath.h` and `particle_math.slang` (the PCG hash, a particle's key and random streams, the
  hashed value noise, `MakeParticleSpawnState` and the trajectory), the `InstancedCull.h` precedent,
  so the unit band pins what the GPU computes. A force neither constant nor linear in velocity would
  need a stepped state, which this does not have.
- **The gather is device-free** (`ParticleGather.{h,cpp}`, run each `Execute` after the sprite
  gather). Its instance table is keyed by `(entity, playback)`: a new key claims a slot and bumps
  its generation, a vanished key frees the slot and bumps it again, so its particles die at the next
  update by generation mismatch; the table resets on a new scene. **An instance draws on its
  `ParticleEffectInstance::Layer`**: one on a layer the view's mask leaves out, or the view's
  excluded entity, is tracked, issued and simulated as any other but marked
  `ParticleInstanceHidden`, so a view hiding it neither resets its emission nor leaves a backlog to
  issue when it shows again, and its soft emitters ask for no depth copy. **Spawns come from cursors, not a
  queue**: each slot keeps, per emitter, the burst and rate indices it has issued, and issues
  `[max(cursor, first possibly alive), spawned by now)` per stream (`ParticleSpawnedBy`,
  `ParticleFirstPossiblyAlive`), so each renderer issues every index exactly once whatever its
  cadence, and one meeting an effect late issues only what can still be alive, each at its exact
  spawn time. The emitter table holds each effect's emitters once per frame; an instance whose
  emitters do not fit keeps its particles alive but undrawn and spawns nothing, counted in
  `EmittersDropped`, which the renderer warns of once. An instance whose effect names a flipbook not
  yet resident does the same until it lands, counted apart in `InstancesWaiting` with no warning: an
  effect loaded asynchronously waits a few frames, which is not a budget.
- **Fed spawns ride the same ranges.** For an instance with a `ParticleFeed`, the gather issues each
  spawn at or before the playback time and still alive, from the slot's own sequence cursor, as a
  `GpuParticleFedSpawn` (its offset from the anchor, computed in double; its time, velocity and
  sequence number), a run of one emitter's spawns sharing a range flagged `ParticleRangeFed` whose
  `FirstIndex` is its first record. The spawn pass reads the record for the particle's index, time,
  place and velocity and derives everything else from the index as for any particle. Spawns past
  the pool or the record table are passed over and counted with the dropped spawns.
- **An emitter with several flipbooks takes a record per flipbook.** A billboard's
  `ParticleEmitterDesc::Flipbooks` may list up to `MaxParticleFlipbookVariants`; an effect's
  records are its emitters' own, in order (each playing its first flipbook, so an emitter's index
  is its record's), then the records for their further flipbooks, which the own record's
  `Variants` (count, first record) names. Spawn picks one with `ParticleVariantOf` — draw 9 of the
  particle's key, twinned with the rest of the math — and stores that record's index in the
  particle, so update and draw read only the record of the flipbook it plays, and every renderer
  picks the same. The records carry the same spawn parameters; only the flipbook's shading
  differs. An emitter's live particles count as alpha, and as reading scene depth, when any of its
  records does. The CPU knows
  `SpawnTotal`, `AliveBound` and `AlphaBound` exactly; spawns past the capacity or the range table
  are trimmed and counted, never deferred.
- **Precision under a floating origin.** An instance's anchor is a `dvec3` in the stable frame; the
  gather sends `AnchorEye = Anchor − FloatingOrigin::Position − camera` per instance per frame, in
  double, and the shaders work in eye-relative f32 from there (the ribbon precedent), so a
  particle's error is the f32 rounding of its offset from its anchor and a moving origin adds
  nothing. Particles need no offset hook beside `OffsetRibbons`.
- **The chain, in declaration order**, each a compute pass but the last: **Particle Reset** (the
  first frame after allocation: the dead list and counters), **Particle Spawn** (one thread per
  issued index, finding its range by binary search over the prefixes, deriving its state from the
  index, popping a slot with an atomic decrement that a failed pop restores and counts as dropped,
  and skipping a particle already past its lifetime), **Particle Prepare** (the update's dispatch
  arguments, and zeroed append counters), **Particle Update** (dispatched indirectly: kill by
  generation or age onto the dead list, else append to the other live half and write the draw
  record — closed-form position and velocity, the curves read from the emitter's tables, the
  flipbook frame, and for a six-way flipbook the scene's light gathered once with
  `EvaluateSpriteLighting` over the lights its tiles name — then push to the additive list or the
  alpha keys, and count the survivors that may draw alpha), **Particle Sort**,
  **Particle Finalize** (the two draws' vertex counts, and the stats region) — all declared ahead of
  the "Scene Layers" pass — and the draw inside it (two `DrawIndirect` calls: alpha, then
  additive). The update binds the IBL and shadow
  sets at 4 and 5 with the frame-slot dynamic offsets and declares the shadow atlases
  `SampleCompute`.
- **Idle gating, decided on the CPU.** A frame that spawns marks the GPU as possibly holding live
  particles; a frame whose update ran with `AliveBound == 0` clears it, since that update killed
  everything left. Prepare, update, finalize and the draw skip while neither holds, the live halves
  swap only on a frame whose update ran, and a wired but idle chain records no dispatch and no render
  pass. Wiring has the sprite pass's hysteresis (`ParticleIdleFrameLimit`, 256 gathers tracking no
  instance), and the battery is created and destroyed with it.
- **The sort is a bitonic sort** of `(complement of the view distance's float bits, draw index)`
  keys, ascending — so back to front — over `P = max(1024, nextpow2(B))`. `B` is
  `min(AlphaBound, reported + issued)`: `reported` is the last read-back update's survivors that may
  draw alpha (an alpha emitter record's, and every particle of an instance hidden that frame), and
  `issued` every spawn the gather issued since that frame for an emitter any of whose records is
  alpha. Particles enter only by spawning, so `B` bounds what draws alpha however late the readback
  is, and the sort tracks what is alive rather than the gather's lifetime bound. It runs in one source
  with three entries: `csSortLocal` sorts 1,024-key blocks in groupshared memory, reading an index
  past the frame's alpha count as the sentinel `0xFFFFFFFF` so no padding pass is needed;
  `csSortGlobal` is one compare-exchange step for `j ≥ 1024`; `csSortMergeLocal` finishes a stage's
  `j < 1024` steps in groupshared memory. `1 + Σ_{s=11}^{log₂P} (s − 9)` dispatches, 28 at 65,536,
  separated by compute-to-compute barriers the pass records itself. Additive particles are not
  sorted.
- **Drawing goes through the sprite shading core.** The vertex stage indexes the sorted keys or the
  additive list by `VertexIndex / 6`, rotates the eye-relative centre by the View matrix's rotation
  alone and projects through the jittered `Proj`. A billboard places its corners about the
  flipbook's anchor, rolled and sized by the blended frame extent, exactly as a sprite does; its
  fragment runs the sprite pass's arithmetic — the unlit path, the frame blend by crossfade or
  motion, `ShadeSixWay` over the gather the update packed into the record as halves, the emission
  ramp, the soft edge against the opaque-depth copy — so a lit flipbook looks the same as one sprite
  or as a thousand particles. A streak spans tail to head with its width across the projected axis,
  floored at 1.5 px with its coverage scaled by the ratio, falling back to a disc when its axis is
  shorter than its width, shaded `(1 − v²)²` across and soft at its ends. A frame drawing a soft
  particle records the depth copy (`ParticleFramePlan::ReadsSceneDepth`), as a soft sprite does.
  Under a temporal resolve a particle adds its coverage into the reactive mask, as a sprite does.
- **Budgets and stats.** `SceneRenderer::GetParticleStats()` reports the capacity, the live,
  spawned and dropped counts (read back frames-in-flight late, as the point field's are), the last
  sort length, and the instances tracked and dropped; the renderer warns once for its lifetime on the
  first drop of each kind.

`tests/unit/particles.cpp` pins the closed form, the cursors (a burst issues exactly its particles
whatever the partition, a rate its window, a late renderer only the living), the spawn state's
determinism and shapes, the CPU end time, the pool's retire and flash light, the anchor under a
moving origin, the baked tables and the budgets; `tests/gpu/particle_pass.cpp` that a particle draws
where the twin puts it, a burst spawns exactly its count and an overflow is clamped and counted,
death and a retire empty the pool and idle the chain, alpha particles sort back to front across a
device-memory sort stage, a streak lights its span, and a six-way particle reads `E/π` from a light
along its lit axis.

### Ribbons and trails

**`Ribbon`, `Trail` and `RibbonPath` components draw as camera-facing HDR bands** (the components,
`RibbonSystem` and `SpawnTransientBeam` are in [../Scene/CLAUDE.md](../Scene/CLAUDE.md), "Ribbons and
trails").
`RibbonScenePass` (`Passes/RibbonScenePass.h`) records **after the translucent passes and
immediately ahead of the sprites**, first in the shared "Scene Layers" pass, in both compositing arms, into the same lit target and
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
  the closing joint included. **A trail with a `CrossSection` draws its emitter's shape**: each
  end's width is set on the CPU, where the eye, the end and its tangent are all at hand, to the
  extent of the end's elliptical cross-section — semi-axes from the axes its sample recorded,
  tapered with the width — across the side the vertex stage will expand it along
  (`TrailCrossSectionWidth`, perpendicular to the tangent and the eye ray), so a flat plume reads
  wide square-on and thin edge on with no change to the record or the shader. Each 96-byte
  `GpuRibbonSegment` carries both ends' positions, widths, colours and opacities and the per-end
  tangents. The records sit in a host-mapped ring sized at the
  budget below, one region per frame in flight at set 3, as the sprite pass's do — so a frame's
  ribbons, trail samples and path segments all draw from one fixed region and never grow it.
- **A tube trail is a second record kind.** A `Trail` with `Shape` `Tube` contributes one 144-byte
  `GpuTrailTube` per consecutive pair of its points — both rings' eye-relative centres, colours and
  axes (carrying the taper) and an index into a per-frame table of outline points
  (`RibbonDrawPlan::Outlines`, at most `MaxTrailOutlinePointsPerFrame`) — in a ring of its own at set
  3 binding 1, the outlines at binding 2. The vertex stage (`ribbon_tube.vert`) draws each record as
  a quad per outline side at the most sides an outline has (`MaxTrailOutlinePoints`), collapsing the
  quads past its own outline's count, with a smooth outward normal per outline point. The fragment
  draws both faces, each covering `|N·V|^Softness`, so the core reads brightest and the silhouette
  fades out. Tubes are scene-placed and occluded, draw between the occluded and unoccluded band sets
  through their own alpha and additive pipelines, and count one record each against the shared
  budget; the post-resolve pass holds token tube and outline rings, its set's bindings needing a
  buffer each.
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
- **A path may hold its brightness against the exposure (`RibbonPath::CompensateExposure`).** Its
  records carry the flag in `EndTangent.w`, and the vertex stage divides the colour by the view's
  exposure (`ViewInverseExposure`, the view block's `ExposureParams`), so an instrument drawn as
  linework shows as it would at an exposure of 1 while auto-exposure follows the scene.
- **The reactive mask** is written by the scene placement under a temporal resolve, after any bloom
  mask, the larger of the band's coverage and the luminance it adds (see "TAA"), so a thin, fast band
  — a tracer — is not dropped from the history.
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
  a translucent material that does not include it binds nothing extra. Set 6 is a sphere terrain's,
  for a translucent level layer alone (see "A translucent level layer"). The pass declares the shadow
  atlases sampled, as the lighting pass does. The sprite pass binds the two sets too (see "Flipbook
  sprites") and gathers in its vertex stage, and a compute pass may gather per element with the same
  functions, so both layouts — the shadow system's and `BakedSkyCube::CreateConsumerSetLayout`'s —
  are visible to the vertex, fragment and compute stages; every pipeline binding them is built
  against those same layout objects, so no two consumers' layouts drift apart. **The shadow maps
  hold only opaque casters** (a Translucent submesh casts no shadow), so a translucent surface is shadowed by opaque geometry and
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

**A medium a Translucent material marches asks the shadow maps about any point.**
`Veng/medium_lighting.slang` is the medium's counterpart of the forward loop — volumetric fog with
light shafts, dust lit through a window, any view-filling medium a material integrates itself. Its
queries are `Veng/light_visibility.slang`'s, which declares `shadow.slang` at set 5,
`Veng/light_buffer.slang` and `Veng/light_flags.slang` and nothing of the lighting core, and which a
Surface fragment includes too (see "A Surface fragment may read the shadow maps"); the medium header
adds `Veng/ibl_maps.slang`'s set-4 declarations, which it never samples: a translucent material's
declared sets must be contiguous (`ShaderInterface::GroupBindingsBySet`), and the forward-lit
layout binds set 4 ahead of the shadow set. It answers three questions:

- **`LightVisibility(vc, lightIndex, worldPosition[, filter])`** is the share of a light reaching a
  point through the opaque shadow maps, in [0, 1]. It dispatches on the light's arm exactly as
  `AccumulateLight` does — a Directional's cascade set, a near-parallel area light's cascade set,
  any other light's punctual tile — through the same lookups, at the point's own view depth, so a
  surface and a medium at one point agree on whether it is shadowed. A medium has no normal, so the
  normal-offset bias steps toward the light (`N := L`); the surface overload
  `LightVisibility(vc, lightIndex, worldPosition, normal, filter)` takes it along the normal, as the
  deferred loop does. An area light's tile is read through the
  plain comparison lookup, never `AreaShadowSample`'s rotated PCSS disk, so the answer carries no
  per-fragment pattern. The filter is `LightVisibilitySingleTap` (one hardware 2×2 comparison tap,
  the default, for a point integrated along a march) or `LightVisibilityPcf` (the surface lookups'
  3×3 kernel). It returns 1 outside every map's coverage, for a light casting no shadow, and for an
  index naming no live light, so a medium marched past the shadowed scene is lit, not black.
- **`FindLightByTag(vc, tag)`** is the packed index of the first light carrying `Light::Tag`, or
  `LightIndexInvalid`; tag 0 is untagged and finds nothing. The tag rides `Response.y` of the packed
  light as its u32 bits (`PackLightTag`), read back by `LoadLightTag` as raw bits, so the stride is
  unchanged and any tag survives. The tag namespace is the consumer's; a debug build warns once per
  duplicate when two lights in a view carry the same nonzero tag (`PackedSceneLights::DuplicateTag`).
- **`LightShadowMapAt(vc, lightIndex, worldPosition)`** is the map `LightVisibility` reads a light
  through at a point (`LightShadowMap`): the direction toward the light it projects along, the world
  units one of its texels spans across that direction, and the view depth out to which the same map
  covers — a cascade's far split, or `LightShadowMapUnbounded` for a punctual tile, whose texel size
  is measured by projecting the point and one beside it, so it is the tile's perspective footprint at
  the point's depth. A ray's image in the map is its length times the sine of its angle to the light
  over the texel size, so a medium that spaces its lookups about a texel apart along that image reads
  everything the map holds and no more; the covered depth tells it where along the ray to ask again.
  The texel size is zero wherever `LightVisibility` reads 1 without sampling.

None takes a derivative, so all are legal in a loop and in divergent flow. A fragment including
the file declares set 5, so `IsForwardLit` recognises it and the pass binds the forward layout in
both layers, with no new path; `tests/gpu/light_visibility.cpp` reads every cascade from the
half-resolution layer and at full resolution, and an area light's punctual tile, and asks
`LightShadowMapAt` about the same probes. The shared light
includes (and `Veng/ibl_maps.slang`) carry `#pragma once`, so a material lighting a surface includes
`Veng/forward_lighting.slang` and then this.

### Bloom

**Bloom is a compute mip-pyramid battery**, a fixed engine pass like SSAO and the shadow atlas —
not a PostProcess material. The lit HDR target is bright-passed (a soft-knee `Threshold` over a
**per-tap firefly ceiling** on the first downsample, below) into a single `HdrFormat` mip-chain
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
`SceneRendererSettings` topology knobs (a `Configure` recompile); `Threshold` / `FireflyCeiling` /
`Intensity` / `Radius` are per-frame `SceneView` values — all but `Intensity` ride the compute push,
`Intensity` the tonemap's material block — so tuning them never recompiles. `DebugView::Bloom` blits pyramid
mip 0 after the up-sweep — the accumulated bloom contribution the tonemap would add.

**The bloom lights a lens-dirt mask.** `SceneView::LensDirt` (a texture, its red channel read) and
`LensDirtIntensity` — authored on `RenderLook`, overridable through the debug layer like any view
knob — add a second term in the same tonemap pass, beside the ordinary bloom:
`hdr += wide × dirt(uv) × LensDirtIntensity`. With no resident mask, an intensity of 0 (the
default) or bloom inactive, the renderer writes 0 and the shader skips the term's uniform branch, so
the frame is bit-identical to one without the feature (`tests/gpu/scene_renderer.cpp`).

- **`wide` is a coarse level of the pyramid, not mip 0.** A dirty lens scatters a source's light over
  a broad patch of glass, so the term reads the level `LensDirtLevelsAboveCoarsest` (3) above the
  coarsest (`LensDirtBloomLevel`, `LensDirt.h`) — a sixteenth of the frame's resolution at 1080p, and
  the same fraction of the frame at every extent, since the coarsest level is. After the up-sweep that
  level holds every coarser level's glow too, so it carries about mip 0's energy, spread wider:
  mip 0's tight core would reveal the dirt only in a halo hugging the source. Read through the same
  extra-input seam as mip 0 (declared whenever bloom is wired, so a per-frame knob never recompiles),
  its many-fold magnification is a four-tap cubic B-spline rather than one bilinear tap, which would
  draw a single bright source's glow as a diamond.
- **The mask is cover-fitted to the presented frame** (`ComputeLensDirtCoverMap`): scaled uniformly
  until it fills the frame, centred, cropped along one axis, never stretched. It is sampled in output
  UV at the post-resolve allocation through the texture's own sampler, so it is fixed on screen —
  no jitter, no dependence on a temporal resolve — and independent of the render scale.
  `tests/unit/lens_dirt.cpp` pins the fit at 4:3 through 21:9.

**The bloom's bright pass also feeds an anamorphic streak.** `SceneView::StreakIntensity`,
`StreakLength` and `StreakChromaticSpread` — authored on `RenderLook`, overridable through the debug
layer — stretch every bright source into a thin horizontal line in its own colour, the flare of an
anamorphic lens, added in the tonemap beside the bloom: `hdr += streak × StreakIntensity`. It is the
`AnamorphicStreak` battery (`AnamorphicStreak.{h,cpp}`, the arithmetic device-free in
`StreakKernel.h`), built and released with the bloom pyramid, whose down/up set layout it shares.

- **Two passes.** `Streak Prefilter` bright-passes the scene colour into a buffer a quarter of the
  frame's width and half its height (`StreakExtent`): each destination texel averages the 4×2 block
  it covers through two bilinear taps, each tap conditioned and the average soft-kneed by the same
  `bright_tap.slang` the bloom's level 0 uses, at the same threshold and ceiling, so a source streaks
  where it blooms. `Streak Filter` then spreads each row along itself, one workgroup per row: a
  forward and a backward recursive sweep give every texel `Σ_d r^|d| · source[x − d]` normalised to
  keep the light (`StreakFilterRow` is the CPU twin), so each source falls off exactly
  exponentially, symmetric and monotone, with no reach limit and no comb ripple. Both sweeps run as
  running averages (the sum times `1 − r`), which bounds the forward one, stored between them in the
  half-float destination, by the row's brightest texel. Past the row's ends is zero, so light leaves
  at the frame's edges. A sweep is the first-order recurrence `y = r·y + (1 − r)·x`, whose affine
  steps compose associatively, so each invocation reduces a contiguous run of the row to one map, a
  workgroup scan hands each run the value entering it, and each run replays from there — the
  row's dependent chain is a run long rather than a row long.
- **Each channel has its own ratio** (`StreakChannelRatio`): green falls to 1/e over `StreakLength`
  of the frame's width, red and blue over `1 − StreakChromaticSpread` of that (never under a tenth),
  so a positive spread keeps red and blue nearer the source — a violet core shading toward green
  along the tail — and a negative one the reverse, each channel keeping its light.
- **The line is about two output pixels thick**: the buffer's rows are two scene rows each, and the
  tonemap magnifies it with one bilinear tap. Nothing is jittered, so it is steady with
  anti-aliasing off.
- **Off costs nothing.** With an intensity of 0 (the default) or bloom inactive the tonemap's term is
  skipped, and both passes skip through `SkipWhenUnread` — the tonemap samples nothing from them that
  frame — so the frame is byte-identical to one without the feature and no dispatch runs; a
  per-frame intensity never recompiles. `tests/gpu/scene_renderer.cpp` pins the zero case, linearity
  in the intensity, and that a source lights its row far more than the same distance across it;
  `tests/unit/anamorphic_streak.cpp` the filter's conservation, symmetry, monotone falloff and
  per-channel reach.

**Firefly stability is a per-tap ceiling ahead of a linear average.** On the first downsample,
each tap (a bilinear average over a 2×2 block of the scene) is conditioned on its own before the
kernel averages it (`bright_tap.slang`, through `bloom_bright_pass.slang`, shared by both kernels and
the streak's prefilter): a non-finite tap becomes
zero, and a tap brighter than half of `BloomFireflyCeiling` is rolled off along a smooth shoulder
toward the ceiling, scaled whole so its hue is kept. The kernel then takes its plain weights — the
same COD 13-tap (inner ring 0.5, corner groups 0.125) or Kawase 5-tap the deeper levels use — and
the soft-knee `Threshold` runs on that average. Both kernels sum every source texel to the same
total weight however it falls on the destination grid, so **below the ceiling the bright pass
conserves a source's light**: a small bright source seeds the same energy at every sub-texel
offset, and nothing pumps as it moves. A weighting that is nonlinear in brightness — the Karis
`1/(1+luma)` group weight is the usual one — breaks that for exactly the few-texel sources it
exists for, since how strongly it suppresses one depends on how it divides among the groups. Above
the ceiling one texel seeds at most the ceiling's worth (the taps covering a texel carry a total
weight of one), so a firefly is bounded however extreme; a source over half the ceiling across its
whole footprint is rolled off per tap and blooms by its coverage again, which is why the default
(64, display-referred like `Threshold`, so divided by the resolved exposure) sits well above what
the tone curves resolve. The bloom mask takes the same plain weights, so the mask and the colour
stay the same average of the same footprint. `tests/gpu/bloom_bright_pass.cpp` pins all three
properties on both kernels: invariance across sub-texel offsets, the ceiling's bound on one
extreme texel, and zero from a `NaN` or `inf`.

### Screen-space reflections: a port of FidelityFX SSSR

**The reflection chain is AMD FidelityFX SSSR and its reflection denoiser (SDK v1.1.4, MIT)
translated onto the frame graph, not an engine design.** Every function, constant, branch, format and default is the reference's;
the shaders under `assets/core/shaders/ssr/` name the reference file each ports and carry AMD's
notice. Where the port departs from the reference, an engine contract forced it, and the departure
is one of these:

- **D1** — the classifier exchanges its quad neighbours' copy requests through workgroup memory, not
  `WaveReadLaneAt(lane ^ k)`: Vulkan does not promise lane = local index. Its ballot compaction keeps
  the wave operations, which are correct under any lane order.
- **D2** — the depth hierarchy is built one level per dispatch, not by SPD's single pass, whose quad
  operations have the same lane-order dependence. Its contents are SPD's.
- **D3** — roughness is converted once, `α = r²`, where the classifier loads the g-buffer's
  perceptual roughness; everything downstream reads α, as the reference's "roughness" is (the
  denoiser's loads and its roughness history never square it).
- **D4** — the engine's motion vectors are `curUV − prevUV` (`ComputeMotionVector`, in the engine's
  y-down UV); the reprojection loads them with the reference's `motionVectorScale` fixed at −1 on both
  axes, giving its `uv + motion = previous uv`. Velocity is stored whenever SSR runs, anti-aliasing off
  included (`FrameTopology::GBufferStores`). The engine's motion vectors are unjittered over a
  jittered raster, which is the FidelityFX convention the denoiser assumes (the reference sample runs
  no jitter at all), so there is no jitter correction.
- **D5** — the reference's environment cube is the view's prefiltered specular probe
  (`EnvironmentIbl`), sampled the reference's way (mip `sqrt(α) · log2(width)`), and its `iblFactor`
  is the view's IBL intensity — 0 for a view whose ambient arm is not IBL — times the view's
  `EnvironmentLightTint` (`SsrConstants::IblTint`), so the fallback is the light the lighting pass's
  IBL arm would have given. The chain samples the cube
  in that case too, so `EnvironmentIbl` zeroes it at initialization and leaves it sampled for compute
  as well as fragments (`SampleAny`).
- **D6** — the sample's "specular IBL factor 0 for the lighting pass while SSSR is on" is a per-view
  flag the deferred lighting reads: `FrameTopology::DeferredSpecularIbl`, cleared exactly while the
  chain runs, reaches `DeferredLightingScenePass` as the push block's `SpecularIblFactor`, which
  scales the ambient arm's specular term (only the IBL arm has one). So the lit colour the trace reads
  and the composite adds into carries no specular IBL, as the reference's does. The view block is
  untouched, so forward-lit translucents, drawn after the composite, keep their probe specular.

The intersection's low-occupancy exit counts the active lanes of the device's own subgroup (32 on
Apple GPUs), which is the reference's permutation for hardware it cannot force to wave64 — a choice
among its permutations, not a deviation.

The reference's coordinate helpers flip y between UV and NDC (Direct3D's NDC is y-up); the engine's
NDC is y-down like UV, so the flip is omitted — a change of convention, not of behaviour. A load
past a texture's extent returns 0 and a store past it is dropped, Direct3D's semantics, which the
reference relies on along the last row and column of tiles and in the denoiser's 16×16 tile aprons.
The reference's `isnan`/`isinf` tests are written as an exponent-bit test (`DenoiserIsFinite`),
since the engine's shader compile may fold the intrinsics away under fast math — the same test, not a
different one. The 32-bit path is ported; the `FFX_HALF` permutation is not.

**`SsrChain` owns the whole vertical** (`SsrChain.{h,cpp}`), created only while
`ResolveSsrActive(settings)` holds (`Settings.SSR` in the shaded path's Final view), so a renderer
without reflections holds none of its pipelines, tables or targets and declares none of its passes.
`SsrScenePass` places its passes in the pass list, after the opaque lighting and the sky and ahead of
the translucent composite — the reference sample's order. `SsrChain::Declare` is `DeclareTrace`
(passes 1–5, `ffx_sssr.cpp`), then `DeclareDenoiser` (passes 6–9, `ffx_denoiser.cpp`), then
`DeclareApply` (pass 10, the sample's `sssrrendermodule.cpp`); a test that inspects an intermediate
output declares the earlier halves alone. The passes, in the reference's dispatch order:

1. **`SSR Depth Downsample`** ×levels (`ssr_depth_downsample.comp`, D2) — level 0 copies the depth;
   each later level holds the nearest (reverse-Z: largest) of its parent's 2×2 children, a child past
   the parent's extent counting as 0. Each level covers its share of the frame's rendered sub-rect.
2. **`SSR Classify Tiles`** (`ssr_classify_tiles.comp`, D1/D3/D5) — one 64-thread group per 8×8
   tile, remapped so each quad is four consecutive local indices. A pixel traces if it is on screen,
   reflective (depth > 0) and glossy (α < `SsrRoughnessThreshold`), and is its quad's base ray
   (`SsrSamplesPerQuad` 1: the top-left pixel; 2: the diagonal pair; 4: all) or a mirror
   (α < 1e-4), or — under `SsrVarianceGuidedTracing` — its variance history exceeds
   `SsrVarianceThreshold`. A glossy pixel that does not trace sets copy flags on its quad's base ray.
   A rough reflective pixel writes the environment along its mirror direction into radiance; every
   other pixel writes zero. A tile with any glossy reflective pixel is appended to the tile list.
3. **`SSR Prepare Blue Noise`** (`ssr_prepare_blue_noise.comp`) — Heitz and Belcour's sampler over
   the SDK's 1spp Sobol and scrambling tables (the core-pack Raw asset `data/ssr_blue_noise.bin`, one
   byte per entry, widened to `R32_UINT` at upload; `assets/core/tools/generate_ssr_blue_noise.py`
   regenerates it from the SDK source), offset by `frame · φ`.
4. **`SSR Prepare Indirect Args`** (`ssr_prepare_indirect_args.comp`) — the intersection's dispatch
   at byte 0 (`[(rays + 63) / 64, 1, 1]`) and the denoiser's at byte 12 (`[tiles, 1, 1]`), and the
   counters rotated to `[0, rays, 0, tiles]`.
5. **`SSR Intersect`** (`ssr_intersect.comp`, D5) — dispatched indirectly from byte 0, 64 threads a
   group, thread `group·64 + index` tracing ray list entry of that index while it is under
   `counter[1]`. A ray samples a GGX visible normal (Heitz 2018) with the blue-noise tile's pair at
   `pixel mod 128`, marches the depth hierarchy from level 0 (`FFX_SSSR_HierarchicalRaymarch`: the
   initial advance, the 0.005-texel cross epsilon, up a level on a skipped cell to at most level 6,
   down on a crossing, at most `SsrMaxTraversalIntersections` iterations, and — mirrors excepted —
   an early exit once the subgroup's active lanes fall to `SsrMinTraversalOccupancy`), and weighs
   the hit by `ValidateHit`: zero off screen, within two pixels of the origin, on background
   (level 1 depth 0) or on a back face, else `vignette · (1 − smoothstep(0, thickness, d))²`, `d`
   the view-space distance from the level-1 surface. Radiance A gets
   `lerp(env(dir, mip 0) · iblFactor, litColour(hit), confidence)` in rgb (the lit colour a point
   load, read only when confidence > 0) and the ray's world-space length in a, at the ray's pixel
   and at each quad neighbour its copy flags name. The reference's `valid_hit` is always true and
   is kept so.
6. **`SSR Denoiser Reproject`** (`ssr_denoiser_reproject.comp`, D3/D4) — dispatched indirectly from
   byte 12, one 8×8 group per listed tile (the tile list gives its first pixel; threads remapped by
   `ffxRemapForWaveReduction`), each group loading a 16×16 apron of radiance A into workgroup
   memory as packed halves. For each glossy pixel: the 9×9 Gaussian statistics (`exp(−3i²/25)` per
   axis) of this frame's radiance; two history candidates — the surface (`uv + motion`) and the
   virtual hit point (the view ray lengthened by the ray length, projected through the previous
   view-projection); the hit candidate wins when its history normal's similarity exceeds 0.9999, is
   at least the surface's and its history roughness is closer, else the surface's when
   `|hist − μ|² < 1.5·|σ²|`, else none (disocclusion factor 0). The factor
   `exp(−|1 − n·n′|·1.4)·exp(−|Δz|/z)`, below 0.9, takes the best of a 3×3 search, then a 2×2
   bilinear footprint weighted by occlusion over 0.45, then 0. Writes the reprojected radiance,
   variance A (`lerp(temporal variance, history variance, 1/n)`) and the sample count
   `n = min(max(8, 32·(1 − e^(−100α))), history·factor + 1)`, or 0, 1, 1 on a disocclusion or off
   screen; and reduces the tile's radiance (30 % towards the reprojection where history was kept),
   weighted `max(e^(−0.3·luma), 0.01)` with non-finite or off-screen pixels dropped, into the
   8×8 average.
7. **`SSR Denoiser Prefilter`** (`ssr_denoiser_prefilter.comp`) — same dispatch. A glossy, non-mirror
   pixel with variance above zero resolves from itself and 15 Halton(2, 3) taps within 3 pixels,
   weighted `pow(n·n′, 512)` · `exp(−|Δz|·z·4)` · `max(exp(−(0.6 + 0.1·var)·|avg − L|), 0.01)` ·
   `max(0.1, 1 − e^(−4.4·var))` (the centre by its radiance weight alone, variance accumulating
   with w²); every other pixel copies through. Writes radiance B and variance B.
8. **`SSR Denoiser Resolve Temporal`** (`ssr_denoiser_resolve_temporal.comp`) — same dispatch. A
   prefiltered radiance summing under 1e-4 resolves to 0 (variance 0). A glossy pixel clips its
   reprojected history (Playdead's `ClipAABB`, with its notice) into a box of half-size
   `(σ + |μ − avg|)·stability·1.4` about `lerp(μ, avg, 0.2)` (9×9 statistics of the prefiltered
   radiance), blends its signal towards the average by `1/(n + 1)` and clips it to `avg ± that`,
   then outputs `lerp(signal, clipped history, 1 − 1/max(n, 1))` and the variance likewise; a
   non-finite result is zeroed. Any other pixel keeps its prefiltered radiance and variance. Writes
   radiance A (rgb.xyzz, the reference's store) and variance A.
9. **`SSR Denoiser History Copy`** (transfer) — this frame's g-buffer normal, extracted roughness and
   depth-hierarchy level 0 into the normal, roughness and depth histories next frame's reprojection
   samples. The g-buffer normal therefore carries `TransferSrc` (`GBuffer::NormalUsage`).
10. **`SSR Apply Reflections`** (`ssr_apply_reflections.frag`, the sample's
    `sssr_apply_reflections.hlsl`) — one fullscreen draw over the rendered sub-rect into the lit
    colour (`PassIO::Hdr`: the TAA lit target under TAA, ahead of the resolve), with the sample's
    blend (colour `One + Dst·SrcAlpha`, alpha `One + One`) and alpha 1 written, so it adds
    `reflection · (F0·brdf.x + brdf.y)`, F0 = `lerp(0.04, albedo, metallic)`, to what the lit colour
    holds. `brdf` is the engine's split-sum LUT (`EnvironmentIbl::GetBrdfLutView`, x N·V, y perceptual
    roughness), read at (`saturate(N·V)`, perceptual roughness) through a bilinear, nearest-mip,
    clamped sampler — the sample reads its own table at (N·V, 1 − perceptual roughness), the same entry
    through that table's axes, and sets no address mode. V is the sample's per-frame `viewDirection`,
    the camera's direction (the view-to-world z axis, towards the eye), not a per-pixel vector. The
    sample's linear samples at the pixel centre are loads at the fragment's pixel; the g-buffer's
    perceptual roughness and signed normal are its `sqrt(alphaRoughness)` and `2n − 1` decodes. The
    sample's intensity slider (1 for correctness) and reflection-target view are its UI and are not
    ported.

**Two debug arms inspect the chain.** `DebugView::Reflections` and `DebugView::ReflectionCoverage`
(`IsReflectionDebugView`) composite the scene as the Bloom arm does — so they are `SceneComposited`
and wire the same shadows, SSAO, sky and translucents — and force-wire the chain whatever
`Settings.SSR` says, with the deferred lighting's specular IBL off as under Final. Their terminal
pass is `SsrDebugBlitScenePass` (`Passes/SsrScenePass`). `Reflections` blits radiance A, the
denoised reflection before the composite's BRDF weighting, through the albedo blit (its bindless slot
is read per frame, since radiance A alternates). `ReflectionCoverage` turns on
`SsrChain::SetCoverageOutput`: the classifier then runs its `csCoverage` entry point (the engine's,
in `ssr_classify_tiles.comp.slang`), the same `ClassifyTiles` reporting its decisions through an out
struct, and writes one `ssr_coverage.slang` code per pixel to an R8 target — none, environment only,
own ray, a ray traced for temporal variance alone, a quad neighbour's copy (set from the traced
ray's copy flags, so it is exactly the intersection's copy targets), a mirror ray, or a glossy pixel
wanting a copy no neighbour traced. `ssr_coverage_blit.frag` colours the codes. The coverage
pipeline, its set and its target exist only while that arm is selected.

**The denoiser's bindings follow the reference's two hosts exactly**, including what they read from
last frame. With A this frame's write target and B the other: the reprojection reads radiance A and
B, variance B and the sample-count history, and writes variance A, the sample count and the
average; the prefilter reads radiance A, variance A and the *average history* (the one last frame's
reprojection wrote — the reference binds its average SRV and UAV by the same parity for all three
passes) and writes radiance B and variance B; the temporal resolve reads radiance B, variance B,
the *sample-count history* and the average history, and writes radiance A and variance A. So the
temporal resolve blends by last frame's sample count, not this frame's, and the first frame after
creation, reading the cleared count, outputs zero for every glossy pixel. Variance A, written last by
the temporal resolve, is next frame's variance B, which the classifier's variance-guided tracing
reads.

The denoiser's render size is the chain's allocation extent (`SsrConstants::DenoiserRenderSize`),
which is what the reference's host gives it — its context's window size — while the SSSR passes use
this frame's rendered size. The two agree at render scale 1, the only configuration the reference
sample runs; under a dynamic-resolution sub-rect the denoiser maps UVs over the whole allocation as
the reference's would.

**Resources** (all render-allocation sized unless stated; "persistent" outlives the frame):

| Resource | Format | Lifetime | Graph id |
|---|---|---|---|
| Depth hierarchy | `R32Sfloat`, `min(7, ⌈log2 max(w,h)⌉)` levels | per frame, overwritten | `GetDepthHierarchyId()` (per level) |
| Radiance 0 / 1 | `RGBA16Sfloat` (see below) | persistent ping-pong, zeroed at creation | `GetRadianceId()` binds A, `GetRadianceHistoryId()` B |
| Variance 0 / 1 | `R16Sfloat` | persistent ping-pong, zeroed at creation | `GetVarianceId()` binds A, `GetVarianceHistoryId()` B |
| Sample count 0 / 1 | `R16Sfloat` | persistent ping-pong, zeroed at creation | written / history imports (internal) |
| Average radiance 0 / 1 | `B10G11R11Ufloat`, ⌈w/8⌉×⌈h/8⌉ | persistent ping-pong, zeroed at creation | written / history imports (internal) |
| Reprojected radiance | `RGBA16Sfloat` (rgb) | persistent, zeroed at creation | internal |
| Depth / normal / roughness history | `R32Sfloat` / G1's format / `R8Unorm` | persistent, zeroed at creation | internal |
| Extracted roughness | `R8Unorm` (α) | per frame | `GetExtractedRoughnessId()` |
| Blue noise | `RG8Unorm`, 128² | per frame | `GetBlueNoiseId()` |
| Ray list | `u32` per pixel | per frame | `GetRayListId()` |
| Denoiser tile list | `u32` per pixel | per frame | `GetDenoiserTileListId()` |
| Counters | 4 × `u32`, zeroed at creation | persistent (rotated) | `GetRayCounterId()` |
| Indirect arguments | 6 × `u32` | per frame | `GetIndirectArgsId()` |
| Sobol / scrambling tables | `R32Uint`, 256² / 512×256 | static | not imported |
| Constants (`SsrConstants`) | std140, one region per frame in flight | per frame | dynamic UBO |

The chain reads the g-buffer's depth, world normal, ORM and motion vectors, and the lit colour,
through `SsrGraphInputs`; `SsrScenePass` hands it `PassIO::Hdr` / `HdrHandle`, the target the opaque
lighting and the sky wrote at that point in the list (the TAA lit target under TAA, the DoF scene
target under a composited DoF), and `PassIO::Velocity` / `VelocityHandle`.

**What radiance A holds per pixel after the intersection:** a traced pixel and every quad neighbour
its ray copies to hold the ray's result (rgb radiance, a ray length); a rough reflective pixel holds
classify's environment along its mirror direction (a 0); every other pixel holds zero — background,
off-screen, and a glossy pixel that neither traced nor was copied to (its quad's base pixel was not
glossy and reflective).

**What radiance A holds after the temporal resolve — the denoised reflection the apply composite
reads** (`SsrChain::GetReflectionId()`, the same import as `GetRadianceId()`): on every pixel of a
listed tile, the resolve's output in rgb and its blue channel again in a; elsewhere the
classifier's value (a rough pixel's environment, or zero) with a 0. Alpha carries nothing a consumer
reads. It holds until next frame's prefilter reuses the target as radiance B.
`SceneRenderer::GetSsrReflectionView()` returns it after `Execute`. The reference's host copies it to
a separate output target; here the apply pass loads radiance A within the same frame, so the copy
has no reader and is not made. Outside the listed tiles, then, a rough pixel's composite is the
environment along its mirror direction through the BRDF — the specular IBL D6 took out of the
lighting — and every other pixel's is zero.

The ping-pong follows the reference's parity: on an odd frame index radiance, variance, sample count
and average A are target 0 and B target 1; on an even one, 1 and 0. A `Recreate` at a new extent
zeroes every persistent target and restarts the frame index — the reference's first-frame clears.
The reference leaves the depth, normal and roughness histories uninitialized; they are cleared with
the rest, to the g-buffer's own "no surface" (depth 0 under reverse-Z, a zero normal), so the first
frame's history describes an empty frame and every glossy pixel is disoccluded. The per-frame
tunables (`SsrSamplesPerQuad`, `SsrRoughnessThreshold`, `SsrVarianceThreshold`,
`SsrVarianceGuidedTracing`, `SsrDepthBufferThickness`, `SsrMaxTraversalIntersections`,
`SsrMinTraversalOccupancy`, `SsrTemporalStabilityFactor` — the reference sample's UI set, with its
defaults and ranges) ride `SceneView` / `ViewState` / `RenderLook` like the other per-frame knobs;
the reference's most detailed mip (0) is its default in the constants block.

**The settings surface is the sample's and nothing else.** `SceneRendererSettings::SSR` (off by
default, so a renderer that does not ask for reflections allocates and declares nothing) is the
topology toggle; the eight knobs above are the per-frame ones (defaults 1 ray per quad, roughness
threshold 0.2 in α, variance threshold 0, variance-guided tracing on, thickness 0.015, 128
iterations, occupancy 4, stability 0.7). There is no intensity and no reach distance. A level opts in
through its `render` block, which seeds the scene's `RenderLook` by field name — `"SSR": true`,
`"SsrSamplesPerQuad": 4`, and the rest under their `RenderLook` names — and the presenting viewport
resolves it into the topology and the view like any look field.

`tests/gpu/ssr_chain.cpp` drives the passes on a synthetic g-buffer and checks each output against
the reference's rule, the intersection on a mirror floor beside a lit sphere, the denoiser over
several frames (finite output, the zero early-out, the first frame's and a still camera's second
frame's sample counts, and the variance ping-pong into the classifier), and the composite against
its formula through a stand-in LUT (a pixel reflecting nothing keeps its colour exactly, a flagged
surface is untouched); and that a renderer records the whole chain only with SSR on and, with nothing
glossy in view and no image-based lighting, draws the frame it draws with SSR off.
`tests/unit/frame_topology.cpp` pins that D6's flag clears exactly while the chain is wired.

### Auto-exposure

**The meter reads a log-luminance histogram of the scene before the pre-bloom overlays**
(`AutoExposureMeter`, `auto_exposure.comp`), a frame later through a host-mapped ring, and eases an
adapted luminance toward what it asks for; the exposure is `Key / adapted × Exposure`. The arithmetic
is the device-free `MeterAutoExposureHistogram` (`AutoExposureMetering.h`, pinned by
`tests/unit/auto_exposure_metering.cpp`). Four knobs shape it, each a `SceneView` / `ViewState` /
`RenderLook` field:

- **`AutoExposureMeterFloor`** is the histogram's lower bound: a pixel darker than it is not metered,
  so a frame of mostly black space meters its lit content. The top bin is fixed at the scene colour's
  limit (`AutoExposureMeterCeiling`, 65504), so nothing the clamps say truncates what is read.
- **`AutoExposureMinLuminance` / `MaxLuminance`** clamp the adapted luminance — Min is the darkest
  scene exposed up to the key, so a field dimmer than it is shown dimmer than mid-grey; Max is how far
  a bright scene may darken the exposure, so a subject past it (a star filling the view) stays bright. Separate from
  the floor, so a dim field can be metered and shown dim instead of being either ignored or pushed
  to mid-grey.
- **`AutoExposureLowPercentile` / `HighPercentile`** bound the band the geometric mean is taken over.
- **`AutoExposureHighlightLuminance`** (zero, off, by default) caps the exposure so the luminance at
  `AutoExposureHighlightPercentile` exposes no brighter than it: the target is the larger of the
  average and `Highlight · Key / HighlightLuminance`. So a small bright subject in a large dim field
  — a lit planet against a sky — sets the exposure rather than being blown out by the field's average,
  while a frame of only the field meters its average.
- **A surface flagged `SurfaceFlagExcludedFromMetering` is not metered at all** (the g-buffer's
  surface-flags channel, `Veng/surface_flags.slang`): the meter point-loads the flag at each texel's
  place in the frame, mapped into the g-buffer's rendered sub-rect since the scene colour it reads may
  be at the resolved resolution, and skips the texel before it is binned. It is for a light source
  drawn as a surface. A lamp's emission is far brighter than what it lights, so without the flag a
  few fixtures in view are the frame's highlight and its weight in the average, and the room they
  light exposes dark; with it the meter exposes for the room and the fixtures blow out, as a camera
  shows them. A Translucent material declaring `"excludedFromMetering": true` flags the pixels it
  chooses the same way, through the g-buffer's surface flags (see "Additive translucent materials"),
  so a light source drawn as a translucent surface over the scene is excluded too. A renderer with no
  g-buffer meters everything.

### Depth of field and the physical camera

**Depth of field is a half-resolution ring-gather compute battery** (`DofChain`,
`Settings.DepthOfField`, **off by default**) whose parameters come from an authored camera rather
than bare blur knobs. It is an owned subsystem with compute stages, one fullscreen composite (`DofCompositeScenePass`, `Passes/DofCompositeScenePass.h`)
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
`(Mode == Final && DepthOfField) || Mode == DebugView::CoC`, where the disjunct is what lets `DebugView::CoC` force-wire the chain's first two stages **with the feature
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
`DofMaxCoc`, and `DofRingCount`. `ApplyRenderLook` stays a **pure, unconditional
mapping** — it records authored intent into the resolved knobs and never inspects the camera —
while the viewport glue fills the lens-derived fields in the per-frame copy it pushes, and the
viewport's per-frame look write (`CopyLookKnobs`) leaves the lens fields alone while that push
reports a `Physical` camera. So
camera-wins holds **by construction every frame**, the stored authored values survive untouched,
and they come back to life the moment the camera stops being `Physical`. `DofCocScale` is
*always* derived by the glue (sensor height × viewport pixel height) and is never hand-authored in
any mode. `ViewState::DofFromPhysicalCamera` is the flag the settings panel and level editor read
to show `DofFocusDistance`/`DofAperture` **inactive**, so an author is not editing values nothing
consults.

**`DofMaxCoc` and `DofRingCount` still apply in every camera mode** — they are quality knobs, not
lens properties, and a physical camera does not drive them. Both are **hard-clamped where they are
pushed** (`ClampDofMaxCoc` → `DofCocCeiling`, `ClampDofRingCount` → `MaxDofRings`, `Renderer/DofTile.h`)
because `RenderLook` routes authored values in from a cooked level or prefab and **an archive is
untrusted input** (and `CopyLookKnobs` clamps them again, since a host's look resolver may write
them); the ring count is a GPU loop bound, and the gather shader ceilings it a second
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
one author-opt-in **`Sky` component** (`Veng/Scene/Components/Environment.h`): a **source** (`SkySource`
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
`discard`ing foreground, writing the same scene-color target lighting wrote so the sky resolves
and tonemaps with the scene). `SkyMode::Direct` keeps the per-pixel passes as the
authored **dynamic** modes — `SkyScenePass` (procedural atmosphere) and `SkyMaterialScenePass`
(authored material) — for a continuously-animating sky; a direct source **cannot light** (it has
no cube), so a direct source with a lighting tier degrades to background-only with a one-time
warning (bake to light). Both lighting tiers read the resolved source's one cube: `SH` projects it
to the irradiance SH the lighting pass folds into its ambient arm
(`EnvironmentIbl::ProjectCubeToIrradianceSh`, a device-free readback), `IBL` convolves it into the
split-sum maps (`EnvironmentIbl::GenerateFromCube`) — one cube→SH / cube→IBL path for every
source, no per-source special case. (For a baked cube both read its lighting reduction, below.) `SceneView::EnvironmentIntensity`/`AtmosphereIntensity` are
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

**A layered bake may begin with a field, and a re-bake may re-composite over it.** A baked sky's
cost is usually one smooth, expensive evaluation (a volume march, a multiple-scattering integral),
while its look depends on full-resolution detail. A reduced-resolution Picture base only upsamples
that evaluation into the cube as a finished picture; a **Field** base
(`SkyBakeLayer::Role = SkyBakeRole::Field`, first layer only, `RequestBake` only) instead bakes
physical quantities at its `FaceSizeDivisor` into one to four `RGBA16Sfloat` field cubes
(`FieldTargets`, which must equal its Sky material's cooked `"fieldTargets"`; the fragment writes
them as ordinary multiple render targets), and every later layer reads them:

- **The job runs in phases, each tile at its own layer's cost**: every field tile, then one step that
  box-reduces each target's full mip chain (2×2 linear blits), writes each face's solid-angle-weighted
  sum (`sky_bake_field_sum.comp`, one workgroup per face, each texel weighted by its exact solid
  angle) and leaves the targets sampled, then the Picture layers' tiles — the first of them clearing
  every tile, since nothing was promoted beneath it. The costs ride
  `GeneratedTextureRequest::CostPhases`, so each layer's `Cost` prices its own ticks (a promote or
  field step charges one). The displayed cube is untouched until the composite lands.
- **The field is read through the view block.** The targets are registered bindless cubes
  (`g_Cubes`, set 2), and a Picture tile of a field bake writes their slots, the sums' storage-buffer
  slot, a trilinear clamp sampler and the field's face size into the view block's
  `SkyBakeFieldCubes` / `SkyBakeFieldInfo`. `Veng/sky_bake.slang` (which re-exports
  `Veng/sky.slang`) reads them: `SkyBakeField(pc, target, dir)` filters mip 0 with a cubic
  B-spline as four bilinear taps laid out on the direction's face (taps past an edge land on the
  neighbour), `SkyBakeFieldLod(pc, target, dir, lod)` samples a mip bilinear, and
  `SkyBakeFieldMean(pc, target)` sums the six face sums over 4π. Every other block — a scene view, a
  direct sky, a picture-only bake, the field layer itself — carries a zero count, and all three return
  zero. The block grew to 896 bytes for it.
- **Re-compositing.** `RequestBake`'s `fieldKey` names the field a request bakes. A field
  **stands** once its every tile, mips and sums have landed; a request naming the standing field's key
  at the same face size and target count skips the field phase and runs only the Picture layers
  (`GetFieldTilesRecorded` counts what ran). The field's first tile clears the standing key, so a
  supersede mid-field leaves none; a new face size recreates the targets and leaves none; an empty key
  always runs the field. A cache hit runs no tick, so it leaves whatever stood standing. The targets
  are kept between bakes (about 4.2 MB each at 256 per face with mips).
- **The cube needs the asset manager to bake a field**: `BakedSkyCube::Create`'s optional
  `AssetManager*` loads the sum shader and allocates its storage then; a field bake on a cube created
  without one is fatal. `tests/gpu/sky_bake_field.cpp` reads each of three targets back bicubically
  at every texel (face edges and corners included), the mean against the CPU's solid-angle sum of the
  same texels, and pins the re-composite, supersede and per-phase cost behaviour.

**A baked cube displays through its mips and lights from a float reduction.** The displayed cube
carries a full chain, and its consumer set's sampler is trilinear with the LOD unclamped, so the
skybox (`skybox.frag`, explicit gradients taken ahead of its depth discard) minifies a face finer
than a pixel's view angle without aliasing; a float bake rebuilds the chain by box blits when it
lands (`RecordAmortized`, and after the synchronous `Bake`/`BakeAtmosphere`). The IBL and SH never
read the display cube: they read the **lighting reduction**, at most `LightingFaceSize` (256) per
face — `GetLightingView` for the convolution, `GetLightingImage` at `GetShReadbackMipLevel` for the
SH readback. For a float cube it is the display cube's own mip tail from the first level at or below
256, so a float cube that small lights from exactly what it displays. The convolution samples the
reduction's mips too, so its filtered importance sampling has a chain to read.

**A baked cube may display block-compressed.** `BakedSkyCube::Create`'s display format is the
scene-colour float format (the default) or `Format::BC6HUfloat` — HDR radiance at one byte per
texel, where a 2048 float cube costs 200 MB twice over while a bake is in flight. Requesting it
needs `Context::IsBlockCompressionSupported()` (asserted; the caller checks and picks its own
fallback), an `AssetManager`, and a power-of-two face. BC6H blocks cannot be rendered to or blended
over, so a compressed cube bakes only through the layered `RequestBake`, and its Picture layers run
**tile-major**:

- **One tile grid for every layer.** `RequestBake`'s `encodeTilePixels` (default 256, clamped to the
  face) is a power of two dividing the face and at least the face over `MaxEncodeTilesPerAxis`
  (128). Each tick renders every Picture layer, in order and blended, into one tile-sized float
  target, through a viewport offset by minus the tile's origin, so each fragment keeps its face
  mapping. The per-layer `TilePixels` then applies only to a field phase; a Picture base with
  `FaceSizeDivisor > 1` is refused. A tick charges each layer's `Cost` scaled by the tile's area
  over that layer's own `TilePixels` area.
- **The per-tile encode.** The tick box-blits the tile's own mips, copies its mip at the
  reduction's scale into the lighting-reduction scratch, and encodes every face mip whose share of
  the tile is at least one 4×4 block (`sky_bake_bc6h.comp`, Krzysztof Narkowicz's real-time encoder
  under MIT, run with its two-region partition search so an isolated bright texel does not drag a
  block's colour line) into a buffer copied into the compressed scratch cube, whose producer access
  is `TransferDst`. One last step builds the reduction's mips and encodes the coarser face mips from
  them — the bound on the tile is what keeps those mips at 256 or below, where the reduction has
  them.
- **Double-buffered.** The compressed scratch and the reduction scratch are the job's two targets,
  so the bake cache stores and restores the whole chain and the reduction together (16 bytes per
  4×4 block; a mip under a block takes one partial block). `RecordAmortized` copies both across, so
  a renderer never reads either half-written. Peak memory is the two compressed cubes, two 256
  reductions, and one float tile with its mips; no full-size float cube exists.
- **Reading it back** is a sampling pass (`ReadbackTexels`), not a copy.
  `tests/gpu/sky_bake_compressed.cpp` holds a compressed bake to a float one of the same pattern: a
  smooth sky within BC6H's bound, isolated bright texels kept as their blocks' brightest, speckle
  within luminance and chroma bounds, the reduction and mips as box reductions, and a cached cube
  restoring its chain and reduction.

**A capture of the scene can light it in place of the sky — the lights model again.** A
`CaptureSurface` whose `Output` is `CaptureOutput::SceneLighting` is an environment probe: it renders
a radiance cube (`SceneCaptureInfo::Cube`) and binds no material. Each `Resolve`, the resolver takes
the scene's **first enabled such surface whose capture has completed a six-face sweep**
(`GetCubeRevision() > 0`; several enabled warns once, the `Sky` rule). Until that first sweep lands
the scene keeps the `Sky`'s lighting, so a cold probe never lights the scene black. While one is
active:

- **It is the IBL input whatever the sky is.** The displayed sky is always the `Sky`'s source and the
  skybox pass is untouched, but the effective tier is IBL — the one tier taking the capture's full
  directional content — whatever tier the `Sky` authors, under every source kind (an environment, a
  baked or direct sky) and with no `Sky` at all. The topology input carries it as
  `SkyTopologyInput::LightingCube`, so `IblAllowed` no longer needs a cube-backed sky; a change of it
  recompiles at the frame boundary like any tier change.
- **It re-derives once per completed sweep, never per frame.** The derive gate is the capture's cube
  identity *or* its revision: an on-demand probe re-lights when the sweep its `MarkDirty` started
  completes, an every-frame one re-convolves once per six frames. A pooled capture reads revision 0
  again after `ResetForReuse` until its new owner's first sweep, which publishes a revision past any
  its cube published before, so neither a stale cube nor a reused one is mistaken for current.
- **It overwrites the convolved maps, not the radiance cube.** `GenerateFromCube` writes only the
  irradiance and prefiltered maps, so an environment sky keeps its displayed radiance; while a
  lighting cube owns the maps an environment change regenerates the radiance cube alone
  (`EnvironmentIbl::GenerateRadiance`). When the capture goes inactive — disabled, destroyed, not yet
  swept — the `Sky`'s authored source and tier resume and the maps it overwrote are convolved again:
  a baked cube re-derives on tier entry, an environment through `Generate`.
- **It never lights its own faces.** `SceneCapture` marks every face view `SceneView::CaptureFace`,
  and the resolver ignores lighting captures there, so each sweep is lit by the `Sky` rather than by
  the sweep before it — a feedback loop that would compound the scene's light into its lighting.

A scene-lighting capture is the only input to this arm other than the sky's own cube.
`SkyResolver::GetLightingDeriveCount` (and `SceneRenderer`'s) counts its derives. The ambient arm takes IBL once its source is resident
(`SkyResolver::IsIblSourceResident`): a lighting cube, a loaded environment or sky material, or a
baked cube a bake has landed in. **Cost** is authored, as the tiers are: one convolution per
presenting renderer per completed sweep, plus the capture's own face renders.

**The sky has an orientation, and turning it is a lookup.** `Sky::Rotation` (identity by default)
orients the source in the scene: a feature the source holds at direction `d` is displayed at world
direction `Rotation · d` — for an application whose rendered frame turns relative to the backdrop.
The resolver normalises it into `SceneView::SkyRotation` for an environment, material or cube
source; an `AtmosphereSky` is not turned, since it is defined by the frame's up and the scene's sun.
The view block carries two lookup quaternions, **`SkyFromWorld`** (the conjugate: a world ray into
the source's frame) and **`EnvironmentFromWorld`** (the same, or the identity while the
image-based-lighting maps hold a scene-lighting capture's convolution, which was rendered in the
world frame). `Veng/view_constants.slang`'s `SkyDirection` / `EnvironmentDirection` apply them:
the skybox's cube lookup and `SkyViewDirection` (so a direct material sky and a baked one turn
alike), the SH skylight's evaluation, the irradiance and prefiltered lookups of the lighting core
and the sprite ambient, the IBL debug blit, and the reflection chain's environment fallback (its own
constants carry `EnvironmentFromWorld`). Nothing bakes in the turn: a bake face's view region
carries a zero quaternion, which turns nothing, so changing the rotation every frame re-bakes no
cube and re-convolves no lighting.

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

**Per-frame shading values ride the block, not a recompile.** `EnvironmentLightTint.rgb` is a
colour multiplying the sky's light (`SceneView::EnvironmentLightTint`, from the scene's
`RenderLook`, white by default): `Veng/lighting.slang` scales the IBL arm's diffuse and specular and
the SH skylight arm's diffuse by it, so it reaches the deferred pass and the forward loop alike, and
the reflection chain carries it into its environment fallback (D5). The skybox, the direct lights
and the flat arm are untouched, so a consumer can colour the light a scene's surfaces take from the
sky without recolouring the sky itself. `AmbientParams.x` carries the SH
skylight arm's intensity, and `.y` / `.z` the geometric specular anti-aliasing variance scale and
threshold (`SceneView::SpecularAntiAliasingVariance` / `SpecularAntiAliasingThreshold`, clamped to
non-negative), read by `MakeLightingSurface`; `.w` the normal-map specular anti-aliasing scale
(`SceneView::NormalMapAntiAliasingScale`, clamped likewise), read by `WidenRoughnessForNormalMap`. A
view block a pass writes without them (zero) shades with both widenings off.

**The block carries the exposure the tonemap applies to the view** — `ExposureParams`, x the resolved
exposure and y its reciprocal, written after the auto-exposure resolve so it is this frame's. A shader
reads the reciprocal through `ViewInverseExposure` (`view_constants.slang`), which treats a block no
tonemapped view wrote (zero, a sky-bake face) as an exposure of 1; a Translucent material keeping its
authored brightness passes its colour through `CompensateExposure` (`Veng/translucent.slang`).

**The block carries the view's frame count and its temporal resolve.** `FrameIndex` is the
renderer's executed-frame counter (the one the TAA jitter sequence reads), wrapped to 32 bits; each
`SceneRenderer` keeps its own, so two viewports count their own frames and a cube capture counts one
per face it renders. `TemporalResolveActive` is 1 when the view's anti-aliasing is TAA or TAAU
(`FrameTopology::TaaActive`). Together they let a material that jitters its sampling rotate the
jitter per frame only when a resolve will integrate it, and hold it still otherwise, where rotation
reads as crawl. The volume-field march takes its jitter rotation from here.

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
registered viewports (one slot each), one face per driven scene capture, and one slot per face an
amortized sky bake renders in a frame (its tiles share their face's slot; the display cube fills
through `GeneratedTextureService`, whose pump defers a tick that would eat the frame's own views) — so
ordinary content can want more than one
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
latency instead of starving whichever captures registered last. Its stride is **896 bytes**. The shadow system's own state — each cascade
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

### An infinite far plane

A perspective camera can project with **no far plane**: `CameraView::SetPerspective(fovY, aspect,
near, InfiniteFar)`, or a `Camera` component with `InfiniteFar` set (a flag beside `Far`, so `Far`
stays a finite number a JSON record holds; `MakeCameraView` ignores `Far` under it). The projection
is built in its exact infinite reverse-Z form — `P[2][2] = 0`, `P[2][3] = −1`, `P[3][2] = near` —
so device depth is `near / viewDepth`: 1 at the near plane, falling toward 0 without reaching it,
and the cleared 0 stays the background. `GetFar()` returns `InfiniteFar`; `IsFarInfinite()` names
the case. An orthographic projection is always finite (`SetOrthographic` asserts it).

**Precision does not change in kind.** Reverse-Z in a float buffer keeps a nearly constant relative
resolution of about 2⁻²³ in view depth whatever the far: two surfaces resolve apart at about 0.12 m
at 1,000 km and 120 m at 10⁶ km, and a finite far of 10⁶ resolves essentially the same. At
`near = 0.1` device depth stays a normal float out to about 10³⁷ m. A consumer writing its own depth
through `GBufferDepthOutput` (a scaled draw's true depth, `TerrainTrueDeviceDepth`) writes
`clip.z / clip.w` of the true position through the view's projection, which is representable at any
finite distance; nothing about the encoding changes.

**What a consumer must not do under it.** Never unproject at device depth 0: the far plane has
`w = 0`, so `inverse(viewProj) · (ndc, 0, 1)` is a point at infinity and its divide is `inf`/`NaN`.
Cast a ray with `ViewRayThroughNdc` (or solve a point with `ViewPointAtDepth`) instead, and take a
direction through `InvViewRotProj` and `normalize`, as the sky does. A `Frustum`'s depth-0 plane has
a zero normal and passes every point; a consumer normalizing planes skips a zero-length one, as the
sphere `Intersects` does. And a near floor derived from the far (`Far / ratio`) is infinite here: a
consumer deriving its near that way takes its own finite floor instead.

**The audit.** Every engine reader of the far distance or of a reconstruction through the
projection, and how it stands under an infinite far:

- **The cascade fit** — fixed. It finds a finite range before building any corner: the camera's
  far capped by `MaxDistance`, else the scene bound's farthest corner, else no cascade (Count 0,
  which the shadow pass and the lighting path already treat as unshadowed).
- **`Viewport::ScreenToWorldRay` and the `SystemViewInfo` `ScreenToWorldRay`** — fixed. Both
  unprojected the far plane; both now cast through `ViewRayThroughNdc`. The `SystemViewInfo` form
  also had its near and far swapped (it took NDC 0 as the near plane), so its ray started on the far
  plane and pointed back at the camera.
- **The camera gizmo** — fixed. It pulls the far in for its frustum and now also clears the flag.
- **The depth-of-field prefilter** — fixed. Its divisor floor of 10⁻⁶ clamped every surface past
  10⁶ near to that depth (the infinite projection's depth row has no offset); the floor is 10⁻³⁰.
- **Correct as written:** deferred lighting and the forward loop's world reconstruction (finite at
  every drawn depth; cleared depth skipped), SSAO (skips cleared depth; inverts the projection, which
  is invertible), SSR (skips the background; the denoiser's linear depth of a background history
  texel is `inf`, which weights it to zero), sprites' and particles' soft fade
  (`ViewDepthFromProjection` reads depth 0 as `+inf`, a full fade-in), the ribbon occlusion test
  (skips cleared depth), the volume field and the light-tile cull (geometry depth only; the ray
  through the rotation-only inverse), the sky (`normalize` drops the `w` of 0), the hi-Z pyramid and
  occlusion test (min-Z, the background never occludes), TAA's background reprojection (depth-free),
  the LOD metric and instanced-set projected size (`P₁₁`, distance, and the eye from the inverse's
  z column, which the infinite form keeps), every frustum consumer (the box test, the instanced cull
  shader's copy, the sphere test, the cube-face reach test and the cascade clip all pass a
  zero-normal plane), and sphere terrain (its selection reads the four side planes only).
- **Independent of the camera's far:** punctual shadow views (the light's range), model portraits,
  the editor fly camera, and the depth debug view. `SceneCapture` carries its own planes
  (`CaptureView::Near`/`Far`, which may be `InfiniteFar`), and its distance resample linearizes
  through `near / far`, exact under an infinite far.
- **No MCP tool or editor panel prints a camera's far** other than through the reflected fields,
  which carry the flag and a finite `Far`.

### Culling and the BVH broadphase

The scene-drawing passes **cull at submesh granularity through a BVH broadphase**. A
renderer-owned `SceneBroadphase` (`Veng/Scene/SceneBroadphase.h`) holds a bounding volume
hierarchy whose **leaves are per-submesh** — one leaf per `SubMesh`, on its local-space `AABB`
folded over the submesh's index range at load (no cooked-format change). Each `Execute` calls
`SceneBroadphase::Sync`: **only on a frame the scene's spatial version moved** (or a still-loading
mesh became resident, or the scene is another instance — told apart by `Scene::GetInstanceSerial`, never
by address, which a scene built after another's destruction reuses) it re-gathers the candidates (the pure `GatherMeshes` pass,
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
(`hi_z_reduce_tail.comp`), bit-identical to the per-level reduction. **The reduction runs only while
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

### Level of detail

A cooked mesh may carry **coarser levels of detail** (`Mesh::GetLodCount()`, `GetLods()`,
`GetLodRange(level, subMesh)`): each level is a further index range per submesh over the mesh's one
vertex buffer, so a level draws with its submesh's material and a vertex offset of zero, and an
empty range drops that submesh at that level. Level 0 is the submesh table itself, and **the
bounds, the broadphase leaves and every cull stay level 0's** — a coarser level only ever draws
inside them, so the cull stays conservative and the hi-Z test needs nothing new. The cook side
(generated by meshoptimizer, or authored) is in [cooker/CLAUDE.md](../../../cooker/CLAUDE.md); a
runtime-built mesh carries levels through `MeshData::Lods`.

- **The metric is projected screen size**: the mesh's level-0 bounding sphere — the bound's centre
  through the world matrix, its half-diagonal times the largest axis scale — as a diameter over the
  view's height, `radius · P₁₁ / distance` under a perspective projection and `radius · P₁₁` under an
  orthographic one (`LodView`, `ProjectedScreenSize`, `src/Renderer/LodSelection.h`). It measures the
  **distance to the eye, not the view depth**, so turning the camera in place moves no mesh across a
  threshold, and it is resolution-independent. Level *i* (≥ 1) is wanted while the size is below its
  `MeshLod::ScreenSize`; the thresholds descend.
- **Hysteresis.** Each threshold is widened into a band of ±`SceneRendererSettings::LodHysteresis`
  (relative, default 0.1, clamped to [0, 0.5]); `SelectLod` keeps the level a mesh drew last frame
  while its size stays inside every band it would otherwise cross, so a mesh resting near a threshold
  — or jittering across it — does not alternate. The camera view's memory is per entity: two
  `EntityFrameTable<u8>` tables on `SceneRenderer::Internal`, swapped at the start of each gather the
  way the world matrices are. With no memory (an entity first gathered) the threshold itself decides.
- **Bias.** `SceneRendererSettings::LodBias` multiplies every size by 2^−bias before the comparison,
  in every view: +1 switches each level at twice the distance. The level-of-detail fields are read
  per frame, so a `Configure` that changes only them rebuilds nothing.
- **A view shown smaller than the screen says so.** The metric is a fraction of the view's own
  height, which is the screen's only for a view that fills it. `SceneView::LodScreenScale` — the
  fraction of the presenting surface's height the view's image covers, 1 by default — multiplies
  every size in the view and its shadow views, so a view rendered into a small image inside a panel
  (a model portrait) selects what its meshes' size on the actual screen calls for.
- **Forced levels, most specific first:** a per-entity **`LodOverride`** component
  (`Veng/Scene/Components/Mesh.h`, reflected and editor-visible; `GatherMeshes` copies it onto
  `VisibleMesh::ForcedLod`), then **`SceneView::ForcedLod`** (one render's override, no `Configure`),
  then **`SceneRendererSettings::ForcedLod`** (the renderer's; reflected, so the debug override
  layer and `UI::RenderSettingsEditor` reach it and the bias and band with no other change),
  then selection. A forced level clamps to each mesh's coarsest, so `CoarsestLod` forces the lowest
  detail everywhere; `AutomaticLod` (−1) is "not forced". `ResolveMeshLod` is the one function that
  applies the order.
- **One selection per visible mesh, made before the gather.** `SceneRenderer::ResolveCameraLods`
  runs in `PrepareDraws` right after the frustum cull and fills a level per `SceneView::Visible`
  entry, published as **`SceneView::MeshLods`**. The three gather phases read it through
  `DrawGatherInput::MeshLods`: `DrawKey` carries the level (sorted after the mesh, so a submesh's
  instances at one level stay one instanced run), a slot's index range is that level's, and a
  submesh its level drops claims no slot. So **the CPU path and the GPU-cull path agree by
  construction** — the GPU commands are written from the same slot and the same range in the same
  loop (`GpuCullCandidate::IndexCount`/`FirstIndex`); the compute adds only occlusion and never
  selects. Picking, the g-buffer shading override and the depth+normal prepass draw the camera's
  level too (the prepass reads `SceneView::MeshLods`), so a pick or a portrait's depth matches what
  the colour pass drew. A draw's level changing between frames moves nothing per-object velocity
  depends on: the velocity is the world-matrix delta, and both levels approximate one surface.
  **`tests/gpu/scene_renderer.cpp` pins that a CPU-culled and a GPU-culled render of a levelled mesh
  agree pixel for pixel, and that forcing level 0 draws a different picture.**
- **Shadow views select their own level**, from the projection each rasterizes through — a cascade
  its tile's orthographic matrix (so a far cascade covering more world draws coarser casters), a
  punctual face its perspective — with the same bias and forced levels but **no hysteresis**: a
  shadow view keeps no memory across frames, and its projection holds still while its light and the
  caster do (`ShadowCasterViews::AddView`'s `lodViewProj`). A caster is selected once per mesh per
  view. Because two views may draw one caster at different levels, `DepthCasterGrouping` keys a
  shared caster by (candidate, level): views drawing it at one level share an entry, another level
  gets its own, and each view's draws are still exactly what grouping its casters alone would give.
  Skinned casters draw level 0 — the cook gives a skinned mesh no levels.
- **The draw budget is unchanged.** A level changes a slot's index range, never the number of slots
  (a dropped submesh claims none), so `DrawBudget`'s cursors, limits and the static range's
  contiguity from 0 hold as before.
- **The cost** is one projected size per levelled visible mesh per view — a matrix-vector product and
  a length — and one 4×4 inverse per view; a single-level mesh skips selection entirely.
  `tests/unit/lod_selection.cpp` pins monotonicity in size, the hysteresis band, the metric's
  invariance to the view's direction, and the forced-level order.

### Instanced mesh sets

**An `InstancedMesh` entity draws one mesh with one material at every instance of a store** — the
tool for scattered debris, vegetation and props, where an entity per copy would cost
per-entity work every frame and a draw candidate per copy, and the frame's `MaxCullCandidates` is
far below the copy count. The component (`Veng/Scene/Components/Fields.h`) names the mesh, the material,
`CastsShadows`, `Visible`, the projected-size band and the minimum shadow caster size, and holds a
runtime-only `Ref<InstanceSet>` (`Veng/Renderer/InstancedMesh.h`): the CPU store of
`MeshInstance`s — a 3×4 affine transform into the entity's frame and one `vec4` datum, 64 bytes —
with `Resize` (capacity), `SetCount` (the drawn prefix) and `Write` (a range). `InstancedMeshSystem`
(`src/Renderer/InstancedMeshSystem.{h,cpp}`) owns the vertical slice.

- **A set is one bound, one slot, a handful of draws.** The store keeps a conservative bound
  (instance origins grown by the largest instance scale), so the renderer bounds the whole set
  without visiting it: each view rejects a set whole by that one box (the scene BVH never sees
  instances), and it folds into the scene and caster bounds the cascades fit to. Each set takes one
  `DrawBudget` slot after the entity phases (`DrawPhase::InstancedSet`), whose `DrawData` record is
  the set's frame, previous frame and material; its instances are culled and drawn outside the
  budget, as one draw per level and submesh per view.
- **Uploads are the ranges written.** Every per-frame buffer has one copy per frame in flight, so the
  host never writes what a frame still reads. One host-mapped **data buffer** holds every set's
  records (each set's whole capacity, pooled in gather order) followed by the frame's survivor
  lists, and is registered in the set-0 storage array. A frame slot uploads, per set, the union of
  the ranges `InstanceSet::Write` recorded since that slot last saw the store
  (`GetWrittenSince`; it keeps `MaxTrackedWrites` ranges, and a slot further behind, or behind a
  `Resize` or a change of the pooled layout, uploads the store whole). The indirect commands and
  the cull's tables are copied per frame the same way.
- **The cull is per instance and per view, and runs on the GPU whatever the entity cull mode.**
  The views are the main camera then every shadow view `ShadowCasterViews` holds (cascades, then
  punctual faces, in render order; `AddView` records each view's cull and level matrices and tile
  size). A job is a (view, set) pair the view does not reject whole. Where
  `Context::IsGpuDrivenCullingSupported()` holds, **one dispatch per frame** covers every job
  (`instanced_mesh_cull.comp`, one invocation per instance, a binary search over the job table's
  invocation bases) — under `CullMode::CPU` too, which is the entity default a consumer may never
  change. Elsewhere, or under `SceneRenderer::SetInstancedMeshForceCpuCull`, the same decisions run
  on the host (`CullInstance`, `src/Renderer/InstancedCull.h`, the device-free twin the unit band
  pins) into the same lists — O(N) per view, the fallback and not the path — and the draws are
  direct with the counts the host knows. An invocation tests the instance's world box against the
  view's frustum, its size against the band, a shadow view's minimum caster size, and in the main
  view, while the entity GPU cull holds a reduced pyramid with valid history, last frame's hi-Z
  (through its bindless slot; `hi_z_occlusion.slang`'s footprint is shared with the entity cull).
  A survivor selects its level and appends its index to that level's list, counting itself into
  every submesh's command of that level (the first one's count hands out the slot). The host wrote
  each command's index range and a zero count beforehand.
- **Levels follow the entity rule and keep its hysteresis.** Size is `ProjectedScreenSize` of the
  mesh's level-0 sphere under the instance's transform, with the view's bias and screen scale; a
  shadow view measures through its own projection with no memory. The main view keeps one level per
  pooled instance in a device buffer (`InstanceNoPreviousLod` when culled), which the cull reads and
  writes, so an instance resting at a switch distance does not flip. The memory is trusted only for
  a set the main view culled last frame under an unchanged pooled layout. Forced levels apply as for
  entities (the set entity's `LodOverride`, then the view's).
- **The projected-size band is measured in the main camera in every view.**
  `MinProjectedPixels`/`MaxProjectedPixels` cull by `ReferenceProjectedPixels` — the level rule's
  size with no bias, times the main view's rendered height — so a shadow view casts exactly the
  instances the main view's band keeps, not what its own wider projection would. The function is
  exposed on both sides (`Veng/Renderer/InstancedMesh.h`, `Veng/instanced_mesh_math.slang`) so a
  consumer drawing the instances below the band another way partitions them with the identical
  expression, from the view block's `CurViewProj`, `CameraPosition` and `ExtentParams.y`.
  `MinShadowCasterTexels` skips, in a shadow view, an instance spanning fewer of its texels.
- **Drawing.** The g-buffer pass draws the main view's jobs after (and independent of) the entity
  draws through the material's instanced-set pipeline (`Material::EnsureInstancedPipeline`); each
  shadow pass draws its views' jobs through a depth-only pipeline of the pass's own state
  (`shadow_depth_instanced.vert`; the cascades with depth clamp). Neither pipeline has an
  instance-rate binding: the draw's `firstInstance` is the level's list start in the data buffer, so
  `SV_VulkanInstanceID` (which counts from it) names the list entry, and the entry the record. The
  surface push grows by three fields (`SurfacePush`: the data buffer's slot, the set's `DrawData`
  slot, its first record), zero for every other draw. The drawing passes declare
  `StorageBufferRead` on the data buffer and `IndirectRead` on the commands, so the barriers from
  the cull are graph-derived.
- **Shader access.** A Surface fragment reads its instance through `Veng/instanced_mesh.slang`:
  `InstanceDatum(input)`, `LoadInstance`, `IsInstanceDraw`, and `InstanceObjectPosition(input)` —
  the mesh-space position, carried for every surface draw as `SurfaceFragmentInput::v_ObjectPosition`
  beside `v_Instance` (the data buffer's slot and the record index, `InstanceNone` for any other
  draw). The material must declare `"instancedSets": true`; a set drawn with one that does not is
  reported once and skipped.
- **Presence-driven.** The cull pass and the passes' reads are wired while the scene carries a
  drawable set (`m_InstancedActive`, the volume-field model), so a scene with none is unchanged.
- **Diagnostics.** `GetDrawBudgetStats()` counts the sets, their instances and any set dropped for a
  slot; `GetInstancedMeshStats()` adds the views, jobs and draws, and per (set, view) the survivors
  per level, read back from the commands as many frames later as there are frames in flight; the `Render/Instanced*`
  counters sample the same. `ReadbackInstancedMeshSurvivors()` idles the device and returns every
  survivor of the last frame — the test surface.
- **What a set does not do.** It is not pickable, not drawn under `GBufferShadingOverride`, and not
  drawn by the depth+normal path; an instance's own motion is not in the velocity (its previous
  transform is the set's previous frame times its current one); one set draws one opaque material
  on every submesh; a skinned mesh is refused.

`tests/gpu/instanced_mesh_set.cpp` pins the properties on fields of cubes: what is in view survives
and nothing else, levels are monotone in distance and hold inside the band, the band partitions in
the main view and bounds what the cascades cast, the cull ignores the entity mode, fifty thousand
instances take one slot, a partial write moves exactly what it wrote, a set casts into the
cascades, and the host fallback keeps what the GPU cull keeps; `tests/unit/instanced_mesh.cpp` pins
the store's write ranges and bound and the host cull's rules.

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

### Splitting a frame for platform GPU work

**`Context::SplitFrame(encode)` lets work a platform API encodes outside Vulkan run inside a frame.**
It ends the frame's command buffer, submits it as one segment signalling `FrameSplitPoint::WaitValue`
on the context's interop timeline — a timeline semaphore created exportable on the first split, whose
`MTLSharedEvent` is `FrameSplitPoint::SharedEvent` — calls `encode`, and resumes recording in a spare
command buffer whose submit waits `SignalValue` at every stage. The resume swaps the spare's handle
into the frame's own `CommandBuffer` object, so every caller holding it keeps recording unchanged,
the frame's draw count carries over, and each handle in a slot's set is reused only once that slot's
fence is waited. Only the first segment waits the swapchain acquire; every segment drains the pending
transfer waits; only the last signals the present semaphore and the frame fence, whose first scope
covers every earlier batch. A split inside an `ImmediateCommands` recording does the same into
buffers its own fence frees, with a pending wait kept apart from the frame's.

**The external work must be committed to a queue of its own.** Under asynchronous submission (below)
the driver may not yet have encoded the segment the work waits on, so a wait committed to the frame's
own Metal queue would hold that queue against a submit queued behind it; on a separate queue the
shared-event waits order the three pieces on the GPU whatever order they were committed in.
`IsFrameSplitSupported()` is the platform-object interop extension being enabled.

**A validation build resolves the external signal on the host.** The validation layer models only
signals made through Vulkan, so a segment waiting on the value the external work signals through the
shared event is, to the layer, a wait nothing will ever resolve: it reports the frame's present as
depending on an unsubmitted signal (`VUID-vkQueuePresentKHR-pWaitSemaphores-03268`), and
synchronization validation parks every batch behind it, so each present after looks unsynchronized
against the acquire. Under `VE_ENABLE_VALIDATION_LAYERS` the split therefore waits for the external
value on the CPU (`vkWaitSemaphores`) and signals the next one from the host, which the rest of the
frame waits instead (`Context::Native::ResolveExternalSignal`) — a Vulkan signal made before anything
waits on it. That stalls the render thread until the GPU reaches the split, which only a validation
build pays; every other build, the profiling tree included, keeps the GPU-side wait. The headless
band cannot show the difference, since it presents nothing.

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
  Descriptor set layouts retire too. A drop outside a frame parks in `PendingRetire`, adopted by the
  next `BeginFrame`. The one release a submit drives is an `ImmediateCommands` recording made outside
  any frame and not nested in another: once its own fence is waited, `DrainPendingRetiresIfIdle`
  detaches the pending bin under the retire lock, then checks without blocking that every frame
  slot's fence has signaled and the transfer timeline has reached its last submitted value, and only
  then destroys what it detached; otherwise the handles go back to pending. Detaching before checking
  is what makes it safe against a worker: a handle is retired after every submit that uses it, so
  the checks see all work the detached handles could be in. This is what keeps a process that never
  begins a frame — a headless tool, a self-test — from exhausting the descriptor pool.
- **Slot-keyed deferrals** — a bindless slot's `Release`, a material range — file under
  `Context::Native::GetReleaseSlot()`: the recording slot inside a frame, the last *submitted* slot
  outside one. Between `EndFrame` and the next `BeginFrame` (asset finalizes, the world tick,
  `OnUpdate`) `GetCurrentFrameInFlight()` already names the slot about to be waited, whose frame is
  the *older* one; filing a release there reclaimed the slot while the newest frame could still read it.
  A bucket is reclaimed at its slot's next acquire, or — every bucket at once — by the same drain that
  follows an outermost out-of-frame `ImmediateCommands`, when every frame fence has signalled
  (`BindlessRegistry::ReclaimAllReleased`, ahead of the retire-bin detach so what the slots held
  retires into that drain). Without it a process that never begins a frame never gets a slot back.
  Why the early reclaim is safe:
  - A release follows the last *recording* of a use of the slot. At the drain no frame command buffer
    is open, the recording has completed (held setup work recorded at its head, nested recordings
    synchronous), and every submitted frame has finished — so no pending command buffer can index
    the slot, which is what `UPDATE_UNUSED_WHILE_PENDING` asks of the rewrite a re-`Register` makes.
    Descriptor writes are immediate `vkUpdateDescriptorSets` calls, never batched, so no queued write
    is left to order against.
  - The transfer queue binds no descriptor set, so the transfer timeline does not hold a slot back;
    the resources a reclaimed slot let go of retire through the pending bin, which it does gate.
  - Inside a frame the drain does not run: the frame's open command buffer may already hold a draw
    through a slot released after it, and only that frame's fence retires the slot. In a process that
    does run frames, an out-of-frame recording between `EndFrame` and `BeginFrame` reclaims only
    when the last submitted frame has finished, and each slot sits in exactly one bucket that the
    reclaim empties, so the next acquire finds nothing to free twice.
  - There is no off-thread release. The registry is unsynchronized and driven from one thread, the
    one that registers — which is also the one an out-of-frame `ImmediateCommands` runs on (it
    already resets the registry's view budget there). A worker's drops reach the drain through the
    retire bin, under `RetireMutex`, never through a slot release.
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
predicates live in the core shader header `Veng/surface_flags.slang`. Four independent flags are
defined. `SurfaceFlagNoShadowReception` is written by a material whose drawn geometry does not stand
where its shading says it does (an impostor, a sky proxy, a matte-painting shell, a proxy drawn at a
compressed distance), so the lighting pass takes full visibility in place of every shadow atlas
lookup — the receiving counterpart of `MeshRenderer::CastsShadows = false`.
`SurfaceFlagNoEnvironmentLighting` is written by a material the view's sky should not light: the
split-sum IBL and SH skylight arms are replaced, for that surface alone, by the view's flat ambient
floor (`SceneView::AmbientFloor`, carried in the view block under every arm), so a flagged surface in
shadow reads dim rather than black; its direct lights and shadows are untouched.
`SurfaceFlagNoAmbientDiffuse` is written by a material that supplies its own diffuse lighting: the
ambient arm's diffuse term (irradiance, SH skylight or flat floor) is dropped and its specular term,
the split-sum reflection of the environment scaled by occlusion and SSAO, is kept — the one ambient
decline an occlusion of zero cannot express, since occlusion scales both terms.
`SurfaceFlagExcludedFromMetering` is written by a material whose emission is a source of light — a
lamp, a light panel — that the auto-exposure meter should not read (see "Auto-exposure"); its own
shading is untouched. Beside them the surface's **light channels** (see "Light channels" under
Shadows) are written through `SurfaceLightChannels(mask)` and read back by `GBufferLightChannels`. All
are material properties rather than per-instance ones because they describe how the surface shades
(a per-instance value reaches a material through an instance parameter). The channel holds an
eight-bit code over `/255`, which UNORM8 stores exactly: bit 0 shadow reception, bit 1 environment
lighting, bits 2–4 the channels, bit 5 the ambient diffuse, bit 6 free, bit 7 metering — kept highest, since a translucent material raises it
by a MAX blend (see "Additive translucent materials"). Membership is stored **relative to the
default**: bit 2 marks a surface that leaves channel 0, bits 3 and 4 one that joins channels 1 and 2,
so `SurfaceLightChannels(0x1)` is 0. Each flag's value is its bit's level, so a material writes any
sum of them (`SurfaceFlagNoShadowReception + SurfaceLightChannels(0x2)`), or `SurfaceFlagsNone`; a
material writing 0 is unaffected. `tests/gpu/light_channels.cpp` writes all 128 combinations of
the three lighting flags, the channel bits and the metering bit and reads each back from the target,
and the MAX blend over a surface carrying every lower bit. The
lighting pass and the meter read the channel with a **point load** (`LoadSurfaceFlags`), not through
the g-buffer's linear sampler: no scalar encoding of independent flags survives a filtered blend between surfaces. The forward path carries the three
lighting flags as `ForwardSurface::DeclinesShadowMaps` / `DeclinesEnvironmentLighting` /
`DeclinesAmbientDiffuse`, and the membership as `ForwardSurface::LightChannels`, which a translucent
material sets before `EvaluateForwardLighting`; `ForwardSurface`'s default constructor clears the
flags and starts in channel 0. The metering flag reaches the g-buffer from a translucent material through its own output (see "Additive
translucent materials"), since the meter reads it after the translucent passes have drawn. Velocity (G3, `RG16Sfloat`) is the per-object screen-space motion
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
ORM and emissive are always stored (lighting reads them; the debug arms read some), as is
depth. **Velocity is stored only when the TAA/TAAU resolve, the `MotionVectors` arm or the
screen-space reflection chain is wired**; under every other configuration — the default
`AntiAliasingMode::None`, FXAA, CMAA2, every other debug arm, with SSR off — the pass declares `StoreOp::DontCare` on it, so `GetVelocityView()` stays non-null but
its contents are undefined after the frame. On an immediate-mode GPU the discard costs and saves
nothing.

### A Surface fragment may read the shadow maps

**An opaque surface computing part of its own lighting keeps the scene's shadows.** A Surface
fragment including `Veng/light_visibility.slang` declares the shadow system at **set 5**
(`SurfaceShadowMapSet`) and asks `LightVisibility(vc, lightIndex, worldPosition, normal, filter)`
how much of a light reaches a point — the deferred loop's own lookups, the bias taken along the
normal, so at its pixel's position (`SurfaceWorldPosition(sv_position)`, `Veng/surface.slang`, the
position the deferred pass reconstructs) and with the normal it writes it reads what the deferred
pass multiplies a Directional, Point or Spot light by; an area light's tile is read through the plain
lookup rather than PCSS. `FindLightByTag` finds the light. The answer holds the opaque casters only,
as everywhere.

- **Set 4 is reserved in the Surface domain** — the skinning palette, or a sphere terrain's tiles —
  so the cook refuses a Surface fragment declaring it outside `"sphereTerrain"`, and any set past 5.
- **The g-buffer pass builds its pipelines.** The shadow set's layout carries the renderer's
  immutable comparison sampler, and only an identically-defined layout accepts the renderer's set, so
  such a material (`Material::ReadsShadowMaps`) holds no pipeline of its own: its layout is the
  stages' sets 3 and 4 (an empty set 4 where they declare none), the shape the shading override
  builds against. `SurfaceShadowPipelines` (`src/Renderer/SurfaceShadowPipelines.h`, one per
  renderer) builds each such material's static, skinned and instanced-set pipeline against
  `{DrawData, set 4, the renderer's shadow layout}` (`Material::BuildShadowReadingPipeline`) the
  first time it is drawn that way, keyed on the material's layout and the kind, and binds it with the
  renderer's shadow set at 5 and the frame-slot dynamic offsets the lighting pass uses. Every other
  material binds exactly as before. A sphere-terrain material reads it the same way, through its
  terrain set at 4.
- **The pass declares the shadow atlases sampled** whenever they exist, so the graph orders the
  shadow writes ahead of the g-buffer pass rather than ahead of the lighting pass.
- **What it costs:** the lookups run in the g-buffer pass, per covered pixel (nine comparison taps a
  lookup under `LightVisibilityPcf`, eighteen in a cascade blend band), on top of what the deferred
  pass spends.

**The worked case: a self-lit surface.** A surface whose directional light needs terms the deferred
pass cannot know — an analytic occluder, a closed-form multiple-scattering term, a sky-dependent
tint — writes its real albedo, leaves that light's channel and joins the channel the local lights
meant to reach it are on (`SurfaceLightChannels(0x2)`, the directional on channel 0 and those lights
on channels 0 and 1), writes its own directional term, shadowed through `LightVisibility`, into
emissive, and computes that light's specular itself with `Veng/brdf.slang`. Every local light then
lights it through the ordinary deferred path; the ambient is the sky's with SSAO, the flat floor
under `SurfaceFlagNoEnvironmentLighting`, the sky's reflection alone under
`SurfaceFlagNoAmbientDiffuse` (for a surface that carries its sky-lit diffuse in its emissive too),
or none with `ORM.r = 0` (the ambient arm and SSAO scale by occlusion, never direct light). `tests/gpu/surface_light_visibility.cpp` holds a shadow-reading
plane under a box caster to the regions where its default-lit twin reads shadowed and lit, and the
skinned and instanced-set stages to reading an occluded and an open surface as such;
`tests/gpu/sphere_terrain.cpp` a terrain material likewise.

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

**An overlay can hold its brightness against the exposure, and the meter never sees one.** A
pre-bloom overlay is composited into the scene HDR, so the tonemap multiplies it by the frame's
exposure like the scene behind it. `GuiOverlay::CompensateExposure` (conveyed as
`GuiHdrOverlayView::CompensateExposure`) divides the light it adds by the frame's resolved exposure
on the way in, so it reaches the tone curve as it would at an exposure of 1 — an interface stays
legible as auto-exposure moves. It is applied through the blend, not the shader: the direct path's
`GuiScenePass` pipelines and the composite pipelines weight the source colour by the blend constant
(`BlendFactor::ConstantColor`, dynamic per pipeline that uses it, `CommandBuffer::SetBlendConstants`),
so a composite material or a GUI fill material needs no cooperation, and the coverage taken from the
scene and a material's bloom-mask amplitude are untouched. The direct path records the compensating
overlays as a second merged list. Separately, the auto-exposure meter is declared **ahead of** this
pass and reads the same target, so it meters the scene without any overlay: a bright panel never sets
the exposure of the world behind it.

**A composite may replace the scene beneath its document with a blurred copy of it.** A material
declaring `Backdrop` (texture), `BackdropSampler` and `BackdropParams` (`float4`: the level count,
the reciprocal of the composite's colour weight, the reciprocal of the extent) reads, through
`SampleOverlayBackdrop` (`Veng/overlay_composite.slang`), the tail's scene colour as it stood before
any overlay composited this frame, at a blur in [0, 1] across a `SceneColorChain`
(`BlurLevelsFor` levels) at the post-resolve allocation. It composites its document over the
sample and emits at full coverage, so the blend replaces what was there; the sample is pre-divided
by the blend constant, so at blur 0 a `CompensateExposure` overlay restores the scene exactly too.
The chain and its copy passes are wired while a conveyed composite declares `Backdrop`
(`ResolveHdrOverlays`, structural like the composite count) and record only on a frame such a
composite has a non-empty rect (`GetBackdropSampledFlag`, `SkipWhenUnread`); they are declared at
the head of the pass's contribution, so the backdrop holds no overlay and one frosted overlay never
blurs another. Every composite declares a sampled read of each level.
`tests/gpu/gui_hdr_overlay.cpp` holds that blur 0 changes nothing (compensating or not), and that a
blur softens a silhouette inside the document's rect and moves nothing outside it.

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
  (worker-legal creation; the march binds it through a dedicated set, below, and `Finalize()`
  separately registers it into the bindless volume set for a material that samples it). The
  reflected **`VolumeField` scene component** (`Veng/Scene/Components/Fields.h`) carries the authored,
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
  **translucent blends over it**, and — the load-bearing one — the **TAA
  resolve is the march jitter's integrator** (the per-pixel start offset is dithered and temporally
  rotated by the view block's `FrameIndex` precisely so TAA converges it to a smooth result; without TAA the march reads as
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
  argument buffer is a MoltenVK mistranslation risk the engine refuses once, and the march needs no
  global registration. A field a material also samples is registered by `VolumeField::Finalize()`.
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
  the resolved `CameraView`, `Delta`, and the knobs a look does not own — the input subset of the
  renderer's internal `SceneView`); the viewport never reaches into the scene for a camera. A null
  `World` renders nothing (a closed document is a no-op, not a null deref). The viewport retains
  the camera for screen-to-world mapping, and the whole view (`GetViewState`).
- **The viewport resolves the scene's look itself.** In `Render`, before the `SceneView` is built,
  it reads the presented scene's `RenderLook` (`Veng/Scene/Components/RenderLook.h`; the first walked wins,
  with a warning once if there are several). When the look differs from the last one resolved
  (`RenderLook` has `operator==`) or the owner called `InvalidateLook`, it runs its **look resolver**
  (`SetLookResolver`; a `ViewportCompositor` hands its own to every viewport it registers, and an
  unset one is `ApplyRenderLook` alone) from the current settings and `Configure`s the result, which
  no-ops an unchanged topology; then, every frame, it writes the resolved per-frame fields over the
  pushed view (`CopyLookKnobs`). So a per-frame look field (exposure, a bloom knob, the ambient
  floor) is live on the next frame with no rebuild, a topology field (bloom, shadows, AO) costs one
  rebuild — the same frame-boundary recompile a `Sky` source change takes — and an unchanged look
  costs a comparison. A scene with no look renders the pushed values on the topology last
  configured: an editor's prefab preview, or a consumer driving `SetViewState` itself. Because a
  look edit can `Configure` inside `Render`, an owner sampling the output re-fetches it on a
  `GetOutputGeneration` change rather than only after its own `Configure`.
- **`Render(cmd)` does Execute + the Sample barrier.** It applies any pending region resize,
  resolves the look as above, builds the internal `SceneView` from the bound `ViewState`, calls `SceneRenderer::Execute`, then
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

### The debug override layer

**A developer panel's edits need a layer that wins over both the application and the look.** An
application's graphics resolve (`OnResolveGraphics`, run as the viewport's look resolver)
legitimately rewrites every field its saved quality tiers own whenever a look resolves, and the look
writes its per-frame knobs over every push — so a field set through `Configure` or a pushed view
lasts only until the next resolve, which a system writing the look triggers as often as it likes.
`ViewportDebugOverrides` (`Viewport::SetDebugOverrides` / `GetDebugOverrides` /
`ClearDebugOverrides`) is that layer, and it composes last:

- **Settings:** `GetSettings()` is what the owner and the look resolve configured (the read side of
  `Configure`, and what a resolver starts from, so no override is baked into it);
  `GetEffectiveSettings()` is that with the overrides written over it, and is what the renderer is
  configured to. A changed override set configures it at the next `Render`, through the same
  configure-only-on-change compare, so an override that leaves the topology as it is rebuilds
  nothing and a held one costs nothing per frame.
- **View:** `GetViewState()` is the push with the look's knobs written over it;
  `GetEffectiveViewState()` adds the overrides, and is what `Render` builds its `SceneView` from. So
  an override wins over the look, over a resolver's view, and over a managed viewport's
  default-constructed push — including knobs no look carries.
- **Render scale:** while set it is the scale (`GetRenderScale()` reports it), the scene is
  allocated at exactly it and the dynamic-resolution step holds still; `SetRenderScale`, holds and
  controller changes made meanwhile are recorded and render once it clears.

`Settings` and `View` are `FieldOverrides<T>` (`Veng/Reflection/FieldOverrides.h`): one value and one
set bit per reflected field, applied by copying exactly the set fields. That is why
`SceneRendererSettings` and `ViewState` are reflected (the latter over its knobs only) — the set, and
the engine's `UI::RenderSettingsEditor` that drives it, enumerate the fields rather than listing
them, so a field added to either struct is overridable and editable with no other change. The GPU
loop bounds (`DofMaxCoc`, `DofRingCount`) are clamped again after the overrides. The
layer is runtime-only — never saved, never replicated — and lasts until cleared or until the viewport
is destroyed. `tests/unit/viewport_debug_overrides.cpp` pins the set's semantics device-free;
`tests/gpu/render_look_resolve.cpp` pins that an override holds against a resolver through a look
change and a re-resolve, wins over a managed push, and returns the resolved value when cleared.

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

**A presented frame is never read back off the swap chain.** The finished composite exists in the
swap chain image, and `vkQueuePresentKHR` hands that image to the presentation engine: it is not the
application's again until it is re-acquired, so transitioning it for a readback afterwards is a
**write-after-present hazard** (and, on MoltenVK, a readback that can stall on the held drawable).
The swap chain images therefore carry colour-attachment usage alone. A consumer that wants the
finished frame asks for the composite to be run again into an image it owns, below.

**A `CaptureSink` composites the frame again, into an image somebody else owns.** A consumer that
wants the *frame* — at its own encoding, with or without the app's overlay, into memory a platform
encoder reads or a picture is read back from — installs itself with
`ViewportCompositor::AddCaptureSink(CaptureSink&)` (`Veng/Renderer/CaptureSink.h`;
`RemoveCaptureSink` uninstalls it). Each `Composite`, after the presented composite, every installed
sink is asked for a `CaptureTarget` (`{ Ref<Image>, DisplayColorSpace, IncludeOverlay }`) for this
frame's in-flight slot, and a supplied one is written by a further `SwapChainCompositePass` **that
sink owns** — the same scene source as the presented composite, the target's format and colour
space, the overlay bound only when the target asks for it. So a file's encoding is a setting rather
than a consequence of the display, the presented frame is untouched, and several sinks coexist — the
video recorder and a one-off `Capture::FrameCapture` screenshot each keep their own pass rather than
rebuilding one another's every frame (see [../Capture/CLAUDE.md](../Capture/CLAUDE.md)).

Four properties of the seam:

- **The target is the sink's, and the sink is asked every frame.** Returning nullopt skips the
  capture composite for that frame, which is how a sink idles. The compositor caches one `ImageView`
  per target image, so a recycled pool of targets is viewed once each rather than per frame.
- **Extent is a contract, not a resample.** The sink is handed the presented extent; a target of any
  other extent is a fatal assert and the composite is skipped rather than stretched into.
- **A sink's pass is rebuilt only when its shape moves** — its overlay choice changing, or a
  swap-chain invalidation (which re-sources the presented composite the same way). A format or colour-space
  change is a `SetSwapChainTarget` on the existing pass, and every change of the presented
  composite's scene or overlay source is applied to it too, so it never samples a source the
  display has moved off.
- **Readability rides the frame fence.** The capture composite is recorded into the frame's command
  buffer, so its target is readable when that slot's fence has been waited —
  `GetMaxFramesInFlight()` frames later, the contract `AsyncReadback` already rides. While any sink
  is installed the compositor registers one `Context::AddFrameRetiredCallback` and forwards it as
  `CaptureSink::OnSlotRetired` to every installed sink; a sink may add or remove sinks — itself
  included — from that call, and one removed mid-dispatch hears nothing more. The engine never waits
  a fence on a sink's behalf.

**The composite into a sink cannot run in the test band.** Every context in the `gpu` tier is
headless and the compositor's tail exists only windowed, so what the band proves is the import
(`tests/gpu/external_texture_import.mm`), the retirement contract (`tests/gpu/frame_retired.cpp`) and
its dispatch to several sinks (`tests/gpu/frame_capture.cpp`); that a capture composite writes the
right pixels is a live look.

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
apply. Each `Render` reads `Context::GetLastGpuFrameTimeMs()`, less the generated-texture pump's
scope (`DynamicResolutionFrameTimeMs` — background work is not frame load), and steps
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
[../Gui/CLAUDE.md](../Gui/CLAUDE.md)) is the authoring front end, and **the viewport that presents
its scene is what drives it**, exactly as a `GuiSurface` is driven by the viewport that renders it: a
mirror authored in a level works in any viewport showing that level, an editor's scene viewport
included, with no simulation host involved.

**The probe binds into a per-entity clone of the sibling material, not the shared mesh instance.**
The drive writes its output — the octahedral map handle and the `ProbeCenter` validity flag a
consumer gates the reflection on — into `materials[0]` of the sibling `MeshRenderer`. That instance
belongs to the **mesh asset**, shared by every entity drawing that mesh, so writing it directly
would make every sharer sample this probe (the classic case: two ships of one model, only the
player's cockpit should reflect). So the component's scene drive (`CaptureSurface::Drive` taking the
scene mutable — the `RenderSurfaces` precedent, which hands drivers the presented scene mutable the
same way), on a captured entity's first drive, **clones the sibling `materials[0]`
(`MaterialInstance::Clone`) and installs it as that entity's `MeshRenderer::InstanceMaterials`
override** — the per-entity material seam the render
gather honours (`GatherMeshes` → `VisibleMesh::Materials`, see [../Scene/CLAUDE.md](../Scene/CLAUDE.md)
and [../Asset/CLAUDE.md](../Asset/CLAUDE.md)). The probe then writes only the clone: the capturing
entity draws the reflective copy and every other sharer keeps the untouched asset instance, sampling
nothing (its `ProbeCenter.w` stays 0 → the material's no-probe path). The install is one mutable
component write per capturing entity — it bumps the scene's spatial version once, which the
broadphase re-gathers on — so a consumer authors no second mesh or material to keep an NPC's copy
of a reflective surface flat.

**The clone follows the material it was cloned from, and goes with the capture.** The owner of a
captured mesh keeps writing the mesh's own material — it never learns a clone exists — so each drive
copies that material's parameters into the clone whenever its revision has moved
(`MaterialInstance::CopyParamsFrom`), before binding the capture's slots over them: a parameter the
owner writes reaches the draw the frame after, and the capture's own bindings survive the copy. When
the capture goes — the component removed (its `VE_ON_REMOVE` hook) or the surface disabled (the
compositor's drive) — `CaptureSurface::RestoreMaterial` puts the source back in the override's slot,
dropping an override left identical to the mesh's own materials, so the entity draws what its owner
writes again. Neither runs when another system has since replaced the clone in that slot.

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

**A capture is driven by the viewport that will render its scene, and by nothing else.** A capture
feeds a material sampled by a mesh drawn in some view, so a scene no view shows has nowhere its
capture could be seen — and worlds are flat peers of which several are live at once in the ordinary
case, so driving every live scene's captures would multiply the per-frame view budget by the number
of worlds held warm. So `ViewportCompositor::RenderRegistered` opens with a **capture pre-pass**: for
each registered viewport, in registration order, that **will render this frame**
(`Viewport::WillRender` — enabled, holding a scene, and for an on-demand viewport pushed a fresh view,
which is exactly `Render`'s early-outs), it drives the `CaptureSurface`s of the scene it presents.
**Each scene is driven once per frame, by its first such viewport** (`ClaimCaptureScenes`,
`CaptureDrive.h`, the device-free rule), placed at that viewport's interpolation alpha — the pose that
viewport draws the carrier at. A viewport the compositor will not render — disabled, or an on-demand
one whose owner stopped pushing (a hidden editor tab) — drives nothing. **A present-on-ready rebind's
destination is warmed too**: while a managed viewport waits on a destination, `ManagedViewportSet`
hands it the destination scene (`Viewport::SetPendingScene`) and the pre-pass drives that scene's
captures as well, so a make-before-break swap presents a probe already rendered. **A gap re-arms**:
the runtime records the `Context` frame serial it was last driven at, and a drive that finds a frame
passed since (`CaptureDriveSkippedFrame`) restarts the refresh first, as `MarkDirty` does — so an
on-demand capture whose scene went unrendered rebuilds its map rather than resuming from what it saw
before it went dark.

**A capture is built at most one per frame, and a released one is reused.** A `SceneCapture` owns a
whole face renderer, and a world presented for the first time can arrive with several capture
surfaces at once — so the pre-pass builds at most `ViewportCompositor::MaxNewCapturesPerFrame` (one)
new capture per frame, leaving a surface past the budget unmaterialized and undriven until a later
frame. A capture whose surface is destroyed — its entity, its component, or its whole scene going —
goes to the compositor's `SceneCapturePool` (`ViewportCompositor::GetCapturePool`) instead of being freed:
detached from the drive-list and reset (`SceneCapture::ResetForReuse` — the pushed view, the
round-robin, the first-render atlas clears, and the scene its face renderer last gathered, through
`SceneRenderer::ReleaseScene`), it is handed to the next surface asking for an identical
configuration (`SceneCapture::IsConfiguredFor`: face resolution, renderer settings, distance and cube
paths) before any build is considered, and that reuse costs no budget. The pool holds a bounded
number (`SceneCapturePool::DefaultCapacity`) and drops the longest-held past it. A surface driven
directly through `CaptureSurface::Drive`, outside the pre-pass, still builds its own on first use
and frees it when it goes. `ViewportCompositor::GetCaptureSurfaceDrive` reports what the last
pre-pass drove, built, reused, deferred and released.

**A surface can be switched off without losing its settings.** `CaptureSurface::Enabled` false makes
the pre-pass release the surface's runtime — the capture to the pool, the material slots it bound
cleared — and build nothing for it while it stays off (`CaptureSurfaceDriveResult::SurfacesDisabled`
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

**A capture can light its scene instead of feeding a material.** `CaptureSurface::Output =
SceneLighting` builds the capture with its radiance cube on (which `IsConfiguredFor` already tells
apart, so pooling needs nothing more), binds no material — every slot field is ignored and the clone
is never installed — and renders world-aligned, an `Entity` alignment warning once, because
image-based lighting is sampled by world direction. It is driven, budgeted, pooled, re-armed and
released exactly as a material capture is, and excludes its own entity the same way; the renderer
resolves it as the scene's IBL input (see [IBL and the sky](#ibl-and-the-sky)).

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

**A capture's batteries are lean, and shadows and particles are the ones a capture asks back.**
Bloom, AO and TAA are dropped unconditionally — the capture samples pre-tonemap HDR, so the post chain
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
**Particles are dropped by default too** — the face renderer gets no particle capacity, so it holds
no pool and runs no spawn, update, sort or draw — since a pool simulated per capture costs as much as
the camera view's while its particles are lost under the map's resolution. `CaptureSurface::Particles`
(default off, read at materialization) asks them back for a capture that must show an effect.

**A capture also draws a subset of the render layers, and defaults to the environment set.** Beyond
the one nominated entity, a capture filters by `RenderLayer` (`Veng/Scene/RenderLayer.h`): every
drawable sits on a layer (`MeshRenderer::Layer`), and `CaptureView::VisibleLayers` names the layers
the faces draw. `CaptureSurface::VisibleLayers` defaults to `DefaultEnvironmentCaptureLayers` — every
layer but `RenderLayer::ViewAnchored`, `RenderLayer::Display` and `RenderLayer::CameraOnly` — because a probe records the
environment around its position, and neither camera-anchored decoration (a near-field particle
shell, a billboard slaved to the eye) nor content presented to a viewer (a world-space readout, a
holographic instrument) is part of it: drawn into the map, it would appear in every reflection or
lens sampling the capture, floating at a distance it was never at or reflected as though it were
scenery. **Ribbons, trails and ribbon paths honour the same filter and the same exclusion**: each
carries a `Layer`, and `GatherRibbons` takes the view's mask and excluded entity and gathers neither
an off-mask one nor the entity a capture feeds, exactly as `GatherMeshes` does. **A sphere terrain
honours the filter too** (`SphereTerrain::Layer`): a face whose mask omits its layer neither draws it
nor selects, marks or requests its tiles, so an image-based-lighting probe drawing
`RenderLayer::Environment` alone leaves a `Default` terrain out and puts no load on its shared source
(see [Sphere terrain](#sphere-terrain-a-quadtree-of-generated-tiles)). The filter is the same closed producer→consumer machinery the
entity exclusion rides — `SceneView::VisibleLayers` carried into `SceneBroadphase::Sync`, applied by
`GatherMeshes` beside the `Visible` test, a changed mask its own rebuild trigger.

**Two layers split the camera views from the captures.** A camera view draws
`DefaultCameraViewLayers` (`SceneView::VisibleLayers`' default) and a bare `SceneCapture`
`DefaultSceneCaptureLayers` (`CaptureView::VisibleLayers`' default); the two differ only in
`RenderLayer::CameraOnly`, which the camera views draw and every default capture mask leaves out, and
`RenderLayer::CaptureOnly`, which every default capture mask draws and the camera views leave out.
`CameraOnly` is for geometry too costly or too view-dependent to draw again from a capture's
viewpoint — a streamed sphere terrain, whose capture would select and request tiles from its own eye
against the pool the camera view is filling. `CaptureOnly` is for the stand-in that
replaces it in reflections and image-based lighting: since no camera view draws it, it need not hide
under the geometry it replaces, and can stand exactly where that geometry is. Nothing sits on either
layer until a consumer puts it there, so a scene using neither renders and captures as it would
without them. A capture that names its own mask (an IBL probe drawing `RenderLayer::Environment`
alone) adds `CaptureOnly` when it relies on a stand-in.

**A capture carries its own near and far planes.** `CaptureView::Near`/`Far` (0.05 / 10,000 world
units by default) set every face camera's projection, and `CaptureSurface::Near`, `Far` and
`InfiniteFar` author them, forwarded with every view `Drive` pushes, so a change reaches the next face.
`InfiniteFar` is the flag `Camera` carries beside `Far` (see [An infinite far plane](#an-infinite-far-plane)):
the faces project in the infinite reverse-Z form, so device depth is `near / viewDepth` and the
cleared 0 stays the sky. The defaults suit a scene measured in metres and a few kilometres across;
geometry past the far plane is absent from the map and its direction reads as whatever lies behind it,
so a capture of an environment reaching further — terrain to a horizon, a sky shell overhead, distant
bodies — raises the far or sets `InfiniteFar`. The near must stay below the nearest surface the capture
is meant to record. The distance map linearizes its depth through `near / far`
(`capture_octahedral_distance.frag`), which is zero under an infinite far and carries no cancellation
at a large finite one.

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

## Model portraits: a model rendered offscreen for UI

`ModelPortrait` (`Veng/Renderer/ModelPortrait.h`) is the component a UI uses to show a model — a
portrait of a selected object, an equipment preview — without hand-rolling a private scene, a
renderer and a release path. Authoring and the document side are in
[../Gui/CLAUDE.md](../Gui/CLAUDE.md), "The engine-driven scene component family"; the render side is
the capture pre-pass's, extended:

- **Driven where captures are.** `ViewportCompositor::DriveCaptureSurfaces` walks each claimed
  scene's portraits after its captures (`DriveScenePortraits`): a disabled portrait is released, a
  renderer no longer matching `Extent`/`Output` is handed back, one is materialized from the
  compositor's `ModelPortraitPool` or built — sharing the capture pass's one-new-build-per-frame
  budget (`MaxNewCapturesPerFrame`) — and `Prepare` brings the private scene current (instantiate,
  populate, pose, light, frame) and says whether this frame renders (`EveryFrame`, or an `OnDemand`
  portrait that owes one — after `MarkDirty`, an instantiation, a pose change, or a frame its scene
  went unrendered, the capture re-arm rule). A `Retain` portrait holding its kept image is skipped
  before any of that (`PortraitWantsRenderer`). The tally is `GetModelPortraitDrive`.
- **Rendered first, inside the view budget.** `RenderRegistered` renders the queued portraits
  before the captures and the viewports (`DrivePortraits`), each claiming its view slot only while
  one is left beyond the viewports' reserved ones; a portrait the budget cannot seat keeps its last
  render and still owes one. Rendering ahead of the viewports is what lets a Gui driver, running in
  the viewport's drive later in the same command buffer, paint this frame's render.
- **A lean renderer, pooled.** `PortraitRenderer` (`src/Renderer/PortraitRenderer.h`) owns a
  `SceneRenderer` at the portrait's extent with every view battery off (no bloom, shadows, AO,
  post effects or anti-aliasing) — `RenderPath::GeometryDepthNormal` for a depth-normal portrait.
  The pool holds released renderers by configuration (`PortraitRendererConfig`: extent and output),
  so a UI opening and closing a portrait builds once; a returned renderer drops the scene it last
  gathered (`SceneRenderer::ReleaseScene`) before the private scene it drew is destroyed.
- **Coverage counts every layer, and the output is premultiplied.** The deferred lighting pass
  writes the scene's coverage into the HDR target's alpha — 1 on geometry, 0 on the cleared
  background — and every translucent draw accumulates into it by its blend: an alpha draw (the
  `AlphaBlend` and `PremultipliedAlpha` states) by `over`, an additive one (`AlphaAdditive`, `One,
  One` keeping destination alpha) not at all. Nothing else in a frame reads that alpha. The tonemap
  leaves alpha opaque, so a shaded portrait runs one more fullscreen pass (`portrait_coverage.frag`)
  into its own colour target: the tonemapped colour and the HDR target's alpha. The colour is the
  subject tonemapped over black, so it already carries its coverage: the output is premultiplied
  (`ModelPortraitOutput::Premultiplied`), and composited `over` a background it reproduces the
  subject exactly as it rendered over black — a halo outside the solid shape covers partly, and an
  additive glow adds light without covering. A Gui `Image` paints it with
  `ImageAlphaMode::Premultiplied`.
- **Static and silent.** The private scene has no systems and no presentation scope; it is posed by
  `ModelPose` and by what the populate callback attaches, and lit by one key light and an ambient
  floor (`SceneView::AmbientFloor`). Its clip planes are fitted to everything in it each frame.
- **A look of its own.** `Render` reads a `RenderLook` in the private scene — one a populate callback
  attaches — for the per-frame knobs a lean renderer honours: `Exposure`, `Tonemapper`,
  `AmbientFloor` (in place of `PortraitLighting::Ambient`) and `EnvironmentLightTint`. The renderer's
  batteries stay off whatever the look says, since they are its topology and it is pooled by extent
  and output alone. With no look a portrait renders at exposure 1 through the default curve.
- **Model-less.** A portrait whose `Model` is empty and whose populate callback is set builds its
  private scene from the callback alone, run on an empty root posed by `ModelPose`; `AutoFit` frames
  everything the callback attached (`ModelBounds` is empty), and each build counts as an
  instantiation. With neither, it renders nothing.
- **`Retain`: an image without a renderer.** `CaptureRefresh::Retain` renders as `OnDemand`; on the
  frame its render lands, `Render` copies the Shaded colour into an image the portrait owns
  (`RetainOutput`), returns the renderer to the pool and drops the private scene. `GetOutput` then
  reads `Ready` with that image and the shared portrait sampler, and the drive skips the portrait —
  no renderer, no `Prepare`, no view slot — until `MarkDirty` (which takes a renderer, rebuilds the
  scene and renders into the same image, the kept one showing until then), a change of `Extent`,
  `Output` or `Refresh`, or a release. **The cost model:** a still portrait costs one image of its
  extent in the output format and one bindless slot; the pool holds at most its capacity of
  renderers, so a gallery of N stills built one or two a frame costs N images and a handful of
  renderers, where `OnDemand` costs N renderers (each a full lean `SceneRenderer`). The price is a
  re-render on every change, which instantiates the model afresh. A `GeometryDepthNormal` portrait
  treats `Retain` as `OnDemand`.
- **An explicit camera can be tied to the instance it was framed for.** A caller framing by the
  model it measured runs before the drive, which instantiates a re-pointed `Model` and renders it in
  the same `Prepare` — so that camera fits the previous instance. Stamping
  `PortraitFraming::FramedInstantiation` with the `GetInstantiationCount()` it measured makes the
  portrait hold its last render (pose, bounds and camera included) while the instance differs, and
  render once the caller frames the new one. Unset, any instance renders.
- **The level of detail its size on screen calls for.** A portrait frames its model to fill its
  image, so selecting from its own camera alone would always pick full detail however small the
  panel showing it. `ModelPortrait::LodScreenScale` — the fraction of the screen's height the image
  is shown at — rides `SceneView::LodScreenScale` (see "Level of detail") and scales every projected
  size, so a portrait draws the level the model would in a scene at that size; the caller supplies
  it, since only the UI knows where the image lands. `ModelPortrait::ForcedLod` (default
  `AutomaticLod`) rides `SceneView::ForcedLod`, so neither needs a `Configure` of the pooled
  renderer: `0` draws full detail and a level between picks that level. An entity of the model
  carrying a `LodOverride` wins, and a single-level mesh is unaffected.

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
  makes progress rather than stalling; at the default cost of 1 the budget is a tick count. How big
  the budget is each frame is the adaptive policy's (below) unless `SetCostBudget` fixes it. Raising
  a queued job's priority preempts a running one at the *next tick*,
  not the next job. Requests are **idempotent on the key**, so re-requesting every frame while the
  result is still wanted is the intended usage; `Cancel` tears an unfinished job down and simply
  releases its targets.

### The budget follows the frame's GPU headroom

A fixed cost per frame bakes at a rate tied to the frame rate rather than to the GPU's idle time: an
application capped at 30 fps bakes at half the wall-clock rate of 60 fps while its GPU idles most of
each frame, and one at 120 fps bakes twice as fast in frames that may have no idle time at all. So
the pump sizes its budget each frame from what the frame actually leaves (`GeneratedTextureBudgetController`,
`src/Renderer/GeneratedTextureBudget.h`, device-free):

- **The period** is the run loop's frame-cap period when one is set, else an exponentially smoothed
  measured frame delta (sleep included, a hitch clamped at 100 ms), which covers uncapped vsync and
  VRR. It reaches the service as a `GeneratedTextureFrameHint` that `Application::Frame` sets every
  frame just before `Context::BeginFrame` (`SetFrameHint`) — the delta, the limiter's cap, whether the
  clock is driven, and the view reserve below. `Context` itself knows neither the cap nor the delta.
- **The headroom** is `period · (1 − FrameMargin)` minus the last measured GPU frame time *excluding
  the pump's own `Generated Textures` scope* — so the bake's time never reads as load it must back
  off from, which would make it oscillate. The target is `clamp(headroom · IdleShare, 0,
  MaxMilliseconds)`.
- **Milliseconds become cost** through a learned `msPerCost`: a running average of the pump scope's
  measured time over the cost that pump spent. A frame's timings are read back when its
  frame-in-flight slot comes round again (two frames late), so the service keeps the cost each slot's
  pump spent and pairs the timings with that slot's figure, never with the latest pump's.
- **Damping**: the budget falls to its target at once — a frame that lost its headroom must not keep
  paying for it — and rises an eighth of the gap a frame, because the measurement justifying a rise
  is already two frames old.
- **Bounds**: never below `DefaultCostBudget` (4), so the policy never bakes less than the fixed
  schedule did; boosted only while a job has a selectable tick or a staged restore (an idle service
  drops to the floor, so no boost carries into the next job); at most `MaxTicksPerFrame` ticks a pump.

**Why the share is below one.** Idle time is not free: a fanless machine whose GPU runs flat out
every frame throttles, and the frame-to-frame variance of the rest of the frame needs somewhere to
land. The defaults (`GeneratedTextureBudgetSettings`) are `IdleShare` 0.5, `FrameMargin` 0.1,
`MaxMilliseconds` 8 and `MaxTicksPerFrame` 64. The policy does not model the CPU cost of recording a
tick; a job whose recording is heavy declares a higher `Cost`.

**Fallbacks.** A device without GPU timing scales the floor by `period / (1 / FallbackReferenceHz)`
(60 Hz by default), clamped to `[1, 4]` and carrying the fraction a frame cannot spend into the next,
so a 30 Hz frame spends twice the 60 Hz budget and a 120 Hz frame keeps the floor. A driven frame
clock, a context handed no hint (the gpu fixtures, any consumer driving `Context` without a run loop),
and `Enabled = false` spend exactly `DefaultCostBudget`, so captures and tests stay deterministic.
`SetCostBudget` fixes the budget outright until `ClearCostBudget`. `GeneratedTextureStats` reports the
resolved budget, the cost spent and the learned `msPerCost`; the pump samples the
`GeneratedTextures/Budget` and `GeneratedTextures/Cost` counters.

**The frame's own views come first.** A tick that renders a view — a sky-bake face tile claims a
view slot (`BindlessRegistry::TryBeginView`) — declares it through `GeneratedTextureRequest::ViewSlots`,
and the pump runs it only while the registry has that many slots left beyond the hint's
`ViewReserve`. A tick that would cut into it ends the frame's spending (the queue's `Admit` refusal
stops rather than skipping, so the job keeps its place) and runs at a later pump, never an assert.
The pump runs before the frame's portraits, captures and viewports, so `Application` computes the
reserve from the compositor (`ViewportCompositor::GetBackgroundViewReserve`): every registered
viewport's `GetViewSlotsPerExecute` in full, plus one per registered capture and per portrait driven
last frame — that share capped at half of what the viewports leave (`BackgroundViewReserve`,
`CaptureRotation.h`), since captures degrade gracefully too and a large capture set must not starve
background work outright.

**A large budget does not crowd out the frame's timings.** The pump opens one GPU scope for itself
and one per tick for only the first `MaxTickScopesPerPump` (16) ticks; later ticks record under the
pump's scope alone, so a many-tick frame cannot spend the `MaxGpuScopes` (128) the passes after it
need. **Dynamic resolution ignores the pump**: `Viewport::UpdateDynamicResolution` hands the
controller `DynamicResolutionFrameTimeMs(GetLastGpuFrameTimeMs(), GetLastPumpGpuMs())`, so
background generation filling idle time never drives the render scale down.

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

### A target can be a layer range of an array

`GeneratedTextureTargetInfo::AdoptBaseLayer` and `AdoptLayerCount` make an adopted target **one
layer range of the adopted image** rather than all of it. `GetSampledView()` is then a view over the
range, so every transition the service records — the producer access before a tick, the
write-after-write between ticks, `SampleAny` on completion, the transfer access of a cache store or
restore — reaches those layers and no others (layouts are tracked per subresource, and a view's
`PrepareForAccess` touches only its range). `GetView(mip, layer)` counts `layer` from the range's
base (`GetBaseLayer`, `GetLayerCount`), a store reads and a restore writes the range alone through
`BufferImageCopyRegion::BaseArrayLayer`/`ArrayLayers`, and the stored shape is the range's — so a
cached range restores into the same range of a fresh array. It serves **many jobs filling disjoint
layers of one array that the frame samples throughout**: a tile pool, a shadow-map array, a decal
atlas. Adopting the whole array for one job would put every other layer into the producer's layout
mid-frame. `Bindless` stays single-layer 2D only, and a layer-range target asking for it is a fatal
assert. `tests/gpu/generated_texture.cpp` pins both halves: the other layers' tracked layouts and
texels survive a job on one, and a cached one restores into the same layer of an untouched array.

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

## Sphere terrain: a quadtree of generated tiles

**A sphere's terrain is a cube-sphere quadtree of tiles**, chosen per view by screen-space error,
generated on the GPU by kernels a consumer supplies, held in a pooled tile atlas, and drawn as one
instanced grid that morphs between levels — from a whole-globe view down to centimetre tiles on a
sphere thousands of kilometres across. The first half below is the data (the mapping, the pool, the
measured bounds and the selection); the second the drawing.

- **The mapping** (`Veng/Math/CubeSphere.h`, device-free, double). Six faces `+X −X +Y −Y +Z −Z`,
  each `u, v ∈ [−1, 1]` with a right-handed basis, warped equal-angle (`x = tan(π/4·u)`, area ratio
  ~1.4 across a face). Tile edges are lines on the cube plane, so great-circle arcs: a tile is a
  spherical quadrilateral. `CubeSphereTileKey` packs face, level (≤ 24) and `x, y` into a `u64`;
  `GetParent` / `GetChild` / `GetNeighbour` are pure, and a neighbour across a face edge is found by
  folding doubled integer coordinates over the edge, so it is exact. `GetCubeSphereTileGeometry`
  gives a tile's corners, norms and warp terms in double.
- **Precision at depth.** The face fraction across a tile is the tangent difference rewritten as a
  quotient of sines and cosines (`TileFraction`), which never subtracts nearly equal tangents; the
  f32 form the shaders run (`TileFractionF32`, `Veng/sphere_terrain_mapping.slang`) divides through
  by `cos θ0` and evaluates fixed sine and cosine polynomials, so it does not depend on the device's
  transcendentals or its fast-math mode. A texel's direction is the tile's centre (double, handed to
  the GPU as an f32 value plus residual) plus an offset built only from small differences
  `d_i − d_c` (`CubeSphereTileOffsetF32` / `SphereTerrainTileOffset`), accurate to the f32 rounding
  of the tile's own size: on a 6.4×10⁶ m sphere the device places a level-20–24 texel within about
  a micrometre (`tests/gpu/sphere_terrain.cpp`). MoltenVK's fast-math mode is requested explicitly
  (`MVK_CONFIG_FAST_MATH_ENABLED`, "always", through `VK_EXT_layer_settings`; the environment wins)
  rather than inherited. Arithmetic that must hold its rounding is marked `precise`, but Slang's
  default floating-point mode emits no `NoContraction` for it, so under fast math the device may
  still contract or reassociate it; the micrometre bound is what the gpu test holds.
- **The generator** (`SphereTerrainGenerator`, `Veng/Renderer/SphereTerrain.h`) is where a
  consumer's kernels enter: `GetChannels` (≤ `MaxSphereTerrainChannels`; channel 0 is height,
  `R32Sfloat`, metres above the radius; a channel may hold a divisor of the tile's resolution),
  `RecordTile`, `GetTileCost`, `GetCacheSalt` and `GetLevelHeightBound`. **Its contract: a texel's
  value is a function of its direction and level alone**, so same-level tiles agree on shared and
  border texels to their anchors' rounding. A tile is `SphereTerrainTileTexels` (133) a side: 128
  cells plus the shared edge row plus a 2-texel border; grid vertex `k` sits on texel `2 + 4k`.
  `SphereTerrainTileWork` carries the key and layer, the geometry in double, the packed
  `SphereTerrainTileConstants` (128 bytes, a push block), a storage view per channel over the tile's
  layer, and the parent's layer as single-layer sampled views — never the whole arrays, whose
  descriptor would name this tile's layer in the wrong layout while it is being written.
- **The pool** (`SphereTerrainSource`, created by the consumer and shared through the
  `SphereTerrain` component). One array image per channel, `MaxResidentTiles` layers (clamped to
  `Context::GetMaxImageArrayLayers` with a warning), one layer index per tile across channels, every
  layer held in a sampled layout outside its own tick, so `GetChannelArrayView` is bindable by any
  pass after the frame's generation; set 0 is never touched. Each tile is one generated-texture job
  whose targets adopt that tile's layer of every channel (a layer-range target, above), keyed
  `mix64(source serial, tile key)`, cached under `GetCacheSalt` when it is non-empty; the job is
  released once complete, since the texels live in the pool. `SetCapacity` reallocates in place,
  copying (and compacting) the resident layers through `CommandBuffer::CopyImageLayers` as setup
  commands.
- **Residency** (`SphereTerrainResidency`, device-free, `src/Renderer/`). A tile is requested only
  under a resident parent, so the held set is closed under parent; a tile is evictable when not
  pinned, holding no pending or resident child, and wanted neither this frame nor the last (two
  frames, so renderers marking in turn cannot evict each other's tiles); eviction takes the least
  recently wanted, ties on the least key, and a request wants its parent first, so never evicts it.
  Neither costs a scan: the pending tiles are counted as they come and go, and every resident tile
  neither pinned nor a parent sits in a bucket of the frame it was last wanted, ordered by key, so an
  eviction pops the oldest bucket's least key (`tests/unit/sphere_terrain_residency.cpp`). When the
  core takes a tile in it computes the tile's **facts** once (`SphereTerrainTileFacts`: geometry,
  the box's height-free frame, packed constants, arc terms, chords, tangent, edge, and the
  generator's `WriteTileDrawData` words), which the selection, the cull, the records and the tile's
  own generation then read rather than recompute. Full with nothing evictable, a request waits and is counted
  (`RequestsDeferred`); so does one past `MaxPendingTiles`. A pending tile no renderer wants for two
  frames is cancelled. The pinned top (`0..PinnedLevel`, 126 tiles at 2) is requested level by
  level from creation, at `PinnedPriority`, and never evicted, so every parent chain ends resident.
  The source advances on `Context::AddFrameRetiredCallback`, ahead of the frame's pump: it releases
  completed jobs, reads back retired bounds, cancels stale requests and requests the pinned top.
- **Measured bounds.** Each tick runs the generator, then the engine's reduction
  (`sphere_terrain_bounds.comp`, one workgroup) ordered after it by a `StorageRead` barrier on the
  height layer: the interior's min and max, and `MorphError` — the largest difference over the
  33×33 grid vertices between the tile's height and its parent's grid (linear along a parent edge,
  the larger of both diagonals at a cell centre). Each result is one 256-byte entry of a host-mapped
  per-frame-slot buffer, bound alone so dispatches never share a range, and read when the slot's
  fence has been waited. A tile restored from the cache ran no tick, so it is measured by a job of
  its own. Until measured, a tile is bounded by its parent's range widened by
  `GetLevelHeightBound`; its working bound is the union of its own and its parent's.
- **Selection** (`SelectSphereTerrain`, `src/Renderer/SphereTerrainSelection.h`, device-free and
  pure). A level's range is `D_L = max(s_L·K/τ, c·Ď_L)`, both terms upper bounds that halve exactly
  per level, so `D_{L−1} = 2 D_L`. Distances are to the tile's bound, an oriented box in the tile's
  own frame (so its diameter is the tile's, whatever face it is on). A drawn tile splits under
  `(1 − h)·D_L` and merges past `(1 + h)·D_L`, by distance alone, so a rough sphere draws the same
  levels as a smooth one; a split needs all four children resident, else the tile
  is drawn and its children requested (the parent fallback). Subtrees are pruned below the horizon
  (all eight bound corners hidden by the sphere of `Radius + MinHeight`), past their range, and —
  **beyond `OmnidirectionalRadius`** (64 m, raised to the renderer's shadow distance) — when their
  bound misses the view frustum's side planes, so turning the head never strips the ground near the
  eye while the far field behind the view stays at its parents' levels. **Splits are taken nearest
  first under a tile budget**: every split wanted joins a queue ordered by the distance to its bound
  (a split taken last frame ranked at `(1 − h)` of it, a new one at `(1 + h)`), and each split
  charges its four children to `TileBudget` (the pool's capacity less the pinned top and a reserve
  of `max(16, capacity/16)`), resident or still to generate; a split the budget cannot afford is
  held whole. So when the pool cannot hold every wanted split, the ground under and before the eye
  keeps its depth and the shortfall falls on the far field. **The drawn set is 2:1 balanced by
  construction**: a tile splits only once every same-level neighbour exists, so a missing
  neighbour's parent is split first (past the horizon or outside the frustum, where nothing else
  wants it), and a neighbour whose children are not resident has them requested and holds the split
  until they land. A tile that wanted to split and was held — by residency, budget, frustum or
  balance — is drawn with `SphereTerrainDrawnTile::Held` set. Morph ranges are
  `E_L = 2(1 − h)·D_L` and `S_L = a·E_L`; with `h = 0.05`, `a = 0.85`, `c = 2`, every coarse–fine
  edge in view whose coarse side was not held has the coarse side unmorphed and the fine side fully
  morphed (`tests/unit/sphere_terrain_selection.cpp`); against a held coarse side the fine side is
  partly morphed, and its edge stands off by at most its MorphError, which the skirts cover.
  **The floor protects that invariant and nothing else.** A coarse edge vertex lies in the fine
  side's parent, which split nearer than `(1 + h)·D_L`, so it is within `(1 + h)·D_L + Ď_L` of the
  eye, and stays unmorphed while that is under `S_L = 2a(1 − h)·D_L`: `c ≥ 1/(2a(1 − h) − (1 + h))`
  = 1.77, and `c = 2` leaves an eighth of a diameter for the bound's height. (The fine side's
  edge is fully morphed whatever `c` is, since the coarse box is past `(1 − h)·D_L = E_{L+1}`, and
  balance is by construction.) The floor governs only below `K = 2√2·32·τ` (724 px/rad at
  `τ = 8`); at any sharper view `TargetPixelsPerQuad` alone sets how far each level reaches, and an
  unheld tile's quads span about `τ` pixels at its nearest distance and about half that at twice it.
- **A view selects only the terrains on its layers.** A terrain sits on a render layer
  (`SphereTerrain::Layer`, `RenderLayer::Default` unless set), and a renderer selects it only when the
  view's mask names that layer (`SceneView::VisibleLayers`, carried in `SphereTerrainSelectInfo`). A
  view leaving the layer out draws no tile, marks none wanted and requests none — so a scene capture,
  whose own renderer otherwise selects against the shared source from its own eye one face a frame,
  costs the source nothing when its mask omits the terrain, as an environment probe drawing only
  `RenderLayer::Environment` does (`tests/gpu/sphere_terrain.cpp`). A terrain on
  `RenderLayer::CameraOnly` is left out of every capture at its default mask, and drawn by every
  camera view.
- **Per renderer** (`SphereTerrainSystem`). Each `Execute` forms the eye in each sphere's body-fixed
  frame in double — `inverse(Orientation)·(FloatingOrigin::Position + camera − Center)` — selects,
  marks the wanted set (visited tiles, children of waiting splits, prefetched children within
  `PrefetchRangeScale` of the split distance) and issues the requests: splits in view first, then
  splits behind, by how far the parent's error exceeds the threshold, then prefetch. Its memory (last
  frame's splits, for hysteresis, and the containers each frame clears and refills) is keyed by the
  source's serial and by whether the view is a cube face, so a source moved between entities or
  scenes keeps it. **A capture selects once per cube cycle**: a view rendering a cube face
  (`SceneView::CaptureFace`, `CaptureFaceIndex`) selects at face 0 with no frustum and every face
  draws that selection, culled to its own frustum and re-marking what it wants, so the six faces never
  compete for tiles and a capture's splits and morphs do not churn with the face it renders. `SceneRenderer::GetSphereTerrainSelection(entity)` returns the drawn
  tiles (key, layers, working range, morph range, MorphError, held); `GetSphereTerrainStats()` the
  counts per level, waiting splits, balance splits and holds; `SphereTerrainSource::FindDeepestResident(direction)` and
  `GetStats()` the pool's side. `Render/SphereTerrain*` counters sample them.

**What a view wants is large near the ground, and balance makes depth cost a constant per level.**
Around an eye 2 m above a 6.4×10⁶ m sphere, at `τ = 8` and a 1440-pixel view, every tile resident,
the unbudgeted selection wants about 7,500 tiles omnidirectionally and about 5,000 pruned to a
16:9 frustum; at 430 m, 5,300 against 1,900. A budget far below that still reaches the deepest level
under the eye, because the nearest splits come first, but the balanced chain down to it costs about
24 tiles a level, so a pool's depth near the ground is bounded by `TileBudget / 24` levels below the
pinned top: a 512-tile pool reaches level 16 under the eye at 2 m, and full depth (level 24) wants
a budget near 550, about a 700-tile pool.

### Drawing sphere terrain

**Every drawn tile is one instance of one shared grid** (`SphereTerrainSystem`, per renderer;
`SphereTerrainVertex.h` and `SphereTerrainDraw.h` the device-free cores the unit band pins).

- **The grid.** 33 × 33 vertices (32 quads a side, vertex `k` on texel `2 + 4k`) plus a skirt vertex
  under each edge vertex, carried by no vertex buffer: the index (u16, built once per renderer) names
  the vertex and the stage decodes it (`SphereTerrainGridVertex`). Every quad is cut from its lowest
  corner to its highest, the diagonal whose collapse under the morph reproduces the parent's
  triangulation; 2,048 triangles a tile plus 256 of skirt, wound counter-clockwise from outside.
- **Records and lists.** Each frame, each tile some view draws gets one 688-byte record
  (`SphereTerrainDrawRecord` / `SphereTerrainRecord`) in a host-mapped per-frame-slot buffer: its four
  corners eye-relative, `Center + Orientation·(R·d_i) − eye` formed in double; the corners' verticals;
  the six squared chords `|d_i − d_j|²`; the tile's `SphereTerrainTileConstants`; last frame's corners
  (from last frame's pose and origin, relative to this frame's camera), the tangent, the morph range,
  arrival floor and skirt depth, the level layers' heights, the layers and quadrant, the tile's arc
  terms (`CubeSphere::TileArcF32`, so a vertex evaluates no sine or cosine of the span), and the
  generator's `MaxSphereTerrainTileWords` words for the tile and its parent (from the pool's facts).
  `CullSphereTerrainFrame` decides the lists, then `WriteSphereTerrainRecords` writes each record
  straight into the frame slot's mapping; the set is written again only when the buffers or the
  pool's views (`SphereTerrainSource::GetViewGeneration`) it names have changed. Each
  view and surface has a list of record indices; a draw's `firstInstance` is its list's start, so
  `SV_VulkanInstanceID` names the entry. **Set 4** is the terrain's own layout
  (`CreateSphereTerrainSetLayout`): channel `n`'s array at binding `n` (an absent channel binds the
  height's), the records at 8, the lists at 9, written once per source per frame slot. A translucent
  level layer reads the same set at 6 (below); `VE_SPHERE_TERRAIN_SET` names it in the shaders.
- **Placement** (`SphereTerrainPlace`): `Σ ŵ_i C_i + n·(R (1 − |m|²)/(1 + |m|) + h)` with
  `1 − |m|² = Σ_{i<j} ŵ_iŵ_j|d_i − d_j|²` — every term an eye-relative corner or a quantity no larger
  than the tile, so nothing the size of the radius is subtracted in f32: the device places a level-22
  vertex 2 m under the eye on a 6.4×10⁶ m sphere to 0.3 µm, and a level-8 vertex from 10,000 km to
  1×10⁻⁷ of its distance (`tests/gpu/sphere_terrain.cpp`). The corner weights are computed once
  per placement and handed to the weight-taking `SphereTerrainPlace`, `SphereTerrainPlacePrevious`
  and `SphereTerrainTileOffset`; a parameter of exactly 0 or 1 takes a fraction of exactly 0 or 1,
  so neighbouring tiles' edge vertices coincide.
- **The morph** (CDLOD's, reimplemented from the published description): `k` from the vertex's
  unmorphed distance, `max(saturate((d − S)/(E − S)), floor)`; odd grid coordinates slide to the even
  one below by `k`, and the height blends from the tile's own grid to its parent's grid (linear over
  the parent's triangles) by `k`, so at `k = 1` a tile *is* its parent, vertex for vertex and
  triangle for triangle. A vertex whose `k` is 0 — most of every tile, coherently — reads no parent
  or odd height and is placed once, and `TerrainChannel` samples no parent where the fragment does
  not morph. A tile appearing under one drawn last frame takes an **arrival floor**
  falling from 1 to 0 over `SphereTerrainArrivalSeconds` (0.25 s) on the view's delta, so a split
  delayed past its distance morphs in rather than popping. A tile the selection replaces by a
  coarser one **departs**: it keeps drawing under a floor rising from 0 to 1 over
  `SphereTerrainDepartureSeconds` (0.25 s), wanted so the pool keeps it, and the coarser tile is
  withheld until every tile departing under it has been drawn once wholly in its shape — so a
  split the budget or the frustum takes back short of the morph end morphs out rather than
  stepping. A tile changing direction mid-way keeps its floor; a departure whose tile or parent
  leaves the pool ends at once. The morph runs one level, so a merge across two or more levels
  fades its finest level alone. `AdvanceSphereTerrainTransitions` (`SphereTerrainTransitions.h`,
  device-free, pinned by `tests/unit/sphere_terrain_transitions.cpp`) applies both to the
  selection; the drawn set it returns covers the sphere once, as the selection's does.
- **Skirts** hang each edge vertex along the vertical by the record's depth:
  `max(M_L, M_{L+1})` (the frame's largest MorphError at the tile's level and the next, the most a
  partly morphed neighbour's edge can stand off) plus the parent grid's chord sagitta, two f32 steps
  at the tile's distance and two tiles' rounding of one texel, and at least
  `SphereTerrainMinSkirtMetres`. A skirt takes its edge vertex's shading.
- **Culling** is per view, in unscaled metres, against the selection's box raised to the level
  layers the tile draws (`SphereTerrainCullHeights`; a layer below all its ground is not drawn on it
  and does not widen it): the main view by its four side planes (so neither the near and far planes nor a scale
  move it) and the horizon, each shadow view by its six cull planes (`ShadowCasterViews`). The
  cascades fit to the shadow-casting tiles within `MaxShadowDistance` alone
  (`GetCasterBounds`, selected ahead of the cascade fit).
- **Scale about the eye and the depth hook.** `SphereTerrain::Scale` multiplies the eye-relative
  position for the rasterized position only; the motion vector and the interpolated true clip
  position (`v_CurClip`) are the unscaled geometry's. `TerrainTrueViewDepth` / `TerrainTrueDeviceDepth`
  give a material its true depth to write through `GBufferDepthOutput`, which also has the deferred
  pass reconstruct the true position. A terrain at a scale other than one draws into no shadow view.
- **The material** declares `"sphereTerrain"` (see [../Asset/CLAUDE.md](../Asset/CLAUDE.md)); its
  fragment takes `SphereTerrainFragmentInput` and reads `TerrainChannel` (the tile's sample blended
  with the parent's by the geometry's own morph, so the shading has no seam where the geometry has
  none; `TerrainChannelAt` at caller-given coordinates, for a channel held at a divisor), `TerrainTexelSize`, `TerrainDirectionOffset` with `TerrainCenterHigh`/`Low`,
  `TerrainTileData`/`TerrainParentTileData`, `TerrainMorph`, `TerrainSurface`, `TerrainTileLevel`,
  `TerrainTileHeld` and `TerrainTileOverlay` (below). Each drawn surface
  takes one `DrawBudget` slot (`DrawPhase::SphereTerrain`) for its material and an identity frame;
  the push block's `InstanceDraw` names it and `InstanceBuffer` the surface drawn.
- **The near material.** A material whose fragment writes its own depth (`GBufferDepthOutput`)
  disables early depth rejection for everything it draws, and the ground filling the screen is
  where that costs most. So the terrain and each level layer may name a `NearMaterial` and a
  `NearDistance` (drawn metres, after the scale about the eye; 0 disables it): a main-view tile
  whose cull box's farthest corner, scaled about the eye, lies within the distance draws with the
  near material instead — the Euclidean distance bounds the forward depth from above, so the test
  is conservative. `BuildSphereTerrainFrame` puts such a tile in `SphereTerrainFrame::NearLists` (or
  `NearTranslucentLists` for a translucent layer) and not in its surface's own list, so no tile
  draws twice and each surface issues at most three instanced draws per view (the far material's
  below), each on a `DrawData` slot of its own (a translucent layer's near draw sorts just nearer
  than its own one). The near
  material must draw in the same pass as its surface's (reported and ignored otherwise); the
  shadow views take no near list (`tests/unit/sphere_terrain_cull.cpp`).
- **The far material** is shading level of detail: past a footprint where a material's finest
  detail contributes nothing, a cheaper material giving the same result can draw. So the terrain
  and each level layer may also name a `FarMaterial` and a `FarFootprint` (drawn metres a pixel
  spans, after the scale about the eye; 0 disables it): a main-view tile not taken by the near
  material whose cull box's nearest point (`SphereTerrainBoxDistance`, scaled about the eye) spans
  more than the footprint in the view's smallest pixel angle draws with the far material instead,
  listed in `SphereTerrainFrame::FarLists` (or `FarTranslucentLists`, sorting just farther than the
  surface's own draw). The smallest pixel angle (`SphereTerrainMinPixelAngle`) is the corner pixel's
  along the image's radius, `1 / (K (1 + tan²(fovX/2) + tan²(fovY/2)))`, so every pixel of a far tile
  spans at least the footprint, and a surface seen obliquely more. The near material wins where both
  apply; the same pass rule holds, and the shadow views take no far list.
- **Level layers** (`SphereTerrain::LevelLayers`, at most `MaxSphereTerrainLevelLayers`): each a
  second terrain material drawn over the same selection at a constant height in place of the height
  channel, with the terrain's placement, morph, scale and helpers, only on tiles whose working range
  dips below it; one more draw per view, an opaque layer casting like the terrain.
- **A translucent level layer.** A layer whose material is Translucent and declares
  `"sphereTerrain"` draws in the full-resolution translucent pass instead of the g-buffer, so a surface
  at a constant height — a water surface, a sheet of ice — can show what lies beneath it. The ground
  itself is always a Surface material (`CheckSphereTerrainMaterial`, pure, pinned by
  `tests/unit/sphere_terrain_translucent.cpp`; a translucent ground is reported once and not drawn).
  - **Lists.** `BuildSphereTerrainFrame` puts the layer's main-view tiles in
    `SphereTerrainFrame::TranslucentLists` and nowhere else: the g-buffer list is empty and it casts
    into no shadow view, as no translucent submesh does (`tests/unit/sphere_terrain_cull.cpp`).
  - **Sorting.** `ClaimDrawSlots` seats it on a `DrawData` slot like any surface and appends a
    `TranslucentDraw` carrying the terrain (`TranslucentDraw::Terrain`) to the full-resolution plan,
    which is re-sorted: by its material's `sortPriority`, then at its sphere centre's distance from
    the eye (not the centre's view z, which an eye on the ground looking up would put behind it). So
    within a priority it draws behind every translucent nearer than the centre, and a surface sharing
    that sort point (a shell sorted at the same centre) is ordered against it by priority.
  - **Sets.** Its pipeline is the translucent pass's, built against the material's `DrawData` set,
    the renderer's IBL (4) and shadow (5) layouts and the terrain's layout at
    `TranslucentSphereTerrainSet` (6), with no vertex input; it binds all four whatever the fragment
    declares, so forward lighting and `LightVisibility` are always available. The material loader
    lays such a material out as set 3, two empty sets and the terrain's own (`BuildPipelineLayout`),
    since the renderer's forward-lighting layouts are not the loader's. Seven sets in all, within
    `CommandBuffer::MaxBoundDescriptorSets`.
  - **Its contract** is `Veng/sphere_terrain_translucent.slang`, which moves the terrain set to 6 and
    re-exports `Veng/forward_lighting.slang` and `Veng/sphere_terrain.slang`; its vertex stage is
    `translucent_terrain.vert` (`TranslucentSphereTerrainVertexShaderId`), the same
    `SphereTerrainVertexStage` as the opaque one. The scene colour and depth copies are read as by any
    translucent material, behind `"readsSceneColor"` / `"readsSceneDepth"`.
  - **Depth.** It depth-tests against the opaque depth with writes off. A terrain drawn at a scale
    about the eye writes its true depth (`TerrainTrueDeviceDepth` through `GBufferDepthOutput`), so
    the opaque depth there is the true one, and a layer at a scale other than one tests against it
    by returning `TranslucentDepthOutput` with its own true depth; at scale one the rasterized depth
    is already true and the layer may return a bare colour.
  - **Both sides.** The grid is wound counter-clockwise seen from outside; a layer meant to be seen
    from below authors `"cull": "None"`.
  - **Not here:** the half-resolution and post-resolve layers (cook errors beside `"sphereTerrain"`).
    A layer with no tile in view still holds its sorted draw, and records nothing.
- **Shadows.** Each shadow pass draws each view's lists through a depth-only pipeline of its own
  (`shadow_depth_terrain.vert`, the same `SphereTerrainEvaluate`, both faces, the cascades with
  depth clamp). A terrain material may read the shadow maps through `Veng/light_visibility.slang`
  at set 5, beside its set 4, as any Surface fragment may (see "A Surface fragment may read the
  shadow maps"); the g-buffer pass then binds the renderer's shadow set for its draw.
- **What a terrain draw does not do:** it is not pickable, not drawn by the depth-and-normal path or
  under `GBufferShadingOverride` (whose pipelines take neither its stage nor its set 4), and not
  occlusion-tested against the hi-Z pyramid; it draws one material for its own surface plus its
  layers.
- **The tile overlay.** `SceneView::SphereTerrainTiles` (off by default; `ViewState` carries it, so
  a viewport's debug overrides reach it) sets a flag on every record the view draws, beside the
  record's held flag (`SphereTerrainRecord*`, `Layers.w`). A terrain material honours it by passing
  its surface colour through `TerrainTileOverlay` (`Veng/sphere_terrain.slang`): the tile's level as
  a colour (`SphereTerrainLevelColor`, successive levels a golden-ratio turn of hue apart), striped
  with its parent level's colour as far as it has morphed, darkened where the selection held it, and
  outlined at its edges from the tile parameter's screen derivative. With the flag clear the helper
  returns its input unchanged, and a material that never calls it ignores the switch.
- **Diagnostics.** `GetSphereTerrainStats()` adds the main view's tiles, the frustum and horizon
  culls, the shadow views' tiles, triangles, arriving and departing tiles, skipped terrains, the budget held and
  the frustum held; `GetDrawBudgetStats().SphereTerrainDropped` counts surfaces denied a slot.
  `tests/gpu/sphere_terrain.cpp` draws a test sphere from orbit to 2 m with no hole inside its
  silhouette, holds coverage and written depth across a scale of 1×10⁻³, shows it shadowing a cube at
  scale one and not scaled, leaves its colour alone with the tile overlay off and recolours and
  outlines it with the overlay on, and refuses an undeclared material.

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
slot reclaim through the same per-frame retire window (see "Slot-keyed deferrals"). **`PipelineLayout` reserves set 0 in every
pipeline** for the registry, bound once per pipeline bind (`registry.Bind(cmd)`), not per draw —
draws select array elements via push-constant indices. Sets 1 and 2 are the registry's volume and
cube arrays, so author-declared descriptor sets shift to **set 3+** (`BindlessRegistry::FirstUserSet`).

**Binding 8 is the normal-variance array**, a second sampled-image array shadowing the texture array
slot for slot: `Register(view, normalVariance)` writes a texture's companion there and `Register(view)`
the registry's one-texel zero image, so a shader reads any texture's normal variance with that
texture's own handle and a texture without one reads zero. `SetNormalVariance` re-points a registered
slot (a map recomputed in place). The companion's `Ref` is held per slot and dropped when the texture
slot is reclaimed.

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
