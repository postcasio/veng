# libveng — the runtime

The engine: the public API under `engine/include/Veng/` and the Vulkan backend
hidden behind it. This file documents the runtime's internal architecture; the
project-wide conventions it is written against (error policy, house vocabulary,
naming, comments, resource ownership, the Native idiom) live in the
[root CLAUDE.md](../CLAUDE.md). The editor framework is documented in
[editor/CLAUDE.md](../editor/CLAUDE.md), the offline cook pipeline in
[cooker/CLAUDE.md](../cooker/CLAUDE.md), and the archive format in
[assetpack/CLAUDE.md](../assetpack/CLAUDE.md).

## Application

Subclass `Application`, override `OnInitialize` / `OnUpdate(delta)` / `OnRender` /
`OnDispose`, and `Run(args)`. ImGui is opt-in (on by default; `nullopt` to skip),
and a `Headless` flag runs windowless to `RequestExit()` instead of a window
close — that's the CI/smoke path. `Application` owns the `AssetManager`
(`GetAssetManager()`), the render `Context`, and the `TaskSystem`
(`GetTaskSystem()`).

`Application` owns the `TaskSystem` — a fixed worker pool draining a work queue
and returning `Task<T>` handles — and threads it explicitly into the `Context`
(per-worker transfer pools) and the `AssetManager`, the same way the context is
threaded. It is pumped once per frame: `Frame()` calls
`TaskSystem::PumpMainThread()` at the top, before `BeginFrame()` advances the
frame, so off-thread continuations land on the main thread.

**`Application` drives the viewport list each frame.** It owns a non-owning,
ordered `vector<Renderer::Viewport*>` (registration order = render order);
`RegisterViewport(Viewport&)` appends a pointer and hands the viewport a
back-reference, so dropping the owner's `Unique<Viewport>` self-unregisters it
(`~Viewport` erases its own pointer, order-preserving) and the caller keeps
ownership — only the *driving* is central. `GetPrimaryViewport()` returns the
engine-owned **managed primary viewport** when `ApplicationInfo::ManagedViewport`
is set (`ManagedViewportInfo`: render extent, color format, `SceneRendererSettings`):
`Application` constructs one `Presented` viewport covering the whole window,
registers it, tracks swapchain resize, and exposes it — the plug-and-play path
where a game owns no `SceneRenderer`/sampler/texture/composite and just pushes its
scene through `GetPrimaryViewport()->SetViewState` each frame. The editor leaves
`ManagedViewport` unset, so `GetPrimaryViewport()` is null and it registers its own
viewports.

The **engine render phase** runs between `BeginFrame()` and `EndFrame()`, uniform
for every app and not overridable: render every registered viewport in
registration order (each does its own `Execute` + `PrepareForAccess(Sample)`), so
every viewport output is in `Sample` layout before `OnRender` builds the ImGui
draw data that may sample it. `OnRender` is **narrowed** to building the ImGui
frame and recording extra draws — it no longer runs the composite or
`ImGuiLayer::Render`. When ImGui is on, the frame then records the overlay and runs
the **managed tail**: the `GatherPass` assembles the registered `Presented`
viewports into one full-window assembly target and `SwapChainCompositePass`
composites it behind the ImGui overlay. The managed tail's gather + composite
graphs re-`Compile()` on swapchain resize, and the composite re-targets the
swapchain (`SetSwapChainTarget`) on a format change. See the `Viewport` section for
the viewport model the drive-list holds.

## Game modules: a shared lib + a launcher

A game is a **`libgame` (shared)** — the runtime: `Application` logic, components,
custom runtime types — loaded by a thin **launcher** (the shipped exe). The
launcher `dlopen`s the module and calls one C-ABI entry,
`VengModuleRegister(VengModuleHost*)`; the module registers its `Application`
factory into the host-owned `ApplicationRegistry` (`host->App.RegisterApplication(
[]{ … })`, one per module), **registers its reflected component/type descriptors
into the host-owned `TypeRegistry`** (`host->Types`, `Register<T>()` per type), **and
registers its gameplay `SceneSystem`s into the host-owned `SystemRegistry`**
(`host->Systems`, `Register<T>()` per system). `Application` borrows the `TypeRegistry`
and the `SystemRegistry` (`GetTypeRegistry()` / `GetSystemRegistry()`) and still owns
`Context`/`AssetManager`/`TaskSystem` unchanged — the
launcher reads the factory back, constructs the app, and calls `Run()`.
`veng_add_game(<name> SOURCES … [ASSET_PACK …])` is the build entry: it emits
`lib<name>` + `<name>-launcher` from one declaration. **`veng_add_editor(<name>
SOURCES …)`** is its editor sibling — it emits `lib<name>_editor` (SHARED, links
`libveng_editor`) + `<name>-editor` (the editor exe, links `libveng`,
`libveng_editor`, and `libveng_cook`); see [editor/CLAUDE.md](../editor/CLAUDE.md).

- **Same toolchain, one STL, one flag set.** Only the *entry* is C ABI; the payload
  is rich C++ (`string`, `vector`, `Ref<T>` flow across freely). veng is **not** a
  binary-plugin platform — a module is recompiled with the engine from one tree. A
  one-integer `VengModuleAbiVersion` handshake (checked by `ModuleLoader` before the
  entry runs) **rejects a stale module loudly at load**, not later. The ABI is at
  **version 3** (`VENG_MODULE_ABI_VERSION`); a module built against an older header
  fails the handshake at load. The host struct carries the `SystemRegistry` (`Systems`)
  beside `App`/`Types`/`Editor`. The gameplay layer adds **no** ABI surface: game modes
  are systems + components, the system catalog rides a per-system trait the way a
  component's `TypeId` does, and a `Level` is an asset — so it is registered through the
  existing `SystemRegistry`/`TypeRegistry` or authored as data, never through a new host
  entry. That ECS-native shape is what keeps it lighter than an actor-derived game-mode
  object would: nothing here is a host-ABI institution.
- **The relocatable trio.** The module resolves **beside the launcher** via an
  `$ORIGIN`/`@loader_path` rpath, and assets resolve via `ExecutableDirectory()`
  (the public executable-relative path helper) with `veng_add_game` copying the
  cooked pack beside the launcher — so launcher + lib + pack move as one directory.
- **`EditorRegistry*` is the editor-host seam.** The host struct carries `{
  ApplicationRegistry& App; TypeRegistry& Types; SystemRegistry& Systems;
  EditorRegistry* Editor; }`; the
  launcher always passes `Editor = nullptr` (the type is only forward-declared in
  `libveng`). The editor host (`EditorHost`, in `libveng_editor`) passes a non-null
  `&m_EditorRegistry`, activating a module's editor-side registrations.
- **Game-code hot-reload is out** — restart the play session. (Distinct from *asset*
  hot-reload, the async path.)

## RenderGraph: barriers fall out of declared use

Don't hand-write layout transitions/barriers. Declare a pass with the resources it
writes (`.Color(...)`) and reads (`.Sample(...)`); the graph derives the layout
transitions and drives `BeginRendering`/`EndRendering`.

Passes name **logical resources**, addressed by a vk-free `ResourceId`, not a
concrete `Ref<ImageView>`:
- **`CreateTransient({.Format, .Extent, .Usage})`** declares a graph-owned
  transient — the graph allocates its `Image`/`ImageView` at compile, resolves it
  per frame, and may alias non-overlapping transients onto shared backing.
- **`Import(name)`** declares an external resource (the swapchain image, an
  app-owned target). The graph never allocates or aliases it; its concrete view is
  supplied per frame as an `ImportBinding` passed to `Execute`.

A pass's `Execute` callback receives a **`PassContext`** — `Cmd()` for the command
buffer and `Resolved(ResourceId)` for a declared transient's concrete view this
frame. A callback may not capture a transient's view (an aliased transient has no
fixed backing); it resolves through the context at record time.

`RenderGraph` is a **builder**: declaring passes records nothing. `Compile()`
derives the barrier/transition schedule, allocates transients, builds each graphics
pass's `RenderingInfo`, and runs one-time validation, returning a
`Unique<CompiledGraph>`. `CompiledGraph::Execute(cmd, imports)` replays that baked
schedule per frame — only the per-pass callbacks run. A consumer re-`Compile()`s
only on a **structural** change (a pass added/removed, a transient's extent/format
changed); per-frame data never recompiles. See `BuildCompositeGraph` (compile) and
`CompositeToSwapChain` (replay) in the hello-triangle `main.cpp` for the pattern —
a member compiled graph held across frames, imports bound per frame, re-compiled on
resize.

## SceneRenderer: the deferred über-pipeline

`SceneRenderer` is a long-lived, configurable render pipeline on top of
`RenderGraph`: it owns an offscreen target, renders a `Scene` from a `Camera`
through an **internal compiled `RenderGraph`** of reusable `ScenePass` units, and
hands back a sampleable result. It is **`Unique`, single-owner** (nothing holds a
`Ref` to one); `Create(const SceneRendererInfo&)` is the factory.

Its surface is a **lifetime split** keyed on how often each piece of state changes:
- `Create(info)` — once: allocate persistent resources (output, g-buffer, HDR
  targets; fullscreen pipelines), build + compile the graph.
- `Resize(extent)` — recreate the extent-sized images via the retire path,
  re-register them into bindless, rebuild + re-`Compile()`.
- `Configure(settings)` — recreate affected resources, rebuild + re-`Compile()` the
  topology.
- `Execute(cmd, view)` — every frame: replay the graph against this frame's
  `SceneView`. **Never** reallocates or recompiles.
- `GetOutput()` — the sampleable `Ref<ImageView>` of the owned result. **Resize and
  Configure invalidate it** (the old image retires, a new one is created); a
  consumer caching a bindless `TextureHandle` or ImGui texture from it must re-fetch
  and re-register after those calls.

`SceneRenderer` is a **physically-based deferred renderer**: a metallic-roughness
three-target g-buffer (albedo G0, world-normal G1, packed occlusion/roughness/metallic
+ emissive G2, plus a sampled depth attachment) with **tangent-space normal mapping**,
a fullscreen **Cook-Torrance** lighting pass evaluating GGX specular + Lambert diffuse
over **multiple typed lights** (directional / point / spot) and reconstructing world
position from depth, then tonemap to the output. The batteries hang off the g-buffer:
**cascaded shadow maps** for the directional light and a **shared punctual shadow atlas**
for a bounded set of point/spot lights, **SSAO** folded into the ambient/occlusion term,
a **compute mip-pyramid bloom** ahead of tonemap, and an optional **TAA** resolve (off by
default) between lighting and tonemap. Each battery is a `SceneRendererSettings` toggle
driving the `Configure` recompile.

**TAA is an HDR-space temporal resolve** (`Settings.TAA`, off by default). It jitters the
projection by a Halton(2, 3) sub-pixel offset each frame (`Renderer/TaaJitter.h`, a pure
device-free helper), routes the lighting pass into a separate **lit** target, and inserts a
**resolve** pass (lit + reprojected history + velocity/depth → the HDR target the
bloom/tonemap tail already samples) and a **history-copy** pass (HDR → the persisted history
for next frame). Motion is **per-object** and **folded into the g-buffer pass**: velocity is
a fourth g-buffer channel (**G3**, `RG16Sfloat`), written by the surface fragment as
`SV_Target3` alongside G0/G1/G2 — `curUV - prevUV` from the per-vertex current and previous
clip positions, computed by the shared `ComputeMotionVector` helper. So there is **no
separate velocity prepass**: the one geometry rasterization that fills the g-buffer also fills
velocity. The previous position comes from a per-draw `PrevWorld` matrix (`GpuDrawData` carries
it; the renderer tracks each entity's prior world in `m_PreviousWorlds`, keyed by packed
`Entity` and swapped each frame) and the unjittered `CurViewProj`/`PrevViewProj` (both in the
set-0 view-constants block); the skinned surface vertex stage additionally skins the previous
position through the previous-frame palette (`PrevPaletteBase`), so deformation motion writes
velocity too. The resolve uses the velocity vector for geometry (camera **and** object motion)
and falls back to depth-based camera reprojection for the cleared background. Because velocity
is a g-buffer channel it is **always written and always allocated** (the cost is one extra
`RG16Sfloat` target plus a clip-position write, not a second geometry pass); with TAA off it is
written but unread, and the `MotionVectors` debug arm blits it directly. The opaque material
contract is therefore **G0/G1/G2/G3** — velocity is the fourth MRT channel of the surface
output, not a separate pass. History is **YCoCg variance-clipped** to the 3×3 neighborhood, sampled with a
**Catmull-Rom** filter, and blended with **luminance weighting** (Karis anti-flicker);
offscreen reprojection and the first frame after a `Resize`/`Configure` fall back to the
current color (`m_TaaHistoryReset`). The history is a renderer-owned persisted image written
and read within the renderer's own single-queue graph each frame, so it needs no cross-frame
ring or semaphore.

The directional light is shadowed by **cascaded shadow maps**, and a **bounded set of
punctual lights** (`MaxShadowedPunctual`) by a **shared punctual shadow atlas**. The
directional cascades split the camera frustum into depth slices, each cascade fit
(bounding-sphere + texel-snapped) to its slice and rendered into a depth **atlas** in
**one** pass (per-cascade viewports); the lighting pass selects the cascade by the
fragment's view-space depth, remaps to the atlas tile, and **`SampleCmp`s** it through a
**hardware comparison sampler** with a boundary cross-fade. Cascade fit is pure,
device-free math (`Renderer::ComputeCascades`, `Veng/Renderer/ShadowCascades.h`) over the
camera, light direction, and the world-space scene bound (the bound only extends each
cascade's near plane toward the light to catch off-screen casters; the XY extent is the
frustum slice). The punctual lights add the second arm: a **spot** renders one
perspective shadow map through a single frustum, a **point** renders six 90° cube faces,
both into the shared punctual atlas (a 2D atlas of `MaxShadowedPunctual·CubeFaceCount`
tiles); the lighting pass samples each shadowed light's map (projective for a spot,
cube-direction for a point) with the **same** hardware `SampleCmp` + PCF and multiplies
the visibility into that light's contribution. The punctual view math is pure,
device-free glm (`Renderer::ComputeSpotShadowView` / `ComputePointShadowView`,
`Veng/Renderer/PunctualShadows.h`) beside `ShadowCascades.h`. Each shadow view culls its
casters through `SceneBroadphase::Cull` against **its own** frustum — the camera frustum
for the g-buffer, each cascade's light frustum, each spot's frustum, each cube face's
frustum.

**Set 1 is a general shadow system**, not just the directional one: the directional
cascade atlas, the punctual shadow atlas, a **shared** immutable comparison sampler
(hardware `SampleCmp`), the per-frame `ShadowConstants` block (the directional cascade
matrices + splits + params) bound as a **dynamic uniform**, and the `PunctualShadowBlock`
(the per-light shadow records — view-proj(s), tile rects, type) ringed beside it. All of
set 1 is held **off the set-0 bindless registry**, where a comparison sampler
mistranslates inside the Metal argument buffer on MoltenVK and a closed
producer→consumer resource needs no global registration. The set-0 view-constants block
stays trimmed to material-facing camera/view state. A `GpuLight`'s shadow **slot** (an
index into the punctual record array, or `-1` for unshadowed) rides the `Cone.zw`
padding, so `LightStride` is unchanged. `CascadeCount`, `CascadeSplitLambda`, and
`ShadowResolution` (default 1024) are the directional CSM knobs; `PunctualShadows` (the
on/off toggle) and `PunctualShadowResolution` (the per-tile edge length) are the punctual
knobs; `DebugView::Cascades` tints each fragment by the cascade it selects and
`DebugView::PunctualShadows` blits the punctual atlas. This per-light shadow cull is the
**prime consumer of the delivered BVH broadphase** — one tree queried many times (`N`
spot frustums + `6N` cube faces per frame, on top of the camera and cascade queries).

**Bloom is a compute mip-pyramid battery**, a fixed engine pass like SSAO and the shadow
atlas — not a PostProcess material. The lit HDR target is bright-passed (a soft-knee
`Threshold` with **Karis-average** firefly suppression on the first downsample, the
firefly-stability mechanism that holds whether or not the optional TAA resolve is on) into a
single `HdrFormat` mip-chain image's
mip 0, **progressively downsampled** through the chain, then **upsampled** with an
accumulating dual filter (`mip[i] += upsample(mip[i+1]) * Radius`) and **composited** back
into linear HDR (`hdr + mip0 * Intensity`) ahead of tonemap. The whole sweep is **compute**:
per-level dispatches with a barrier between levels, mirroring the hi-Z reduction's mip-chain
shape (one image with N mip levels, per-mip storage views for the writes, a whole-chain
sampled view + a clamp-to-edge linear sampler for the bilinear taps, per-level descriptor
sets, all off bindless). The filter kernel is a `BloomKernel { Cod, Kawase }` topology knob
— the COD/Jimenez 13-tap-down / tent-up dual filter (the default and the golden's kernel)
or the bandwidth-optimized **Dual Kawase** filter for the TBDR GPUs veng primarily targets.
`Bloom` (on/off) and `Kernel` are `SceneRendererSettings` topology knobs (a `Configure`
recompile); `Threshold` / `Intensity` / `Radius` are per-frame `SceneView` values that ride
the compute push, so tuning them never recompiles. `DebugView::Bloom` blits pyramid mip 0
after the up-sweep — the accumulated bloom contribution before composite.

**Image-based lighting is a split-sum IBL battery driven by a per-scene environment map.** A
resident `AssetHandle<Environment>` rides the per-frame `SceneView` (set by the app through the
`Viewport`'s `ViewState`, like `Exposure`); a renderer-owned **`EnvironmentIbl`** helper
(`engine/src/Renderer/EnvironmentIbl.{h,cpp}`) generates the maps it derives — a **radiance
cubemap** (the skybox source), a **diffuse irradiance cubemap**, a **GGX-prefiltered specular
cubemap** (roughness mip chain), and the environment-independent **BRDF integration LUT** — all
through compute (the four `ibl_*.comp` core shaders), mirroring the bloom/hi-Z
compute-with-manual-barriers pattern (per-face/per-mip storage views, cube sampled views,
explicit `PrepareForAccess` barriers, all off bindless). Generation is recorded **once when the
bound environment changes** (a `m_LastEnvironment` gate in `Execute`, into the same command buffer
before the graph runs); the BRDF LUT is generated once on first use. The four sampled maps + a
linear sampler reach the deferred lighting pass as **one dedicated descriptor set bound at set 2**
— **off the set-0 bindless registry**, mirroring the shadow-atlas "closed producer→consumer"
precedent (a cubemap in a Metal argument buffer is a MoltenVK risk, and a closed resource needs no
global registration). The lighting fragment replaces its flat hemispheric ambient with `kD ·
irradiance · albedo` diffuse + `prefiltered · (F · brdf.x + brdf.y)` specular when an environment
is bound, and **folds the skybox into the same pass** — at cleared-depth background it samples the
radiance cube along the view ray rather than a separate skybox pass. IBL is a **runtime push flag**
(`IblEnabled`), not a pipeline variant: the set is always bound and valid (the maps are
transitioned to a sampled layout at first `Execute` even before an environment arrives), so a scene
**without** an environment falls back to the exact flat-ambient path and renders unchanged.
`SceneView::EnvironmentIntensity` and `Skybox` are per-frame push values (no recompile).

Per-view data rides a **ring-buffered view-constants buffer**, not push constants: the
`InvViewProj`/`CameraPosition` (for world-position reconstruction), the view/projection,
and the SSAO view/projection live in a per-frame set-0 buffer ringed for frames-in-flight
and selected by an index fold (a dynamic-offset descriptor mistranslates in set 0 on
MoltenVK). Its stride is **512 bytes**. The shadow system's own state — the cascade
matrices, splits, and params — rides the **set-1** `ShadowConstants` block instead, so
set 0 stays a lean, material-facing view block (shared by materials, lighting, and SSAO).
Push constants in the deferred path carry only
small per-invocation bindless handle indices and the live light count; the typed lights
ride a separate ring-buffered light buffer the lighting pass loops over.

The renderer's pipeline images (g-buffer albedo / world-normal / ORM, depth, HDR, the bloom
mip pyramid + composite result, output) are **renderer-owned `Image`/`ImageView`s `Import`ed** into the internal graph — not
graph transients — because a fullscreen pass samples an upstream target through the
bindless set-0 array, which needs a `Ref<ImageView>` to `Register` (a transient
exposes only a per-frame `ImageView&`). They are registered into bindless once at
`Create` (re-registered on `Resize`) and reach the sampling pass as `TextureHandle`s
through `PassIO`. The one exception is the **shadow atlas**: a closed
producer→consumer resource (the shadow pass writes it, the lighting pass and the
`DebugView` blit read it, nothing else) reaches its consumer through a **dedicated
descriptor set** via a `PassIO` **bound-view** slot — off bindless — because the
lighting pass uses a comparison sampler / `SampleCmp`, which a set-0 bindless argument
buffer bars on MoltenVK, and a closed resource needs no global registration. It is
still an `Import`ed, graph-declared resource (the lighting pass's `.Sample` drives the
graph-derived `DepthAttachment → ShaderReadOnly` barrier); only its *binding* sits off
bindless.

The per-frame `SceneView { const Scene& World; const CameraView& Camera; Light Light;
f32 Delta; }` reaches pass callbacks through an **opaque `void* userData`** channel
on `RenderGraph::PassContext` / `CompiledGraph::Execute` — so `RenderGraph` stays
scene-agnostic. A `ScenePass` reads it back through a typed `ScenePassContext`
(`Cmd()` / `View()` / `Resolved(id)`); `View()` asserts the pointer is non-null
before the reinterpret, and `SceneRenderer` sets it on every `Execute`.

The scene-drawing passes **cull at submesh granularity through a BVH broadphase**. A
renderer-owned `SceneBroadphase` (`Veng/Scene/SceneBroadphase.h`) holds a bounding volume
hierarchy whose **leaves are per-submesh** — one leaf per `SubMesh`, on its local-space `AABB`
folded over the submesh's index range at load (no cooked-format change). Each `Execute` calls
`SceneBroadphase::Sync`: it re-gathers the candidates (the pure `GatherMeshes` pass,
`Veng/Scene/Visibility.h`, over every resident `(Transform, MeshRenderer)` entity — world
matrix + world-space `AABB` + resident mesh) and rebuilds the tree **only on a frame the
scene's spatial version moved** (or a still-loading mesh became resident) — a static scene
rebuilds the tree not at all and queries a stable one. The gathered list rides `std::span<const
VisibleMesh> Visible` on `SceneView`; `SceneBroadphase::Cull` descends the tree once per view —
the g-buffer geometry pass with the **camera** frustum, the cascaded shadow pass once per
cascade with **each cascade's** light frustum, each punctual light's view with its own — so the
many-view shadow workload queries **one tree, many times** rather than re-scanning the list per
view. A query returns exactly the linear scan's per-submesh survivor set (a node wholly outside
a frustum rejects its subtree; a leaf is accepted on its tight box), so the cull is conservative
(an extra draw, never a dropped visible submesh) and the rendered image is **byte-identical** —
only the draw calls issued differ.

`SceneRendererSettings::Cull` selects how those survivors are submitted. Under
**`CullMode::CPU`** (the default) the renderer records a direct per-submesh draw for each
camera-frustum survivor. Under **`CullMode::GPU`** the same frustum survivors are uploaded to a
GPU buffer, a **compute** pass runs a **hi-Z occlusion test** over each candidate's screen-space
AABB against the **previous-frame depth pyramid** and writes each `VkDrawIndexedIndirectCommand`'s
`instanceCount` (1 for a survivor, 0 for an occluded candidate, which executes as a no-op), and
the geometry pass issues the whole fixed buffer through a single `vkCmdDrawIndexedIndirect` per
mesh group. The compute does **not** re-run frustum culling — the BVH already did; it adds only
occlusion. The hi-Z pyramid is a **max-Z mip chain** reduced from the depth target by compute
into a renderer-owned, cross-frame-persisted resource (temporal hi-Z: the test reads last
frame's chain, so a history-invalid frame — frame 0, the frame after a `Resize`/`Configure`, or
a large view delta — is frustum-only, never a stale false-cull). `SceneRendererSettings::Occlusion`
gates the occlusion test within the GPU path; with it off the GPU path issues every
camera-frustum survivor. The submission shape is the **`drawIndirectCount`-free** form MoltenVK
supports (`multiDrawIndirect` + `drawIndirectFirstInstance`, the candidate id carried in each
command's `firstInstance` and read as an instance-rate vertex attribute); both modes drive the
**same buffer-indexed surface shader**, differing only in submission. `CullMode::GPU` is gated on
`Context::IsGpuDrivenCullingSupported()`: on a device lacking either feature the renderer logs
once and falls back to `CullMode::CPU`, and `GetActiveCullMode()` reports the real mode.

`SceneRendererSettings::FrustumCull` (default on) toggles the frustum cull itself; the cull
funnel is reported by `GetLastVisibleCount()` (gathered submesh candidates) → `GetFrustumSurvivedCount()`
(frustum survivors) → `GetLastDrawnCount()` (drawn — equal to the frustum survivors on the CPU
path), with `GetLastGpuSurvivorCount()` the GPU occlusion survivor count read back one frame late
under `CullMode::GPU`, and
`DidBroadphaseRebuildLastFrame()` / `GetBroadphaseNodeCount()` reporting whether the tree rebuilt
and its size. The **BVH broadphase, per-submesh leaves, and GPU-driven hi-Z occlusion culling are
delivered**; **incremental tree maintenance** (per-object insert/update/remove with fat boxes),
**meshlet/cluster-granularity GPU culling**, **two-pass occlusion** (the higher-quality
alternative to temporal hi-Z), and **GPU-driven shadow-caster culling** (the highest-payoff next
consumer — the `N` spot + `6N` cube shadow views) are its refinements behind the same
`Sync`/`Cull` + version-gate seam.

A `ScenePass` is a reusable, self-contained pipeline stage (`Configure` / `Resize` /
`Declare(RenderGraph&, const PassIO&)`) that **contributes** one or more
`RenderGraph` passes into the renderer's single internal graph — it is not a
`RenderGraph::Pass`. The renderer owns the **wiring** (which pass reads whose
target, via the named-slot `PassIO`); each pass owns **itself** (sizing, declared
reads/writes, recording). It knows only how to record, never what feeds it.

The über-pipeline is **batteries-included, not extensible**: a bespoke pass graph
still means dropping to `RenderGraph` directly (the composite path the sample
retains). `SceneRendererSettings` carries the topology/sizing knobs — `DebugView Mode`
(Final, plus the `Albedo` / `Normal` / `Depth` g-buffer arms, the `Roughness` /
`Metallic` / `Occlusion` packed-ORM-channel arms, and the `AO` / `Shadows` / `Cascades` /
`PunctualShadows` / `Bloom` / `MotionVectors` battery-target arms) re-wires the pass set through
`Configure`, the recompile seam; the `Bloom` / `Shadows` / `PunctualShadows` / `AO` battery
toggles, the bloom `Kernel`, and `ShadowResolution` / `PunctualShadowResolution` are the other
recompile knobs. A debug arm terminates the chain after the g-buffer (and, for `AO` / `Shadows` /
`PunctualShadows`, the force-wired producing battery pass) with a single fullscreen debug blit;
the `Bloom` arm additionally runs the lighting pass and the force-wired bloom sweep and blits
pyramid mip 0 after the up-sweep, and the `MotionVectors` arm blits the per-object velocity
g-buffer channel (written by the surface pass every frame) colorized as an optical-flow
field (hue = direction, brightness = magnitude). Per-frame values (`Exposure`, the bloom
`Threshold` / `Intensity` / `Radius`, the camera, the lights) ride `SceneView`, so tuning them
never recompiles.

The renderer-owned images are **single-copy**: one `Execute` resolves and completes
before the next begins, written-then-read images within a frame are ordered by the
graph's derived barriers, and the retire path covers destruction safety on
`Resize`/`Configure`. The output is consumed in the frame it is written — a
compositor samples `GetOutput()` for the same frame the renderer wrote it.

**Frames-in-flight contract.** The output stays single-copy across frames-in-flight.
A consumer transitions it for its read (`PrepareForAccess(Sample)`) and the next
frame's scene render transitions it back (`PrepareForAccess(ColorAttachment)`,
recorded by the renderer before each `Execute`), bracketing a cross-graph handoff no
single graph can derive a barrier for. This barrier suffices without a semaphore or a
ring because both halves record on the single graphics queue in submission order, so
the barrier's first synchronization scope reaches the prior frame's read; the internal
targets (g-buffer, depth, HDR) are single-copy and serialized by the renderer's own
graph. Ringing the output — or a cross-queue semaphore — is reserved for a future
async consumer or a handoff side moving off the single graphics queue. The TAA resolve
needs neither: its history is a renderer-owned persisted image written and read inside the
renderer's own single-queue graph each frame, ordered by the graph's derived barriers.

**The deferred opaque material g-buffer contract.** An opaque (Surface-domain)
material's **fragment shader outputs** are **g-buffer channels**, not final swapchain
color, written through a single engine-provided `GBufferOutput` struct
(`float4 Albedo : SV_Target0; float4 Normal : SV_Target1; float4 ORM : SV_Target2;
float2 Velocity : SV_Target3;`).
Albedo (G0) is sRGB-encoded (sampled back as linear); the normal (G1) is the
tangent-space-perturbed world normal in a signed float format; ORM (G2) packs occlusion
(R), roughness (G), metallic (B), and emissive strength (A) — the metallic-roughness
PBR channel set; velocity (G3, `RG16Sfloat`) is the per-object screen-space motion vector
(`curUV - prevUV`) the TAA resolve reprojects through, written by the shared
`ComputeMotionVector` helper from the vertex stage's unjittered current/previous clip
positions. Depth is the depth attachment, also sampled by the lighting pass for
world-position reconstruction (one of the depth targets read as textures in the engine —
the directional cascade atlas and the punctual shadow atlas are the others). The g-buffer
layout (channels,
formats, usage) is fixed in `Renderer/GBuffer.h`, agreed on by the geometry pass's
`RenderingInfo` and every material pipeline. It is the **opaque** contract — a
transparent/forward material outputs final color through a separate fragment entry, not
a change to this one — and **colored** emissive (an emissive color distinct from albedo)
is the one free channel away from needing a fifth target (only G0.a is unused). Folding
velocity into the surface output means motion vectors cost **no second geometry pass** —
the single g-buffer rasterization fills them — at the price of a small always-on write even
when TAA is off. Set-0 bindless, the material parameter block, and texture handles are
unchanged — only the fragment shader's outputs move to the g-buffer.

**The PostProcess fullscreen-material path.** A `PostProcessScenePass` runs a PostProcess
material as a fullscreen effect: it builds a `GraphicsPipeline` from the material's
fragment shader against a renderer-supplied color format (fullscreen triangle, one color
target, no vertex inputs), binds set-0 bindless, runtime-binds an upstream target as a
material handle field (`Material::SetTextureHandle`/`SetSamplerHandle`, no resident asset),
and drives the material's authored params. The loader builds the pipeline *layout* for both
domains but the `GraphicsPipeline` only for Surface — a PostProcess material's pipeline is
built by the pass, which alone knows the color format. **Tonemap is the PostProcess
material** (core `tonemap.vmat`): the HDR (or bloom-composite) target is runtime-bound each
frame and the per-frame `Exposure` from `SceneView` is written into the ring-buffered block
each `Execute`. The fixed plumbing composites stay hardcoded engine passes —
`SwapChainCompositePass` (scene behind, ImGui over) and the `DebugView` blits
(albedo/normal/depth, the packed-ORM channels, the SSAO target, the bloom pyramid, the
directional and punctual shadow maps) have no
authorable surface; a PostProcess material is for *tunable effects with exposed
parameters*, not plumbing.

## Viewport: a region + a renderer + a role

A `Viewport` (`Veng/Renderer/Viewport.h`) is *"a renderable view into a world"*
made first-class: it owns a `SceneRenderer`, carries a **`ViewportRegion`** (its
rectangle in window framebuffer pixels — an `Offset` and an `Extent`, the extent
driving the render resolution), takes a per-frame **`ViewState`** *pushed* by its
owner, and exposes a **`ViewportRole`**. It is **`Unique`, single-owner**;
`Create(const ViewportInfo&)` is the factory. Owning the region is what makes the
name correct — a viewport is classically a rect of the render target — and it is
what lets the engine drive a list of them: every consumer that wanted a rendered
scene used to hand-wire the same trio (a `SceneRenderer`, a sampler, an ImGui
texture, the `Execute` + `Sample` barrier); the `Viewport` is that trio owned once.

- **The role gates engine compositing, nothing else.** `ViewportRole::Presented` —
  the engine compositor places the viewport's texture into its region (a fullscreen
  game is one viewport covering the window; a splitscreen quadrant is one of N).
  `ViewportRole::Offscreen` — a consumer samples the texture (an ImGui panel, a
  material). Both roles render *identically*: every viewport renders into its own
  target at its region's resolution, and the deferred pipeline never scatters into a
  swapchain sub-rect. The region is universal state — an `Offscreen` editor panel
  still owns a region (for resize + picking); the role only decides whether the
  **engine** places it.
- **Push the per-frame source.** The owner sets a `ViewState` each frame (the
  `Scene` to render, the resolved `CameraView`, `Delta`, and the tone/bloom knobs —
  the input subset of the renderer's internal `SceneView`); the viewport never
  reaches into the scene for a camera. A null `World` renders nothing (a closed
  document is a no-op, not a null deref). The viewport retains the camera for
  screen-to-world mapping.
- **`Render(cmd)` does Execute + the Sample barrier.** It applies any pending region
  resize, builds the internal `SceneView` from the bound `ViewState`, calls
  `SceneRenderer::Execute`, then `PrepareForAccess(Sample)` — so the output is
  sampleable when the frame's later consumers read it. Its product is a sampleable
  `Ref<ImageView>` (`GetOutput()`) and a bindless `TextureHandle` (`GetOutputHandle()`)
  for the compositor, `ImGuiLayer::CreateTexture`, or `Material::SetTextureHandle`.
  Both invalidate on an extent change applied in `Render` and on `Configure` —
  re-fetch after, exactly as the underlying `SceneRenderer::Resize`/`Configure`
  invalidate `GetOutput()` (the lifetime split + composite encode are unchanged; see
  the `SceneRenderer` section).
- **Central driving, local ownership, RAII cleanup.** The engine drive-list holds
  raw `Viewport*` (registration order = render order), not the viewports; the owner
  constructs and registers — `m_vp = Viewport::Create(info); app.RegisterViewport(*m_vp);`
  — keeping the owning `Unique`, and `~Viewport` removes the engine's pointer through
  the stored back-reference. So registration is explicit but dropping the `Unique` is
  the whole of cleanup, and "0..N viewports including zero" is the list length.
- **The render-phase order is render-all → `OnRender`/ImGui → gather + composite.**
  The engine renders every registered viewport first (so every output is in `Sample`
  layout), then `OnRender` builds the ImGui frame (an `Offscreen` panel draws
  `UI::Image(vp.GetOutput())`), then — when ImGui is on — the overlay records and the
  managed tail gathers the `Presented` viewports and composites. The managed primary
  viewport is the game's plug-and-play path; the editor registers no `Presented`
  viewport, so the gather assembles **zero placements** (a cleared target) and the
  composite is ImGui-only.

**A gather pass assembles; the composite encodes.** `GatherPass` (`Veng/Renderer/GatherPass.h`)
scissor-blits each `Presented` viewport's texture (a `CompositePlacement` = its
`Ref<ImageView>` + its `ViewportRegion`) into its region on one full-window
linear-HDR (RGBA16F) **assembly target**, in list order, clearing the area no
placement covers; `SwapChainCompositePass` then consumes that single target
*unchanged* (ImGui over, the display-transfer encode once). One window-covering
placement is the old fullscreen-game case (a point-sampled same-resolution copy, so
the assembled values are bit-identical to sampling the source directly); zero
placements is the editor (a cleared target); N quadrant placements is splitscreen —
the same gather + composite tail for all three, the HDR/color-space encode left
untouched. `SetPlacements` registers exactly one bindless slot per placement
(`MaxPresented` is the budget, asserted at register time). **Splitscreen falls out**
as "register N `Presented` viewports with quadrant regions"; it needs no bespoke
compositing path.

**Owning the region yields a window↔view mapping.** `WindowToViewport(windowPoint)`
hit-tests a window point against the region and, on a hit, remaps it to normalized
`[0,1]` across the region (nullopt outside); `ScreenToWorldRay(windowPoint)` composes
that with the camera retained from the last `ViewState` — mapping the point to NDC and
unprojecting it through `glm::inverse(camera.ViewProjection())` into a world-space
`Ray` (`Veng/Math/Ray.h`, a glm-only origin + direction value type) whose origin is
the camera and whose normalized direction passes through the pixel (nullopt outside
the region or before any `ViewState`). These are **gameplay-agnostic** primitives —
the viewport imports no `Viewer`/`PlayerInput`; it supplies the ray, and what the ray
hits (a scene raycast) is editor or gameplay code. Editor entity-picking consumes them
now; multi-seat *pointer* routing (which quadrant a click landed in) consumes them
later.

**The registration-order RTT contract.** An `Offscreen` viewport's `GetOutputHandle`
can be bound into a material (`Material::SetTextureHandle`) so one viewport samples
another's output. Because registration order is render order, a producer registered
**before** its consumer ends its `Render` with the output in `Sample` layout before
the consumer's `Render` reads it. Both halves record on the single graphics queue in
submission order, so the handoff needs **no ring and no semaphore** and the output
stays **single-copy**; the producer's next-frame `Execute` transitions it back to
`ColorAttachment`. (A declared topological render order over registration order, and
output ringing for an off-queue/async consumer, are future refinements behind this
seam — the single-copy contract holds for same-frame, same-queue RTT.)

## Pipeline cache

`Context` owns a `vk::PipelineCache` created at device init and threaded into both
the graphics and compute pipeline factories — every pipeline build in a run reuses
it. Persistence is **opt-in** via `ApplicationInfo::PipelineCachePath`: set → seed
from the file at startup + write it back at shutdown; `nullopt` (default) keeps it
in-memory only. A stale/foreign/truncated cache file is safe — Vulkan validates the
cache header and starts cold on a mismatch; veng feeds the bytes as `pInitialData`
and never parses them. The cache is touched only on the single render thread, so it
needs no external sync (off-thread pipeline creation would).

## Bindless: set 0 is the engine's

The engine provides a global `BindlessRegistry` (owned by `Context`, reachable via
`Context::GetBindlessRegistry()`): a few large arrayed, `partiallyBound` +
`updateAfterBind` bindings (sampled images, samplers, storage images, and the
per-material parameter-block SSBO) living in **set 0**. `Register(...)` allocates a
free-list slot and returns a typed `u32` handle (`TextureHandle`, `SamplerHandle`,
`StorageImageHandle`, `MaterialHandle`); `Release` defers the slot reclaim through
the same per-frame retire window. **`PipelineLayout` reserves set 0 in every
pipeline** for the registry, bound once per pipeline bind (`registry.Bind(cmd)`),
not per draw — draws select array elements via push-constant indices. Author-
declared descriptor sets shift to **set 1+**.

## Veng::UI: the engine-tier immediate-mode vocabulary

`Veng::UI` (`engine/include/Veng/UI/`, in `libveng`) is the engine-tier immediate-mode
vocabulary fronting ImGui. UI is authored against it, not raw `ImGui::`, at every widget
site — game modules and the editor both. (`Veng::UI` is the only reason a game links a UI
surface; it does not link the editor framework for a debug slider.)

- **One `Drag`, overloaded on the value type** — `f32`/`vec2`/`vec3`/`vec4`/`i32`. Options
  are designated-initializer structs (`DragOptions`, `SliderOptions` — `.Speed`/`.Min`/
  `.Max`/`.Format`), never an `ImGui*Flags` value. Configurability is deliberately reduced
  for call-site consistency: only the knobs the engine wants UI authors to vary are exposed.
  The closed `FieldClass` set maps onto this closed overload set, so the reflection
  inspector's `Vector` dispatch is a single `Drag` call.
- **Every editable widget returns `[[nodiscard]] bool`** ("changed"), keeping immediate-mode
  semantics.
- **Text is preformatted `string_view`, not printf varargs** — a caller writes
  `UI::Text(fmt::format("{}: {}", a, b))`. fmt is veng's one formatting idiom.
- **RAII scope guards** (`UI::Window`/`Child`/`TreeNode`/`CollapsingHeader`/`Table`/`Menu`/
  `Popup`/`Disabled`/`PushId`/`StyleColor`/`StyleVar`/…) replace every begin/end and
  push/pop pair, closing on scope exit so the close survives every early-out. Flags are
  engine vocab enums (`WindowFlags`, `TreeFlags`, `StyleColorId`, `StyleVarId`), not
  `ImGui*Flags`. `EditorPanel::GetWindowFlags()` returns `UI::WindowFlags`.
- **The `Veng/UI/` headers are imgui-free in their signatures and members.** `<imgui.h>`
  appears only under `engine/src/UI/` (the scope-guard dtors are defined out-of-line there).
  The one ImGui-adjacent type a signature names is the engine's own `ImGuiTexture`
  (`UI::Image(const Ref<ImGuiTexture>&, vec2)`), already an engine wrapper. This keeps the
  surface within `include_hygiene`'s existing guarantee.
- **ImGui stays a PUBLIC dependency** (wrapper-only) — the aim is call-site consistency and a
  tight surface, not hiding ImGui. Driving ImGui fully private (no `<imgui.h>` reachable
  through any public header, imgui linked PRIVATE) is a possible later planset `Veng::UI`
  unblocks.
- **`ImVec2`/`ImVec4` convert implicitly to/from glm's `vec2`/`vec4`.** `Veng/Vendor/ImGuiConfig.h`
  injects the conversions through ImGui's `IM_VEC2_CLASS_EXTRA`/`IM_VEC4_CLASS_EXTRA` hooks, wired
  as the `IMGUI_USER_CONFIG` compile definition (PUBLIC on `veng`, so imgui's own aggregation TU
  and every consumer compile one identical `ImVec2`/`ImVec4` — the macros add member functions
  only, no data members, so layout is unchanged). So a raw imgui/imnodes call takes a `vec2`
  directly and its return reads straight into a `vec2`; no `ImVec2(v.x, v.y)` glue at the
  boundary. ImGui-native literals (theme colors/padding in `ImGuiLayer`) stay authored as
  `ImVec2`/`ImVec4` — the conversion is for the glm seam, not a reason to rewrite native values.
- **The boundary.** `Veng::UI` replaces `ImGui::` only at widget-authoring sites. ImGui's
  frame lifecycle (`CreateContext`/`NewFrame`/`Render`/`GetDrawData`/`GetIO`/…) and the
  host/dock/present plumbing (`DockSpaceOverViewport`/`UpdatePlatformWindows`/…) are the
  integration layer and stay raw in `ImGuiLayer`/`EditorHost`. Immediate-mode input queries
  for UI logic (double-click, right-click menu, delete-key shortcut) are wrapped in
  `Veng/UI/Query.h` over closed `UI::MouseButton`/`UI::Key` enums — distinct from the
  event/input system, which feeds gameplay; the `Key` enum is populated to the keys the call
  sites use. The only ImGui types remaining in panel src are those crossing into imnodes
  (`ImVec2` for node positioning), which is itself an editor-private dependency.

## Assets: cook offline, load by `AssetId`

The on-disk archive format lives in [assetpack/CLAUDE.md](../assetpack/CLAUDE.md) and
the offline cook that produces it in [cooker/CLAUDE.md](../cooker/CLAUDE.md); this
section covers runtime loading.

Assets are **cooked offline** into a binary archive, never imported at runtime —
there is no cook-on-demand, no source parser, no re-cook path in `libveng`. The
`vengc cook` tool (built behind `VENG_BUILD_TOOLS`, wired into the example's
build) turns a hand-written JSON **asset pack** into a single `.vengpack`
archive; the engine *mounts* archives and resolves assets against them.

- **`.vengpack` archives (format v2) carry content hashes.** Every cooked blob
  gets a content hash and the table of contents gets a digest (over the serialized
  TOC bytes), cooker-written via xxh3-128 and checkable with **`vengc verify`** (it
  re-hashes the blobs + digest and exits nonzero on any mismatch). **The loader
  never verifies** — hashing is tooling, not the hot path; the runtime trusts its
  packs. The hash function lives **only** in the cooker/verify tool, so `assetpack`
  (which stores the raw 16 bytes and computes nothing) and `libveng` gain no hash
  dependency.
- **An asset pack is a pure `{ id, type, source }` manifest.** It carries no
  per-asset settings. **Every** asset type — texture, mesh, shader, material,
  prefab — has its own per-asset JSON source file (`*.tex.json` / `*.mesh.json` /
  `*.shader.json` / `*.vmat.json` / `*.prefab.json`) that the manifest entry points
  at; the sampler settings, import options, shader source/entry, material fields,
  and prefab entities/components live in those files.
- **Load is by opaque `u64` `AssetId`** through mounted archives.
  `AssetManager::Load<T>(AssetId)` is **async by default**: it returns a
  not-yet-resident `AssetHandle<T>` immediately and runs the decode + GPU upload
  on the task system (transfer queue, no frame stall); poll `IsLoaded()` before
  using it. `AssetManager::LoadSync<T>(AssetId)` is the **blocking** sibling — it
  runs the whole pipeline inline and returns a resident handle or a structured
  error, `AssetResult<AssetHandle<T>>` (`std::expected<…, AssetLoadError>` —
  branch on `AssetError::Kind`, not a string).
- **`AssetManager::MountMemory(vector<u8>, string) → MountHandle`** shadow-mounts an
  **in-memory archive** over the on-disk mounts: a later resolve of an `AssetId` the
  in-memory archive carries hits it first. The returned `MountHandle` is **RAII** — drop
  it to unmount and reveal the underlying mount again. The cook-on-demand loop uses it:
  the editor cooks a source into a scratch archive in memory, mounts it, reloads the
  handle behind it, and replaces the `MountHandle` on the next recook.
- **`AssetManager` is owned by `Application` and constructed with a `Context&`,
  a `TaskSystem&`, and the borrowed `TypeRegistry&`** (the registry the prefab
  loader reflects components through). The async `Load` is the obvious call and the
  non-stalling one; `LoadSync` is the marked-verbose blocking spelling for tests,
  tools, and the smoke path.
- **The cooker loads the game module to reflect its types.** `vengc cook --module
  <lib>` `dlopen`s the game module and reflects its component types into a
  `TypeRegistry` (reusing `ModuleLoader`, ABI-version check included), so the
  `PrefabImporter` validates a prefab's components against the **real** reflected
  descriptors — an unknown component, a wrong field type, or a malformed value is a
  located cook-time error, the way the material importer validates `*.vmat.json`
  against a shader's reflected parameters. A field absent from the source keeps
  its default-constructed value (schema tolerance), so omission is allowed,
  type-mismatch is not. `vengc generate-type-id` mints a collision-free `TypeId`
  (the `TypeId` analogue of `generate-id`) and `vengc` can emit a type manifest.
- **Type registration is GPU-free, by contract.** `RegisterBuiltinTypes(TypeRegistry&)`
  (public, in `Veng/Scene/BuiltinTypes.h`), `Register<T>()`, and a module's
  `VengModuleRegister` (the `Application` factory + the type registration) touch
  **no** `Context`/device — the headless cooker reflects a module's types with no
  ICD present, and a no-device cooker test pins the contract. The **host** (launcher
  or cooker) owns the `TypeRegistry`: it constructs it, pre-registers the builtins,
  puts it in the `VengModuleHost` as `Types`, calls `VengModuleRegister` (at which
  point the module registers its component types), and threads it onward.
- **Build-order edge: `add_asset_pack(... MODULE <lib>)`.** A pack containing
  prefabs names its game module; the build graph grows a `lib → cook → bundle` edge
  so the pack cooks after its lib is built. Packs without prefabs stay
  module-independent. `veng_add_game` wires the example's prefab pack to depend on
  `libhello_triangle`.

The same split runs underneath at the resource level: `Buffer/Image::Upload`
(taking a `TaskSystem&`) is **async by default** — it returns a `Task<void>`,
records the copy on the transfer queue, and never blocks — while `UploadSync`
is the blocking path (host memcpy + `WaitIdle`) the sync loaders, tests, and
smoke render use.
- **`AssetHandle<T>` is refcounted indirection into the manager's cache**, not a
  `Ref` to a GPU resource (see the root CLAUDE.md ownership rule). Apps drop their handles in
  `OnDispose()` like any other engine resource; `CollectGarbage()` evicts entries
  no handle references, retiring their GPU resources through the per-frame
  deferred-destruction path. **`AssetManager::Adopt<T>(Ref<T>)`** wraps an
  already-resident, runtime-created resource in an `AssetHandle<T>` so it is usable
  everywhere a cooked, `AssetId`-loaded handle is. The adopted handle carries the
  invalid `AssetId` (`Id().IsValid() == false`), and its cache entry is **detached**
  — never inserted into the `AssetId` map, so `CollectGarbage()` ignores it; it
  stays alive exactly as long as a handle references it. A reflective serializer
  records the invalid id as "no asset", so a runtime resource is not a persistable
  content reference.
- **A `Mesh` can also be built at runtime, no cooker.** `Primitives::Cube` /
  `Plane` / `Sphere` (`Veng/Asset/Primitives.h`) generate CPU-side `MeshData`
  (canonical-layout vertices + `u32` indices + a resident material list + an
  indexed submesh table) with analytic normals/tangents/UVs and an optional
  `AssetHandle<Material>`; `Mesh::BuildSync(Renderer::Context&, const MeshData&,
  const string&)` uploads that into a resident `Ref<Mesh>` via the blocking
  `UploadSync` (its async sibling `AssetManager::Build<Mesh>` streams the same geometry
  in off the render thread). A runtime primitive is **not** an `AssetId`-addressable asset and
  never touches an archive — it is owned by whoever calls the factory and retires
  through the per-frame deferred-destruction path like any other `Mesh`. It is
  interchangeable with a cooked mesh at every pipeline and draw call, both being in
  the canonical layout (`Mesh::CanonicalLayout()`), and `AssetManager::Adopt`
  wraps its `Ref<Mesh>` in an (id-less) `AssetHandle<Mesh>` so it is equally usable
  anywhere a cooked handle is — e.g. a `MeshRenderer`.
- **A primitive is also a persistable, prefab-authored component.** A
  `Primitive` (`Veng/Scene/Components.h`) stores the *recipe* of a procedural
  mesh — a `PrimitiveShapeVariant` (`Variant<CubeShape, PlaneShape, SphereShape,
  IcosphereShape>`, each alternative carrying that shape's parameters plus an
  `AssetHandle<Material>`) — so a prefab persists "icosphere, radius 0.8, 4
  subdivisions, brick material" rather than an unaddressable runtime handle. The
  recipe cooks and loads through the ordinary prefab path; the embedded material in the
  active alternative resolves as an ordinary load-time dependency. The recipe becomes a
  renderable mesh **automatically at spawn**: `Primitive` declares a
  spawn-resolve thunk (`VE_RESOLVE` in `Veng/Scene/Components.h`, the resolver body in
  `Veng/Scene/Resolve.{h,cpp}`), and `Prefab::SpawnInto` fires it after populating the
  component. The resolver calls `BuildPrimitiveMesh(AssetManager&, const
  PrimitiveShapeVariant&) → AssetHandle<Mesh>` — which builds the active shape's CPU
  geometry (`BuildShapeMeshData`) and streams it in via `AssetManager::Build<Mesh>` —
  then adds (if absent) and sets the entity's `MeshRenderer.Mesh`, so the primitive
  **appears** a few frames after spawn exactly as a cooked mesh would (the renderer
  skips a not-yet-resident mesh). There is no caller-driven resolve pass and no dedup
  cache: identical recipes build independent meshes, and a consumer wanting N entities
  on one mesh calls `BuildPrimitiveMesh` once and assigns the shared handle N times.
  The hand-built `Primitives::`/`Adopt` path above stays public for tests and tools.
- **A mesh owns its materials; submeshes index them.** A `Mesh` holds a resident
  `vector<AssetHandle<Material>>` (`GetMaterials()`) and each `SubMesh` carries a
  `u32 MaterialIndex` into it (`SubMesh::NoMaterial` = unassigned). The cooked
  on-disk mesh format stores u64 material ids; `MeshLoader` eager-resolves those
  ids into material instances and builds the list, exactly as `Material` resolves
  its own texture/shader dependencies — so every asset eager-loads its
  dependencies. A draw iterates submeshes, binding `GetMaterials()[MaterialIndex]`
  per range.
- **Skinned meshes carry a skeleton and animate through GPU skinning.** A `Mesh` with a
  `SkeletonId` is **skinned** (`Mesh::IsSkinned()`): its vertices use the skinned layout
  (`Mesh::SkinnedLayout()` — canonical attributes plus `RGBA16Uint` bone indices +
  `RGBA32Sfloat` weights) and it eager-resolves an `AssetHandle<Skeleton>` (`Skeleton` and
  `Animation` are CPU-only assets, loaded by `AssetId` like any other, no GPU resource). An
  **`Animator`** component (`AssetHandle<Animation>` + time/speed/loop/playing) plays a clip;
  the View-phase **`AnimationSystem`** samples it against the mesh's `Skeleton` each tick into
  a transient **`SkinnedPose`** component (the bone palette, `Skeleton::ComputeSkinningMatrices`
  = `GlobalInverse · modelBone · inverseBind`). The `SceneRenderer` splits its g-buffer draw
  plan into a static path (the existing GPU-driven-cull pipeline) and a **skinned path**: a
  second pipeline built from the core `surface_skinned.vert` (4-influence linear-blend skinning)
  drawn CPU-direct, reading a per-instance **skinning palette** SSBO (ring-buffered, **set 2**;
  `DrawData.PaletteBase` is each instance's offset). The directional `ShadowScenePass` and the
  `PunctualShadowScenePass` both cast a skinned caster's posed shadow through a parallel
  `shadow_depth_skinned.vert` + the palette at set 1, and `surface_skinned.vert` skins both the
  current and previous position (the latter through the previous-frame palette base the renderer
  tracks, valid because the palette is ring-buffered) so a skinned mesh's deformation writes its
  motion vector into the g-buffer velocity channel. An entity with no `SkinnedPose` (e.g. the
  editor with systems paused) renders at the skeleton's bind pose. The core pack ships the
  `skinned` vertex layout and the skinned surface/shadow vertex shaders.
- **Cooked prefabs load like every other asset; a `Scene` is what you spawn into.**
  A `*.prefab.json` (entities + components + field values) cooks into an
  `AssetType::Prefab` blob and loads through the **identical**
  `AssetManager::Load`/`LoadSync` path — a cached `AssetHandle<Prefab>` whose embedded
  asset references (a `MeshRenderer`'s mesh, a `Material`, …) are resolved as ordinary
  load-time dependencies, exactly as a `Material` resolves its textures and shaders.
  The cooked blob **is** the reflection serializer's name-keyed `WriteFields` record
  encoding, per component, wrapped in an entity/component table — not a new format.
  A `Scene` is an engine primitive, **never loaded**; you create one and spawn into it:
  `Prefab::SpawnInto(Scene&, AssetManager&) const → vector<Entity>` (the spawned roots)
  creates the entities, `ReadFields` each component, remaps intra-prefab `Entity`
  reference fields to the fresh handles, and rehydrates the embedded `AssetHandle`
  fields. Spawning the same prefab twice spawns two independent copies — a prefab is a
  reusable recipe, not a singleton. `SpawnInto` lives on `Prefab`, so the dependency
  points asset → primitive; the `Scene` primitive gains no asset-system dependency.
  After populating every spawned entity, `SpawnInto` runs a post-populate resolve pass
  that fires each component's spawn-resolve thunk, so a recipe component's derived
  resource streams in with no caller intervention.
- **A component can carry a recipe whose resources resolve at spawn.** `TypeInfo` holds
  an optional `SpawnResolve` thunk (`void (*)(void* component, Scene&, Entity,
  AssetManager&)`) — a component opts in via the `VE_RESOLVE` macro (keyed off a separate
  `VengResolver<T>` trait, wired through `RegisterImpl`'s `if constexpr` like the variant
  thunks) and writes its resolver fully typed; the macro generates the `void*`→`T*`
  trampoline. The typedef names `Scene`/`AssetManager`/`Entity` through forward
  declarations in `TypeRegistry.h` (no upward include — the one layering compromise, so
  `include_hygiene` stays green), and its parameter set is frozen; anything more is
  reached through `Scene`/`AssetManager`. `Prefab::SpawnInto` fires every resolver-bearing
  component after the spawn populates the subtree, and the public
  `ResolveComponents(Scene&, Entity, AssetManager&)` (`Veng/Scene/Resolve.h`) is the
  single resolve code path a runtime caller uses after adding or editing such a
  component. `Primitive` is the first rider; any future resolve-time component
  (a mesh-baking spline, a buffer-allocating emitter) reuses the same seam.

## Shaders & materials

The offline shader compile + reflection and `.vmat` validation run in the cooker
([cooker/CLAUDE.md](../cooker/CLAUDE.md)); this section covers the runtime material.

Shaders are a first-class asset authored in **Slang** — a `*.shader.json` names
its `.slang` source, entry point, and optional vertex-layout `AssetId`. The
cooker **always** compiles from source (there is no precompiled-inline path) and
**reflects the shader offline** into a serializable `ShaderInterface`; the engine
loads plain **SPIR-V** and gains no Slang dependency. (There is no `glslc` /
`add_shaders` path — GLSL was removed project-wide.)

A material (`*.vmat.json`) references its vertex/fragment shaders by `AssetId` and
declares an **ordered, explicitly-typed** field list; the cook validates those
fields against the fragment shader's reflected parameters. At runtime a `Material` is
thin — shader handle + texture **handles** + one parameter-block entry, bound through
set 0 — and `Material::Bind` pushes its per-draw material selector.

A material's GPU parameters are **one reflection-sized block** per material (set 0
binding 4, byte-addressed at `index * MaterialParamStride`): its bindless handle slots
(`uint` members the loader patches with the resolved index) and its authored
scalar/vector params share that single block, laid out by reflection at each field's
offset. There is no fixed engine struct and no second SSBO — a material declares an
arbitrary, shader-defined handle set (zero, one, or several), and `CookedMaterialField::Kind`
(handle vs. param) is the seam the loader patches by offset. `CookedMaterialHeader` carries
`Version` (`CookedMaterialVersion`, currently `2`), `Domain`, and `BlockBytes`; a stale blob
rejects loudly. `Material::GetFields()` exposes the reflected `MaterialField` table — the
editor's parameter-schema source, so the node editor reads a material's authorable parameters
with no Slang in `libveng_editor`.

The block buffer is **N-buffered for frames-in-flight, host-visible + persistently
mapped**: it holds `framesInFlight` copies of the `MaxMaterials * MaterialParamStride`
table, and each frame-in-flight owns one region. `Register/UpdateMaterial` mark a
material dirty for `framesInFlight` frames and write only the *current* frame's region
(safe because that frame is not yet submitted); `OnFrameAcquired` flushes each
still-dirty material into the region it just made current. A per-frame `SetParam` /
`SetTexture` is therefore a direct, stall-free write — no staging, no `WaitIdle`, no
hazard. **The current frame's region is selected by folding the frame base
(`currentFrame * MaxMaterials`, via `BindlessRegistry::GetCurrentFrameBase()`) into the
pushed material selector index in `Material::Bind`** — not by a dynamic descriptor
offset: a `STORAGE_BUFFER_DYNAMIC` descriptor mistranslates inside set 0's bindless Metal
argument buffer on MoltenVK. The buffer stays a plain storage buffer bound at full range,
and the shader's `index * MaterialParamStride` load is unchanged.

A `Material` carries a first-class **`MaterialDomain`** (`Surface` / `PostProcess`,
`Veng/Asset/Material.h`) selecting its output contract, pipeline shape, standard vertex
shader, and invocation site — the parameter schema, bindless handles, `.vmat` authoring,
and editor inspector are shared across domains. `Surface` is the existing opaque path made
explicit (canonical-layout vertex stage, g-buffer MRT output, drawn per-submesh by the
geometry pass); `PostProcess` is the fullscreen path (screenspace vertex stage, a single
`SV_Target0` color, invoked by the post chain). The lowercase `"domain"` `.vmat.json` key
selects it (default `surface`), and the cook validates the fragment shader's outputs against
the domain's contract (Surface → g-buffer MRT `SV_Target0`+`SV_Target1`; PostProcess → a
single `SV_Target0`). The per-draw selector push offset is domain-keyed — Surface 64 (after
the MVP block), PostProcess 0.

The engine ships the **standard vertex shader per domain in the core pack**: `surface.vert`
(canonical layout) and `fullscreen.vert` (screenspace); `material.slang` holds the shared
bindless/material/push-block declarations. A game references the core `surface.vert` rather
than shipping its own surface vertex stage.

## Scene & ECS

A `Scene` is a runtime **ECS world**: a generational `Entity` handle
(`{ u32 Index; u32 Generation; }`, `Entity::Null` empty) over a `TypeId`-keyed,
type-erased **sparse-set** per-component storage, with templated
`Add`/`Remove`/`Get`/`TryGet`/`Has` and the multi-component queries `View<Ts...>`
(range-for, yields `(Entity, Ts&...)`, supports `break`) and `Each<Ts...>`. A query
drives off the smallest participating pool. **Structural changes during iteration are
illegal** — adding/removing components or destroying entities mid-`View`/`Each` is API
misuse; the single-threaded model offers no re-entrancy guard. A stale `Entity` (its
slot recycled, generation bumped) accessed through the API is a fatal `VE_ASSERT`, not
silent UB. `DestroyEntity` is **recursive** — it walks the `Hierarchy` `FirstChild` →
`NextSibling` links to destroy the entity's whole subtree in **O(subtree)**, detaching the
destroyed root from any surviving parent's child list first so no sibling is left dangling.
A **component is just a reflected type a `Scene` pools** — pools are made lazily on first
`Add` of a type; there is no separate component-id space.

The scene hierarchy is an intrusive, sibling-linked **`Hierarchy`** component: a `Parent`
up-edge plus a doubly-linked, ordered child list (`FirstChild`/`PrevSibling`/`NextSibling`).
Topology is mutated **only** through `Scene` operations — `SetParent(child, parent)` (detach
from the old list, append under the new in O(1); `Entity::Null` parent re-parents to root),
`Detach(child)`, and `MoveBefore(child, sibling)` (the editor's drag-reorder / insert-at) —
which maintain all four links as a set and bump the spatial version. `GetParent(entity)` and
`ForEachChild(entity, fn)` (forward, insertion order) are the read side. A cycle (a descendant
adopting an ancestor) is API misuse and a fatal `VE_ASSERT`. Only `Parent` is a reflected,
persisted field; the three list links are derived and rebuilt on prefab spawn, so the
serializer and cooker never touch them.

A `Scene` carries a monotonic **spatial version counter** (`GetSpatialVersion()`): it bumps
on any change to a **spatial pool** (`Transform`/`Hierarchy`/`MeshRenderer`) — a structural
`Add`/`Remove`, a `DestroyEntity` touching one, a **non-`const`** access (the mutable
`Get`/`View`/`Each` path, a potential in-place edit), or a `ForEachComponent` visit (the
editor inspector's erased-`void*` edit path). A **`const`** `View`/`Each` does **not** bump it,
so a read-only consumer iterates without forcing a version move. This is the access-as-write
change-tick a consumer (the `SceneBroadphase`) gates its tree rebuild on: it caches the version
it last built against and rebuilds only when the version moved. One constraint: a `Transform&`
retained across frames and written without re-acquiring it bypasses the bump — write transforms
through the scene accessors each frame, as all engine and sample code does.
`Scene::ForEachComponent(Entity, const function<void(TypeId, void*)>&)` iterates every
pool that holds the entity, calling the visitor with each component's `TypeId` and an
erased pointer — the type-agnostic enumeration the editor inspector walks (templated
`Get`/`Has` need the type at compile time; this does not).

Types register into the **`TypeRegistry`** — **host-owned, borrowed by the engine**:
the host (launcher or cooker) constructs it, pre-registers the builtins, fills it via
`VengModuleRegister`, and threads it in; `Application` borrows a `TypeRegistry&`
(`Application::GetTypeRegistry()`) and threads it into the `AssetManager` and into
`Scene::Create(TypeRegistry&)`. It is generic over *any* reflected
type — leaf field types, nested structs, and components share **one `TypeId` space**.
Each type carries a stable `u64` **`TypeId` authored exactly like an `AssetId`**:
hardcoded `0x…ULL` for engine types, `vengc generate-id` for game types, hex in C++ /
decimal in JSON. It is a compile-time constant (`TypeIdOf<T>()` reads it off a trait,
independent of registration order), persisted directly (a scene stores a component's
`TypeId`, never its name), and byte-identical across the module boundary (so the
cooker reflecting a module reads the same ids the runtime does); two
types claiming one id is a **fatal collision assert**. **Every reflect-macro site must spell
its type fully qualified** (a leading `::`) — a hard rule the macros enforce with a
`static_assert` (`Detail::IsFullyQualifiedSpelling`; a fundamental type like `bool`, which has
no namespace and cannot be `::`-prefixed, is the sole exception) — so the registry captures the
namespace for every type. `SplitQualifiedTypeName` splits the authored spelling into the bare
`TypeInfo.Name`, its `TypeInfo.Namespace` (e.g. `{ "vec3", "Veng" }`), and the joined
`TypeInfo.QualifiedName` (`"Veng::vec3"`, or just the bare name when global). Name/Namespace
are logs/editor display only — the editor shows them as `Name (Namespace)` with the namespace
de-emphasized (`Veng::UI::TypeLabel`); `QualifiedName` is the single key **all** type-name
matching is done against (`TypeNameMatches`, strict). A game registers its own types through
the same path as the
builtins — a **`VE_REFLECT`** describe-block next to the struct, read back by the
zero-arg `Register<T>()` (a referenced type's schema auto-registers from its trait on
first reference, so there is no registration-ordering burden). A **leaf or enum** type
is authored with **`VE_LEAF(Type, 0x…ULL, FieldClass::Kind)`** and a **fieldless
component** with **`VE_TYPE`** — all three macros specialise the **single
`VengReflect<T>`** identity trait with a uniform member set, so `TypeIdOf<T>()` /
`FieldClassOf<T>()` read it directly and the registry has one `Register<T>()` path (no
separate leaf registration).

The reflection layer (`Veng/Reflection/`): an **open** `TypeId` space (a game adds a
leaf/struct/component with no engine change — a leaf or enum through the `VE_LEAF` seam)
and a **closed** `FieldClass`
(`Scalar`/`Vector`/`Quaternion`/`Matrix`/`String`/`AssetHandle`/`Reference`/`Struct`/
`Enum`/`Variant`) a generic walker switches on. **`FieldClass::Variant`** is the
tagged-union meta-kind: a `Variant<Ts...>` field (`Veng/Reflection/Variant.h`,
authored with `VE_VARIANT`) holds one of several registered struct alternatives, and
reflection reaches its active member only through type-erased thunks on the variant's
`TypeInfo` (`VariantActiveType`/`VariantActivePtr`/`VariantSetActive`/…), never by
offset. It **serializes as a `TypeId` tag** (the active alternative's id, `InvalidTypeId`
for empty) followed by that member's record; an unknown or unregistered tag leaves the
variant empty and skips the record, the same schema-drift tolerance an unknown field
name gets. It is **authored in JSON as `{ "type": <registered name>, "value": {…} }`**,
the cooker matching `"type"` against an alternative's fully-qualified `QualifiedName`
(`TypeNameMatches`, strict). The
prefab loader's dependency walk and `Prefab::SpawnInto`'s rehydration both recurse into
the active alternative, so an embedded `AssetHandle` inside a variant (a shape's
material) loads as an ordinary dependency. `FieldDescriptor`s — authored via
`VE_REFLECT`/`VE_FIELD`, each deriving its `Offset` (`offsetof`) and its field type's
`TypeId`/`FieldClass` at compile time, restating only the field *name* — drive a
tolerant, name-keyed, recursive generic serialization (and, later, editor inspectors).
A `FieldDescriptor` additionally carries **optional editor metadata** (`DisplayName`,
`Tooltip`, `Min`/`Max`/`Step`, `Hidden`, `ReadOnly`, `Category`) that the serializer
**ignores** — it reads only `Name`/`Type`/`Offset`. The serialization key (`Name`) is
kept distinct from the UI label (`DisplayName`), so relabelling never breaks on-disk
compatibility: **on-disk type identity is the `TypeId`, field identity is the name**.

The builtins are plain reflected components, pre-registered identically to a game's
own: `Name` (a display label), `Transform` (**local** TRS — `Position`/`Rotation`/
`Scale`, never a world matrix), `Hierarchy` (the intrusive scene-graph link — a `Parent`
up-edge plus the ordered child list, mutated through `SetParent`/`Detach`/`MoveBefore`;
`WorldMatrix`/`ComputeWorldMatrices` walk the `Parent` edge as `parent.world * local`,
recomputed on demand with no dirty-flag cache), `Camera` (the component whose
FovY/Near/Far and world transform build a `CameraView`, the value type carrying the
view/projection — Y flipped for Vulkan clip space), `MeshRenderer` (holds the
`AssetHandle<Mesh>` a draw queries — the mesh owns its materials, so a renderer queries
`(Transform, MeshRenderer)` and draws each submesh with its material), `Animator` (plays an
`AssetHandle<Animation>` on a skinned-mesh entity; the `AnimationSystem` writes the result into
a transient `SkinnedPose` the renderer uploads), and `Light` (a
directional light — `Direction`/`Color`/`Intensity`; `SceneRenderer::Execute` selects
the first `Light` entity into the `SceneView`, or a zero-intensity default → flat
ambient when the scene has none).

A `Scene` reduces to a world-space bound on demand: `SceneBounds(scene)`
(`Veng/Scene/Transforms.h`) unions every resident `(Transform, MeshRenderer)` entity's
world-space mesh bound, reusing `ComputeWorldMatrices`' one amortized pass — recompute-on-
demand, no dirty-flag cache. `GatherMeshes` (`Veng/Scene/Visibility.h`) is the pure one-shot
candidate gather over the same pass (world matrix + world-space `AABB` + resident mesh per
entity); the `SceneBroadphase` caches that gather and builds the BVH from it, re-gathering only
when the scene's spatial version moves (or a still-loading mesh becomes resident). The **BVH
broadphase, per-submesh leaves, and GPU-driven hi-Z occlusion culling are delivered**; incremental
tree maintenance, meshlet/cluster-granularity GPU culling, two-pass occlusion, and GPU-driven
shadow-caster culling are its refinements. Each `Mesh` carries a local-space `GetBounds()`
derived from its canonical vertex positions at load (no cooked-format change), and each `SubMesh`
a local-space `AABB` folded over its index range — the per-submesh leaf granularity the cull
operates at. Both build on `AABB`
(`Veng/Math/AABB.h`), the engine's glm-only bounds primitive — a min/max `vec3` pair with
the union/expand/center/extents/corners/transform algebra and an empty sentinel. `Frustum`
(`Veng/Math/Frustum.h`) is its visibility companion — six bounding planes extracted
Gribb-Hartmann from a view-projection matrix (Vulkan ZO clip), with a conservative
`Intersects(Frustum, AABB)` p-vertex test (never a false cull).

A `Scene` is **`Unique`, single-owner** — nothing holds a `Ref` to it; the app owns it
and a renderer reads it per frame as a `const Scene&`. Drop it in `OnDispose()` like
any other engine resource. The `TypeRegistry` it was created with must outlive it and
must already have every component type registered.

## Gameplay: cameras, control, systems, game modes, levels

The gameplay layer is built from ECS primitives — components marking entities plus
systems acting on them, never controller/manager/mode objects — shaped for veng's
data-oriented grain and for the networking it anticipates. A task-oriented tutorial for
writing it lives in [`docs/guides/`](../docs/README.md)
([writing gameplay systems](../docs/guides/writing-gameplay-systems.md),
[wiring a level](../docs/guides/wiring-a-level.md)); this section is the architecture.

**Camera is selected per seat and resolved to a `CameraView`.** A camera is
`(Transform, Camera)` data; a **`Viewer { Entity Camera }`** component is a *seat* (a
local player, a render target, the editor viewport) naming the camera entity it renders
through, separating seat from camera. Two pure helpers beside `MakeCameraView`
(`Veng/Scene/Camera.h`) resolve a seat to the view the renderer consumes:
**`ResolveCameraView(const Scene&, Entity viewer, f32 aspect) → optional<CameraView>`**
reads the seat's `Viewer`, looks up its `Camera` entity, and projects through its
`WorldMatrix` (so a camera parented under a rig resolves correctly);
**`ResolvePrimaryCameraView(const Scene&, f32 aspect)`** is the one-seat convenience —
first `Viewer`, else the first bare `(Transform, Camera)` entity. **Aspect is a
render-target property, never a `Camera` field** — the caller passes it (output extent
in the runtime, panel extent in the editor). The renderer is untouched: it still consumes
a resolved `CameraView` through `SceneView`, so the runtime's in-scene camera and the
editor's external orbit `EditorCamera` (its own non-ECS camera, `editor/src/EditorCamera.h`)
coexist with zero renderer change. The prefab editor's Play mode can preview the scene's
authored `Viewer` camera through `ResolvePrimaryCameraView` instead of its orbit camera.

**Control flows Input → Intent → Movement.** Device/wire input is captured into a
per-player **`PlayerInput`** snapshot (move/look axes + a button bitset); a **control**
system translates it into an abstract **`Intent`** command (local-frame move, look delta,
action bitset); a **movement** system (`MovementSystem`, `Veng/Scene/Movement.h`) and
gameplay systems generally consume `Intent` and mutate state, scaled per pawn by an
optional **`Mover`**. The `Intent` indirection is deliberate: it is the serializable
chokepoint a net layer predicts and rolls back, and the uniform interface AI and remote
players write through — both are drop-in `Intent` producers, no movement change. **A
`Possesses { Entity Pawn }`** link names the pawn a seat controls; possession is
independent of `Viewer.Camera` (a spectator views without possessing; a cutscene
retargets the camera without un-possessing).

**The tick is split Sim / View, and entities carry `Authority`.** A `SceneSystem`
(`Veng/Scene/SceneSystem.h`) declares a **`Phase { Sim, View }`** (default `Sim`); a
**`SceneSimulation`** (`Veng/Scene/SceneSimulation.h`) runs all Sim systems, then all View
systems, each tick — so a View system reads the state the Sim phase finalized this tick.
**Sim** is the deterministic, replicable simulation (control, movement, rule systems); a
headless Sim tick is a pure function of state + intents (the `SystemContext.Input` service
is always present, reporting the neutral all-zeros state in headless, so an input-reading
system needs no guard). **View** is client-local presentation derived from finalized Sim
state — the `CameraRigSystem` (`Veng/Scene/CameraRig.h`) trails a possessed pawn via a
`CameraFollow` component, never authoritative and never on the wire. An **`Authority {
Tier, Owner }`** component (`Tier { Server, Local }`) marks who simulates an entity;
authored entities default `Server`, client-local view entities `Local`. Nothing in the
runtime reads it — its consumer is the future net layer — but threading ownership now is
the one annotation painful to retrofit across every spawn site later. The two-pass split
is the whole scheduling mechanism: no dependency graph, no parallelism.

**A game mode is a `Session` state component + rule systems — no object, no registry.** A
**`Session { Phase, Elapsed, Score }`** component (`SessionPhase { NotStarted, Playing,
Ended }`) holds the mode's server-authoritative state on a well-known session entity; a
**`GameModeConfig`** beside it names the player prefab and mode parameters (its JSON key is
`"gameMode"`). The "mode" is a *selectable set of rule systems* (spawn-on-start, scoring,
win-condition) reading and writing the `Session`; begin/end-play is the systems'
`OnStart`/`OnStop` plus `SessionPhase` transitions. Selecting a mode is choosing a config
plus a registered rule set — no C++ path picks it, no `GameModeRegistry`, no ABI bump.

**Systems are a catalog; selection and order are level data; config is components.** A
`SceneSystem` declares a stable **`SystemId`** (a `u64` id space alongside `AssetId`/`TypeId`,
minted with `vengc generate-id`) + a display name through the **`VE_SYSTEM(Type, 0x…ULL,
"Name")`** trait macro — the system analogue of `VE_REFLECT`'s identity. The host-owned
**`SystemRegistry`** (`Veng/Scene/SystemRegistry.h`, mirroring the `TypeRegistry`: the host
constructs it, the module fills it through `VengModuleRegister`, `Application` borrows it)
stores `{ SystemId, Name, factory }`, **enumerates the catalog** without instantiating
anything, resolves an id, and fatally rejects a duplicate id. Registration is GPU-free
(building a system touches no `Context`/device), preserving the headless/cooker contract. A
`SceneSimulation` is built either from an **ordered `SystemId` set** selecting catalog
entries (run in that order, honoring the phase split) or from the whole registry as the "all
registered" convenience. A system's **parameters are authored as components** — a settings
entity the system reads — reusing the entire reflection inspector and keeping systems pure
logic; there is no reflected-system-config mechanism.

**A `Level` is the authored wiring artifact — a thin wrapper by reference.** A **`Level`**
asset (`AssetType::Level`, `Veng/Asset/Level.h`) does not embed world entities: it
*references* a **world prefab** by `AssetId` and adds the data that is not reusable-recipe
data — the ordered active `SystemId` set, the `GameModeConfig`, and a tolerant
**`LevelRenderSettings`** subset (exposure, bloom, shadow toggles the app maps onto its
`SceneRendererSettings`/`SceneView`). This resolves the prefab dual-purpose tension by
*reusing* prefab serialization, not embedding a second copy: a prefab is a reusable recipe
again, the `Level` is the once-loaded playable unit (named `Level`, not `Scene`, to avoid
colliding with the runtime `Scene`). It is CPU data with no GPU resource, loaded through the
ordinary `AssetManager::Load`/`LoadSync` path; its world prefab and that prefab's embedded
asset refs resolve as ordinary load-time dependencies. **`Level::LoadInto(AssetManager&,
const SystemRegistry&) → LevelInstance`** is what *starts the game*: it spawns the world
prefab into a fresh `Scene`, builds a `SceneSimulation` from the level's `SystemId` set
against the catalog, and seeds a `Session` entity from the game-mode config, returning a
`LevelInstance { Unique<Scene> World; Unique<SceneSimulation> Simulation; }` the app ticks
and renders. A game is assembled as authored data, not hand-spawned in `main.cpp`.
