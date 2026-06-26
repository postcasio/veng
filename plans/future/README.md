# future — work beyond the current plansets (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> ahead. Each area below becomes its **own planset** when taken up and detailed
> planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

Numbered for stable cross-references. **Delivered** areas are documented in their
plansets (see [plans/README.md](../README.md)) and are *not* re-narrated here —
only their **still-future remainder** is kept, plus any delivered capability a
pending area builds on directly. The substance of this document is the
**remaining** work: **material domains + shader-graph codegen (area 13)**, the
editor's scene editor (area 6, sub-area D), **engine-owned material shader header +
cross-pack Slang includes (area 14)**, **multi-seat input routing + networking (area 4),
now that the event-routed input core is delivered** — and the named still-future increments
of the areas done in part (areas 1, 2, 7, 8, 9, 10, 12).

### 1. Asset system — remaining: hot-reload

Delivered by planset-5 + planset-6. **Still future — hot-reload (`Reload`).** Its
re-upload half is exactly what the async path delivers, but its re-cook half
conflicts with offline-only cooking, so it needs a dev-only watcher design.
**Design overview:** [asset-system.md](asset-system.md).

### 2. Threading / task system — remaining: task graph, staging pool, cancellation

Delivered by planset-6. **Still future:** a task *graph* (inter-job dependencies),
staging-buffer pooling, and cancellation. **Design overview:**
[threading-task-system.md](threading-task-system.md).

### 3. De-globalize the rendering context — DONE (planset-4)

Delivered; nothing pending builds on it. `Context::Instance()` / `s_Instance` are
gone — every resource `Create` takes an explicit `Context&`.

### 4. Event & input systems

**Event routing landed; networking remains.** Gameplay drives the requirements (area 7,
planset-29). The **event-routed input core is delivered**: the `Window` is the single event
source (typed `Event` queue), an **`InputRouter`** routes each event to consumers by a
**focus stack** (ImGui forwarded to under UI focus, the `Input` snapshot fed always, gameplay
focus making the running game the exclusive owner with the cursor captured), and `Input` is an
event-fed snapshot rather than a global poller. The editor's Play and the shipped sample push
gameplay focus to own input; Shift+Esc (or window-focus loss) releases it. Today single-player
still runs on **one `Veng::Input` → one `PlayerInput`**, the `Intent` chokepoint and Sim/View
split are in place, and `Authority` is threaded — but nothing yet routes input *per seat* or
replicates state. Two coupled bodies of work sit behind those seams:

- **Multi-seat input routing.** Split-screen and AI-vs-player need input routed *per
  `Viewer`/player* into the right `PlayerInput`, rather than one device feeding one
  snapshot. The components are already written so this layer is additive — it routes into
  the existing `PlayerInput`/`Possesses` seats, no gameplay rewrite. **The *pointer*-routing
  seam is delivered (planset-31):** a `Viewport` owns its region and exposes
  `WindowToViewport`/`ScreenToWorldRay`, so a router hit-tests a click against each
  registered viewport's region to find the seat it belongs to (the editor already uses the
  fraction/ray for picking). The remaining half is *device*-keyed routing — fanning gamepad
  (and other per-device) events into the right `PlayerInput` by device id, which is
  independent of viewports entirely. The two compose: pointer events route by region,
  device events by id, both into the existing seats.
- **The networking layer.** A net layer consumes the seams planset-29 established for it —
  it serializes/predicts/rolls back `Intent`, replicates `Session`/pawn state by
  `Authority`, and derives the View phase locally on each client. An ECS-native net model
  (state + input replication, à la the ECS-netcode lineage) slots in behind the
  Intent/Authority/Sim-View structure without dismantling anything, since none of those
  seams reproduces an actor-network object graph.

Multi-seat routing builds directly on the delivered `InputRouter`: it already owns the
event→consumer routing seam, so per-`Viewer` routing extends it to fan one device (or several)
into the right `PlayerInput` rather than the single shared snapshot.

### 5. Unit testing / test infrastructure — DONE (planset-3 + planset-4)

Delivered; nothing pending builds on it. doctest + CTest wiring, the
separate-process death harness, the in-process `veng_gpu` integration suite, and a
local `ctest -L validation` gate. CI with a hosted software ICD was explicitly
descoped.

### 6. Editor application

The authoring environment, spanning several plansets. **Design overview:**
[editor.md](editor.md), with the build-model prerequisite in
[game-module.md](game-module.md). Delivered sub-areas — the scene editor builds on
all three:

- **Sub-area A — game = shared lib + launcher (planset-9).** `libgame` + a thin
  launcher that `dlopen`s one C-ABI `VengModuleRegister`; the ABI carries an
  `EditorRegistry*` slot the launcher leaves null and the editor host makes
  non-null.
- **Sub-area B — editor shell + framework (planset-14).** `libveng_editor`
  (`EditorPanel`, `EditorRegistry`, the docking `EditorHost`), the `veng_add_editor`
  macro, the reflection-driven inspector (`Scene::ForEachComponent` → a widget per
  `FieldClass`, overrides via `RegisterFieldWidget`), cook-on-demand (`libveng_cook`
  in the editor exe only → injected `CookBackend` → `MountMemory` hot-reload), and
  the texture editor. **The editor's render-owning panels moved onto the engine's
  central viewport drive-list (planset-31):** a panel holds a registered `Offscreen`
  `Viewport`, the engine renders it each frame, and `EditorHost` runs the engine
  render tail instead of a hand-rolled present path — `EditorPanel` no longer carries
  a scene-render seam.
- **Sub-area C — material node editor (planset-15).** The generic, device-free
  **`VengEditor/NodeGraph/`** surface (topology core + data-driven `NodeType`/
  `NodeCatalog` + JSON (de)serialization) — **reused by sub-area D** — its material
  specialization, the `MaterialEditorPanel`, and the reusable `MaterialPreview`.

**Sub-area D — scene editor — STILL FUTURE (the only remaining editor sub-area).**
Viewport panel (reuses the `SceneRenderer`, area 8), hierarchy panel, gizmos
(ImGuizmo or hand-rolled), save round-trip to `.prefab.json`. **All its gates are
met:** the runtime scene model (area 7, planset-10), the cooked prefab + module
reflection (area 10, planset-11), the inspector + cook-on-demand foundation
(sub-area B, planset-14), and the reusable `VengEditor/NodeGraph/` surface
(sub-area C, planset-15). Its native-type inspectors reuse area 10's module
reflection rather than re-introducing it; its viewport renders one `Scene` through
N `SceneRenderer` instances with no API change. A planset of its own.

Also still future: **installing `veng_add_game` for downstream
`find_package(veng)` consumers** (see [game-module.md](game-module.md)).

### 7. Scene / entity model — remaining: systems, perf, reflection follow-ons

Runtime half delivered by planset-10; the cooked prefab asset + module-ABI seam by
planset-11 (area 10); the **gameplay + authoring layer** (systems framework, cameras,
control, game modes, levels) by planset-29. **The scene editor (area 6, sub-D) consumes
the runtime scene model directly.**

**Delivered:**

- The **hierarchy representation redesign** — the up-link `Parent` is replaced by an intrusive
  sibling-linked `Hierarchy` (`Parent` / `FirstChild` / `PrevSibling` / `NextSibling`): O(1)
  attach/detach/reparent through `SetParent`/`Detach`/`MoveBefore` operations, ordered children,
  O(children) down-traversal (`GetParent`/`ForEachChild`), and O(subtree) recursive destroy —
  closing the up-link's no-down-traversal / no-order / unmanaged-reference limits and giving
  transform propagation and the broadphase a precise change signal. **Design overview:**
  [hierarchy.md](hierarchy.md).

**Delivered — planset-29 (the gameplay + authoring layer):** the **systems framework**
and the layer that wires an actual game on top of it, built from ECS first principles —
a `SceneSystem` + `SceneSimulation` driver with a **Sim / View tick split**; camera
selection per **`Viewer`** seat resolved to a `CameraView` by a pure function (renderer
untouched); an **Input → Intent → Movement** control pipeline (AI/remote drop-in intent
producers); an **`Authority`** ownership annotation threaded ahead of the net layer; a
game mode as a **`Session`** state component + **rule systems** + a config field (no
object, no registry, no ABI bump); the systems **catalog** (`SystemId`/`VE_SYSTEM`, the
host-owned `SystemRegistry`); the thin **`Level`** asset (a world prefab by reference plus
the level-scoped game-mode/system/render wiring, loaded into play); the editor's
**`LevelEditorPanel`**; and a hand-written **`docs/guides/`** tutorial.

**Still future:**

- A **richer system scheduler** — inter-system dependencies and parallelism. The
  delivered driver runs a flat ordered list in two phases (Sim then View) each tick; a
  dependency graph and worker-parallel execution are the next increment behind the same
  `SceneSimulation` seam.
- **Archetype storage** and **dirty-flag, depth-sorted** transform propagation (perf
  optimizations behind the same API; the propagation layer rides the
  [hierarchy redesign](hierarchy.md)'s ordered down-traversal and is what would let
  planset-23's broadphase move from version-gated rebuild to incremental maintenance).
- **Camera blend/shake** (additional View-phase systems beyond the delivered follow rig)
  and **richer `Level` data** (streaming, sublevels, spawn/nav data) — named refinements
  behind the delivered View phase and `Level` asset.
- Migrating `VE_REFLECT` to inline `[[=…]]` **annotation reflection** once
  AppleClang gains P2996/P3394.

**Named follow-on — unify `ShaderInterface`/`MaterialField` onto the reflection
layer.** A later planset re-expresses the GPU-data field tables (the cooker's reflected
shader/material interface) on planset-10's reflection layer, so the editor inspects
material parameters through the **same generic field-walker** it uses for components,
rather than a parallel mechanism. The two are different mechanisms — `VE_REFLECT`
describes **CPU struct layout** (`offsetof`-based, hand-authored, in `libveng`),
whereas `ShaderInterface`/`MaterialField` describe **GPU data layout** (std140/430
offsets, descriptor slots) reflected **offline by the cooker from Slang**. Folding it
into the CPU-only ECS stream would drag the cooker and the material runtime into it. The
reflection layer is built generic enough to host it (one open `TypeId` space, closed
`FieldClass`, a walker proven *non-component-bound* by planset-10's round-trip tests), so
this is additive when taken up. **Design caveat to settle when it is:**
`FieldDescriptor.Offset` is single-purpose today — a C++ `offsetof`. A GPU field's offset
is a *different* number (a std140/430 buffer offset), so the unification needs
`FieldDescriptor` to carry both (or a layout/`FieldClass` distinction) for one walker to
drive a **CPU memcpy** in the component case and a **GPU buffer write** in the material
case. Decide that representation up front; it is painful to retrofit onto a populated
descriptor table.

**Named follow-on — container/array fields in the reflection layer.** `FieldClass` is closed
and has **no list/array class** (`Vector` is a glm math vector, not `std::vector<T>`), so a
`vector<T>` or `T[N]` field cannot be reflected, serialized, or inspected today —
`FieldClassOf<T>()` silently falls through to `FieldClass::Struct` for an unspecialised type.
A later planset adds container support: a fixed-size `T[N]`/`std::array` is the easy subset
(walkable by element offset + stride — element `TypeId` + count on the `FieldDescriptor`, no
erased ops), while a dynamic `vector<T>` needs the `FieldDescriptor` to carry **type-erased
container ops** (size / element-at / resize) plus the element's `TypeId`/`FieldClass`, with the
generic walker, the name-keyed serializer, and the editor inspectors each gaining an `Array`
arm. That erased-ops payload is exactly the complexity the closed `FieldClass` set was built to
keep out, so it is a deliberate reflection-layer decision — it pairs with the
`ShaderInterface`/`MaterialField` unification above (both grow `FieldDescriptor`) and is decided
at the reflection layer, not in a consumer. (The node-based material editor needs none of this:
its node properties are flat leaves, the graph's node/link collections use a bespoke graph
serializer, and a variable-arity node is modelled as **dynamic pins on the node instance**, never
as an array-typed property.)

### 8. Scene renderer / render-pipeline architecture — remaining: the über-pipeline batteries

The `SceneRenderer` shell + minimal deferred spine (g-buffer → deferred lighting →
tonemap) delivered by planset-12; frames-in-flight > 1 correctness by planset-13.
**The editor's scene viewport (area 6, sub-D) consumes this delivered
`SceneRenderer`.** **Design overview:** [scene-renderer.md](scene-renderer.md).

The **authorable post stack has its mechanism** — PostProcess materials (area 13,
planset-18): a tunable effect (grade, curve) is authored as a PostProcess-domain
material run by a `PostProcessScenePass`, not a bespoke C++ pass. **Tonemap** is the
PostProcess material. Fixed-dataflow batteries — SSAO, the shadow atlas, and the
**compute mip-pyramid bloom** — are hardcoded engine passes, not authorable materials.

**Delivered — planset-19 (the batteries + the G2 PBR target):** the renderer went
physically-based — a **metallic-roughness three-target g-buffer** (the G2 ORM target this
area reserved, extending the one `GBufferOutput` struct), tangent-space normal mapping,
Cook-Torrance over **multiple typed lights** (directional/point/spot) behind a ring-buffered
view-constants buffer, a **directional shadow map**, **SSAO**, scalar emissive,
**bloom as a PostProcess material**, and a `DebugView` arm per new channel. Each landed behind
the same `ScenePass` + `Configure`-recompile mechanism (a tunable effect as a PostProcess
material, plumbing as a C++ pass).

**Delivered — planset-20 (bounds facility + CSM):** the engine's first **bounds facility** — an
`AABB` primitive (`Veng/Math/`), a local-space bound per `Mesh`, and a world-space
`SceneBounds(scene)` — and on it **cascaded shadow maps** for the directional light, replacing
the fixed ortho box: per-frustum-slice fit (bounding-sphere + texel-snapped) rendered into a
cascade-sized depth **atlas** in one pass, bound in a **dedicated set 1** (the atlas + an
immutable comparison sampler + a dynamic-uniform `ShadowConstants` block, off bindless) and
sampled by the lighting pass via hardware **`SampleCmp`** with a boundary cross-fade — plus the
`CascadeCount`/`CascadeSplitLambda`/`ShadowResolution` knobs and a `DebugView::Cascades` arm.
Net-new descriptor infrastructure (immutable samplers, dynamic uniform buffers, the `PassIO`
bound-view seam) lands with it.

**Delivered — planset-21 (frustum culling):** the bounds facility's **other** prime consumer — a
**`Frustum`** primitive (`Veng/Math/`, Gribb-Hartmann over Vulkan ZO clip, a conservative
`Intersects(Frustum, AABB)` test) beside `AABB`, a per-frame **`GatherMeshes`** visibility gather
(subsuming `SceneBounds`), and the two scene-drawing passes culling that one shared candidate list —
the **g-buffer pass by the camera frustum**, the **cascaded shadow pass by each cascade's light
frustum** — behind a `SceneRendererSettings::FrustumCull` knob (default on). The rendered image is
byte-identical (a draw-count test over an off-frustum fixture is the guard, not the golden).

**Delivered — planset-23 (BVH broadphase):** the **delivered scaling step** behind the
`GatherMeshes`/broadphase seam — a renderer-owned **`SceneBroadphase`** holding a **BVH**
(`Veng/Math/BVH.h`) over the draw candidates, rebuilt only on a frame the scene's spatial version
moved (**`Scene::GetSpatialVersion()`**, the access-as-write change-tick recovered from the
immediate-mode ECS). The camera and every shadow cascade query it by tree descent rather than a
per-view linear scan; a static scene rebuilds not at all. The query returns the linear scan's exact
set in `GatherMeshes` order — byte-identical, the golden does not move.

**Delivered — planset-24 (shadowed punctual lights):** a bounded set of point/spot lights (a
`MaxShadowedPunctual` budget) cast real shadows — a spot through one perspective map, a point
through six cube faces, both into a **shared punctual shadow atlas** that generalizes set 1 from
"the directional system" to "a shadow system". The deferred lighting pass samples each shadowed
light's map with hardware `SampleCmp` + PCF and multiplies the visibility into its contribution;
each shadow view culls its casters through `SceneBroadphase::Cull` against its own frustum — the
**delivered prime consumer** of the BVH, one tree queried `N` spot frustums + `6N` cube faces per
frame. `PunctualShadows`/`PunctualShadowResolution` are the knobs, `DebugView::PunctualShadows` the
visualizer, and `Veng/Renderer/PunctualShadows.h` the device-free view math beside `ShadowCascades.h`.

**Delivered — planset-28 (mip-pyramid bloom):** the single-level separable-Gaussian bloom is
replaced by a **compute mip-pyramid bloom** battery — a fixed engine pass like SSAO, **not** a
PostProcess material. The lit HDR is bright-passed with **Karis-average** firefly suppression into
mip 0 of an `HdrFormat` mip chain, progressively downsampled, then upsampled with an accumulating
dual filter and composited back into linear HDR ahead of tonemap (per-level compute dispatches
mirroring the hi-Z reduction). The filter kernel is a `BloomKernel { Cod, Kawase }` topology knob
(COD/Jimenez default; Dual Kawase the TBDR-optimized alternative); `Threshold`/`Intensity`/`Radius`
are per-frame `SceneView` values and `Bloom`/`Kernel` the topology knobs, with a `DebugView::Bloom`
arm blitting pyramid mip 0 after the up-sweep. The four bloom materials + their fragment shaders are
deleted.

**Delivered — planset-31 (viewports):** *"a renderable view into a world"* is made a
first-class, ownable, listable thing. A **`Viewport`** (`Veng/Renderer/Viewport.h`) owns a
**region** of the window (`ViewportRegion`) over a `SceneRenderer` and a **role** (`Presented`
= the engine compositor places its texture into its region; `Offscreen` = a consumer samples
it), takes a *pushed* per-frame `ViewState`, and on `Render` does the `Execute` + `Sample`
barrier itself. A **`GatherPass`** scissor-blits each `Presented` viewport into its region on
one full-window linear-HDR assembly target that `SwapChainCompositePass` consumes *unchanged* —
one placement is the fullscreen game, zero is the editor, N quadrant placements is
**splitscreen**, the same gather + composite tail for all three. `Application` drives a
**central drive-list** of viewports (registration order = render order; RAII cleanup —
`~Viewport` self-unregisters, the owner keeps the `Unique`) with an optional engine-owned
**managed primary** viewport (`ApplicationInfo::ManagedViewport`) as the game's plug-and-play
path. An `Offscreen` viewport's `GetOutputHandle` bound into a material (**material-RTT**) is
single-copy under the registration-order producer-before-consumer guarantee. Owning the region
also yields a window↔view mapping (`WindowToViewport`/`ScreenToWorldRay`, over a glm-only `Ray`
in `Veng/Math/`). It took the viewport slice of this area, the render-driving slice of area 6,
and left the pointer-routing seam area 4 consumes.

**Delivered — planset-32 (the render allocation sizes itself):** the render-target
**allocation** became self-determining. Dynamic resolution had a fast inner loop
(`ComputeDynamicResolutionScale` eases a viewport's per-frame sub-rect `RenderScale` toward a
GPU-frame-time budget); this added the **slow outer loop** that sizes the allocation those
targets live in. A pure, device-free **`StepAllocationTier`** (beside the inner-loop controller
in `Veng/Renderer/DynamicResolution.h`) folds a multi-second EMA of the sub-rect scale into a
**quantized allocation tier** through a **hysteresis band** + **asymmetric dwell timers**, and the
`Viewport` debounces a `SceneRenderer::Resize` on a tier change — so the hand-picked `MaxScale`
allocation is gone and the allocation tracks what the scene sustains **without thrashing** (the
expensive knob never reacts to a fast signal; a tier change keeps the rendered pixel count
constant, so reallocation does not pop). Separately, a **`MaxAllocationScale`** cap decouples the
allocation baseline from the **swapchain framebuffer extent**, so a small window on a 2× **HiDPI**
display is not silently supersampled across every render-graph image.

**Still future (planset-32's named follow-ons):** **safe-moment reallocation** — deferring the
tier-change `Resize` to a scene transition / static camera / loading screen rather than firing it
inline (the dwell already makes the inline hitch rare and acceptable; this is polish).
**Memory-driven initial tier** — choosing the *initial* tier from a device memory-budget query
(`VkPhysicalDeviceMemoryProperties`) so a memory-starved device starts low, distinct from the
perf-driven outer loop; this is the seam where the allocation work meets planset-33's texture
compression, since ~8:1 block compression materially changes the texture VRAM residency the query
reads. A **TAA history ping-pong** to remove the TAA history-copy from the full-allocation tail —
the one full-res cost the sub-rect cannot reduce (it scales with allocation, so a tier step *does*
help it; the ping-pong removes it entirely), the next lever if TAA stays too expensive once the
allocation is right.

**Still future:** a **transparent/forward pass** (a second material contract whose fragment
outputs final color) and **MSAA**, reading the delivered `AABB`/`Frustum`/broadphase facility. The
BVH broadphase's refinements, behind the same `Sync`/`Cull` + version-gate seam: **incremental tree
maintenance** (per-object insert/update/remove with fat boxes, for large *N*), **GPU/compute-driven
culling** + **occlusion (hi-Z / two-phase)**, **per-submesh leaf granularity**, and **a Scene-shared
tree** (one tree across consumers). The shadow system's named next increments: **clustered/tiled
light culling** (the lighting loop stays a bounded linear loop until then), **cached/static shadow
maps** (the highest-value — they retire the per-frame `6N` redraw for a static scene), and
**per-light dynamic resolution / shadow LOD** (a variable tile rect in the set-1 records + a packer,
sample shader unchanged; lands alongside clustered culling). The bloom battery's named follow-ons:
**single-pass downsample (SPD)** (the whole downsample in one dispatch — a bandwidth win, same look,
gated on validating `globallycoherent` + device-scope atomics under MoltenVK) and a separate
**FFT/convolution lens bloom** (a GPU FFT convolution against a kernel texture for anamorphic
streaks / aperture ghosts — a cinematic feature of its own, distinct from the everyday glow). Also
named: **colored emissive** (a fourth g-buffer target). Also future: **history-buffer ringing** for
temporal effects (TAA/motion-blur reading an older frame); **cross-queue synchronization** (an
explicit semaphore once a handoff side moves off the single graphics queue); and **parallel
pass recording** into secondary command buffers (area 2's seam — the user-pointer channel is
shaped for it, not built). The viewport facility (planset-31) has its own named follow-ons:
**declared inter-viewport dependencies** (a topological render order over the drive-list's
registration order, once an RTT graph gets deep), **viewport output ringing** (for an
async/off-queue RTT consumer — the single-copy contract holds only for same-frame, same-queue
RTT), and a **playable splitscreen / multi-monitor sample feature** (the tests cover the gather
+ composite; a runtime sample is deferred to keep the planset asset-free). The **CSM shadow-render
path** (planset-20's depth atlas) has a
follow-on — a single-pass depth **array** via `VK_KHR_multiview` or layered
`SV_RenderTargetArrayIndex` routing — but it is a **quality/cleanliness** change (cleaner
per-layer sampling), **not a perf win**: the installed MoltenVK 1.4.0 implements multiview by
instance multiplication (`instanceCount *= viewCount`), not Metal vertex amplification, so it
renders the scene `viewCount` times exactly as the atlas does (verified against the dylib +
upstream `MVKCmdDraw.mm`; see [scene-renderer.md](scene-renderer.md)). Deprioritized, gated on
a `RenderGraph` layered-pass seam; revisit only if MoltenVK adds amplification-based multiview.

**On-tile / subpass-fused deferred is a measure-first maybe, not a named next increment.**
The richer g-buffer raises its bandwidth payoff on Apple Silicon, but it is **not** a
`ScenePass`-level battery: it is a `RenderGraph`-**core** change (local-read / pass fusion plus
an input-attachment g-buffer binding path), gated on a MoltenVK `dynamic_rendering_local_read`
capability check **and** on the g-buffer store-then-sample round-trip being a *measured*
bottleneck. It is its own future planset behind those two gates, not a battery here.

### 9. Compiled RenderGraph — remaining: culling, multi-queue, parallel recording

Delivered by planset-8. **Still future:** dead-pass culling, multi-queue /
async-compute scheduling, and parallel pass recording into secondary command
buffers (area 2's seam). **Design overview:**
[compiled-rendergraph.md](compiled-rendergraph.md).

### 10. Cooker-side module reflection — remaining: cross-compiled cooking

Delivered by planset-11 (`vengc cook --module` `dlopen`s `libgame` and reflects its
component types; the cooked prefab asset rides on it). **The editor's native-type
inspectors (area 6, sub-D) reuse this same module-reflection seam.** **Still future
— cross-compiled cooking:** `dlopen` reflection loads a **host == target** lib, so
cooking a cross-compiled target lib on the build host is a latent constraint for the
anticipated Windows port — recorded, not solved.

### 11. ImGui composite pass — DONE (planset-16)

Delivered by planset-16 as an engine-provided `ImGuiCompositePass` — the
scene-offscreen-output → swapchain composite, owning the bindless-slot + ImGui-texture
re-registration across `Resize`/`Configure` so an app no longer hand-writes it. Since
**consolidated** into **`SwapChainCompositePass`**, scoped to its single real job: the
fixed scene-behind-ImGui swapchain composite. Surfacing a scene output *inside* an
ImGui panel — an ImGui texture over the output plus the out-of-graph sampleability
barrier that read needs — turned out to be a separate, smaller job each consumer does
inline against `ImGuiLayer`/`CommandBuffer` (the established `TextureEditorPanel`
idiom), not part of the composite pass. Nothing pending builds on it. The original
direction note ([imgui-composite-pass.md](imgui-composite-pass.md)) predates delivery.

### 12. UI toolkit — `Veng::UI` — remaining: drive imgui private, stateful widget classes

Delivered by planset-17. The engine-tier **`Veng::UI`** base vocabulary (in `libveng`,
`engine/include/Veng/UI/`, so games *and* the editor author against it) fronts ImGui with a
thin, overload-driven surface — one `Drag` overloaded on `f32`/`vec2`/`vec3`/`vec4`/`i32`,
designated-initializer options structs instead of ImGui flags, `fmt`-preformatted text, and
RAII scopes for the begin/end and push/pop pairs — with imgui-free public-header signatures;
every widget-authoring `ImGui::` site (hello-triangle's debug panel, the reflection inspector,
the editor panels + menu bar) migrated onto it, wrapper-only (ImGui stays PUBLIC).

**Still future:**
- **Drive imgui private** — the Native-idiom end-state: no `<imgui.h>` reachable through any
  public header, imgui linked PRIVATE, guarded by `include_hygiene`. `Veng::UI`'s imgui-free
  headers already meet that header contract; this is the linkage flip and the remaining
  integration-layer relocation. A possible later planset `Veng::UI` unblocks.
- **Stateful editor widget classes** — the `FileBrowser`-style "stateful widget whose `Draw()`
  returns an event" pattern. No existing instance to extract today (the editor uses panel
  classes and has no in-ImGui file browser); a named follow-on, taken up when a widget that
  actually holds persistent state (cwd, selection, filter, scroll) needs extracting.

**Design overview:** [ui-toolkit.md](ui-toolkit.md).

### 13. Material domains + shader-graph codegen — domains slice DONE (planset-18); codegen PRIORITIZED

The **material-domains slice is delivered (planset-18)** — the prioritized first half of
this area. A material's parameters are now **one reflection-sized, ring-buffered block** (the
fixed engine `MaterialData` struct deleted; an arbitrary shader-defined handle set), and a
`Material` carries a first-class **`MaterialDomain`** (Surface + PostProcess). The PostProcess
**fullscreen-material path** (`PostProcessScenePass`) stands up in `SceneRenderer`, the engine
ships the **standard vertex shader per domain** (`surface.vert`, `fullscreen.vert`), **tonemap
is the first PostProcess material** (authorable exposure), and the node catalog is
**domain-aware** (`MaterialOutput`'s pins follow the domain's output contract). Fixed plumbing
composites (`SwapChainCompositePass`, the debug blits) stay hardcoded engine passes.

The committed end-state is **shader-graph codegen** — the node graph **generates** the Slang
source — which **remains the still-future follow-on**: every node an expression emitter, a
`Param` gaining const-vs-exposed, compile's target becoming generated Slang. The domains slice
lands the foundational inversion it needs (a domain-correct output sink). What was delivered:

- **Material domains (delivered, planset-18) — Surface and PostProcess.** A first-class
  **domain** selects the output contract (g-buffer channels vs a single final color),
  the inputs, the pipeline shape, and the invocation site — the standard cross-engine
  factoring (Unreal `MaterialDomain`, Unity targets, Godot `shader_type`), with the
  parameter schema / bindless / authoring / inspector shared across domains. The
  PostProcess domain's **fullscreen material pipeline path** in `SceneRenderer`
  (a `ScenePass` building a pipeline from a postprocess material against one color
  target) is the authorable **exposure / tonemap-curve / color-grading** stack
  named under [area 8](#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries),
  expressed as materials. Fixed-dataflow batteries (bloom, SSAO, the shadow atlas) and
  *plumbing* composites (`SwapChainCompositePass`) stay hardcoded engine passes — a
  postprocess material is for *tunable effects*, not plumbing.
- **Node→Slang codegen (the still-future follow-on).** The graph emits the fragment source instead
  of binding to a pre-authored one. The node catalog is **reshaped toward it**:
  `MaterialOutput` becomes a **domain-driven sink** (its pins are the domain's output
  contract, not a reflection of a hand-authored shader's `GetFields()`); every node
  becomes an **expression emitter** (`TextureSample` → `tex.Sample(…)`, math nodes →
  real code — the "basic math" nodes are inert in a pure binding model, the tell that
  the catalog was already half-built for codegen); a `Param` gains a **const-vs-exposed**
  distinction (folded inline vs a generated `MaterialParams` uniform); and compile's
  target changes from a `.vmat` field list to **generated Slang** the cooker compiles
  like any shader (Slang → SPIR-V + reflection; no new runtime path). planset-15's
  topology core (typed pins over the `TypeId` space, coercion-on-link, reflected node
  properties, the JSON round-trip) is already codegen-ready; the reshaping is in the
  material catalog + compile target + the new domain concept.

**Supersedes planset-15 decision 9** — codegen is now committed direction, not a
possibility the node model merely tolerates. **Design overview:**
[material-codegen.md](material-codegen.md).

### 14. Engine-owned material shader header + cross-pack Slang includes — PRIORITIZED

A consumer's material fragment shader (e.g.
`examples/hello-triangle/assets/shaders/brick.frag.slang`) `#include`s a **vendored
copy** of the engine's material declarations (`material_data.slang`), kept
byte-identical-by-hand with the engine core copy
(`engine/assets/core/shaders/material.slang`). The cooker's Slang session adds only
the source file's **own directory** to its search paths (`searchPathCount = 1`, in
`ShaderImporter.cpp` + `SlangReflect.cpp`'s two sites), so a consumer shader has no
include path into the engine core pack — hence the duplicate. These declarations are
the **engine's** concern; a consumer should *import* them, not vendor them. Two coupled
changes:

- **Cross-pack Slang include resolution.** The cooker adds the engine core shader
  directory to every Slang session's search paths (the three setup sites share a
  helper), so a consumer `.slang` can `#include "Veng/material.slang"` and resolve the
  engine header directly. The vendored `material_data.slang` is deleted and
  `brick.frag.slang` includes the engine header.
- **The header split — engine contract vs authored surface.** The engine header keeps
  only the **engine's** contract: the set-0 bindless binding declarations
  (`g_Textures`/`g_Samplers`/`g_MaterialParams`), `MaterialParamStride`, the
  `GBufferOutput` g-buffer contract, and the shared `PushConstants` block. **`MaterialParams`
  moves out into the authoring shader** (`brick.frag.slang`): the per-material parameter
  struct is **per-shader by definition** — the cooker reflects each material shader's own
  struct to pack its fields at the reflected offsets — so a single "shared" copy that must
  stay byte-identical across engine and consumer is conceptually wrong. `LoadMaterialParams`
  either becomes generic (a `T Load<T>(uint)` over the engine's `g_MaterialParams` + stride)
  or is authored beside the moved struct. The C++ mirror (`Veng/Renderer/MaterialParams.h`,
  static_assert-guarded) follows the same split if it remains a shared engine type.

A natural **precursor to node→Slang codegen** (area 13): generated fragment shaders will
`#include` the engine material header rather than each carrying a copy, and will emit their
own `MaterialParams` struct — exactly the split this lands.

### 15. Build configurations & project settings — texture-compression authoring

> **Taken up by [planset-35](../planset-35/README.md) (proposed).** The developer-control layer
> below — the project-settings/build-configuration concept, the role → format table, the coarse
> per-config cook dependency, the host-default CMake selection, and the editor host-capability preview
> gate — is scoped into planset-35; the still-open **footprint** items (BC5/BC4, wider ASTC, HDR ASTC,
> the uncompressed fallback pack), **editor active-config persistence**, and the **Windows
> cross-compile constraint** stay future behind it. Marked delivered when that planset lands.

**Motivated by planset-33's texture-compression track**, which ships the BC7/ASTC
codec plumbing but **hardcodes ASTC as the cook default** (BC7 selectable through a
minimal internal seam) and defers all developer control. This area adds that control: a
**project-settings** concept owning a set of **build configurations** (one per ship
target — Windows / macOS / Linux / mobile), each holding the **texture codec policy** as
a **role → concrete-format** table, with per-asset `*.tex.json` declaring a compression
**role/intent** (Color / Normal / Mask / HDR) rather than a raw codec. The codec is
**per-platform by nature** (BC needs `textureCompressionBC` — Apple-Silicon/Windows; ASTC
needs `textureCompressionASTC_LDR` — broad on Apple GPUs), so it belongs on the build
config, not a flat default or a per-texture knob.

Two design pillars beyond the settings tiers: the **cook-time dependency is implicit
and coarse** (the active config is a central depfile input over a whole-pack cook; one
output pack per config makes per-config invalidation fall out — no fine-grained
per-asset edges, the content-addressed per-asset cook cache stays a separate cooker
optimization); and the **editor gates preview to host capability** — building any
config is unrestricted (the encoder is CPU), but *previewing through* one is bounded
by what the host GPU can sample, with a host-safe default preview so "ASTC on Windows"
is structurally impossible. The settings structs are reflected, so the editor panels
come free through `DrawFieldWidget`. **Gate met** by planset-33 (the BC7/ASTC formats, the
`FormatInfo` block math, the `IsBlockCompressionSupported()` / `IsAstcSupported()`
queries, the cooker codec selection). **Design overview:**
[build-configurations.md](build-configurations.md).

**Still-open footprint work** rides this area's open questions, since the right home for
each is the role → format table a build configuration owns: **BC5/BC4 channel
specialization** (two-channel normals / single-channel masks — `Normal → BC5`, `Mask →
BC4` rather than full BC7/ASTC); **wider ASTC footprints** (6×6, 8×8 — more compression at
lower quality, a per-role footprint choice); **HDR ASTC** (the LDR codec this track ships
does not cover HDR sources — environments keep their own `RGBA16Sfloat` panorama path); and
an **uncompressed fallback pack** for a device that supports neither cooked codec (today
such a device gets `AssetError::Unsupported` per texture).

### 16. Dynamic meshes — mutable runtime geometry

**Motivated by planset-34's mesh-source unification**, which makes a procedural primitive a mesh
whose *source* is a declarative recipe (the Godot `PrimitiveMesh : Mesh` model), resolved through the
ordinary load path. That covers meshes that are a pure function of a few parameters; it deliberately
does **not** cover meshes whose **vertex buffer is the source of truth** — geometry mutated in place
at runtime (runtime/in-editor sculpting, voxel / marching-cubes terrain, CSG / destruction,
gameplay-generated trails / soft bodies). The real new capability is a **mutable `Mesh`** (an in-place
buffer-update path with retire-on-resize; `Mesh` is immutable after upload today) — orthogonal to the
recipe question and the substrate a `DynamicMeshComponent` would own. **Deferred** until a concrete
consumer exists to design it against; the single-mesh-slot model from planset-34 keeps the door open
(a dynamic mesh produces a `Mesh` into the same `MeshRenderer.Mesh` slot). **Design overview:**
[dynamic-meshes.md](dynamic-meshes.md).

## Ordering & dependencies

The order to *take the remaining areas up* (each becomes its own planset), not a
schedule:

1. **Material domains (area 13's first slice) is delivered (planset-18)**: the domain
   concept (Surface + PostProcess), the unified ring-buffered parameter block, the
   PostProcess fullscreen-material path, and the domain-aware node catalog. Its named
   follow-on, **node→Slang codegen** (the graph emits the fragment source — every node
   an expression emitter, const-vs-exposed params, generated-Slang compile target), is
   now **prioritized**, with the domain slice having landed the foundational
   domain-driven output sink it needs.
2. **Editor — scene editor (area 6, sub-area D)** is the **next editor planset**;
   all its gates are met (area 7, area 10, area 8's `SceneRenderer`, and editor
   sub-areas B/C).
3. **Engine-owned material shader header + cross-pack Slang includes (area 14)** is a
   small, self-contained prioritized item and a **codegen precursor** — fold it into the
   node→Slang codegen planset (it lands the engine-header import that generated shaders
   need) or take it first on its own.

   Node→Slang codegen, the scene editor, and area 14 are the prioritized next areas,
   whichever is taken up first.
4. **Event & input + networking (area 4)** is the **next gate** the delivered gameplay
   layer (area 7, planset-29) motivates — multi-seat input routing and a net layer
   consuming the `Intent`/`Authority`/Sim-View seams. The remaining named still-future
   increments of the areas done in part (1, 2, 7, 8, 9, 10, 12) are each independent and
   off the critical path — slot in whenever wanted.

## Cross-cutting concerns

Considerations that span the work above and are cheaper to decide early than to
retrofit.

- **Process discipline.** Keep planset-1's cadence — small, sample-verified,
  per-plan increments. Every planset to date has followed it; the same discipline
  applies to the scene editor (area 6, sub-D) ahead.

## Status

Vision only beyond what is delivered in the plansets
([plans/README.md](../README.md)).

**Delivered (planset-29):** area 7's **gameplay + authoring layer** — the systems
framework (`SceneSystem`/`SceneSimulation`, the Sim/View tick split), camera selection
per `Viewer` seat resolved by a pure function, the Input → Intent → Movement pipeline, the
`Authority` annotation, game modes as `Session` state + rule systems, the systems catalog
(`SystemId`/`VE_SYSTEM`/`SystemRegistry`), the thin `Level` asset + its cooker importer +
editor `LevelEditorPanel`, and the `docs/guides/` tutorial. It motivates and shapes area 4.

**Delivered (planset-18):** area 13's **material-domains first slice** — the material
**domain** concept (Surface + PostProcess), the unified ring-buffered parameter block,
the PostProcess fullscreen-material path, the standard per-domain vertex shaders,
tonemap-as-material, and the domain-aware node catalog.

**Prioritized:** area 13's named follow-on, **node→Slang codegen** (the graph generates
the fragment source — every node an expression emitter, const-vs-exposed params,
generated-Slang compile target); the **scene editor** (area 6, sub-area D — all its
gates met: areas 7, 10, 8, and editor sub-areas B/C); and **engine-owned material shader
header + cross-pack Slang includes** (area 14 — the cooker resolves a consumer shader's
`#include` into the engine core pack and `MaterialParams` moves to the authoring shader; a
codegen precursor that can fold into that planset). Whichever the next planset takes up.

**The next gate:** area 4 (**events/input + networking**) — multi-seat input routing and
a net layer over the `Intent`/`Authority`/Sim-View seams planset-29 established.

**Undetailed / unscheduled:** the named still-future
increments of the
areas done in part — area 1's
**hot-reload**, area 2's task graph / staging pool / cancellation, area 7's
**richer system scheduler** (inter-system dependencies/parallelism) + archetype/perf
follow-ons + camera blend/shake + richer `Level` data +
the `ShaderInterface`/`MaterialField`
unification + container/array fields, area 8's **transparent/forward pass** +
**clustered/tiled light culling** + **cached/static shadow maps** + **per-light dynamic
resolution / shadow LOD** + the **BVH broadphase's refinements** (incremental tree
maintenance / GPU/occlusion culling / per-submesh leaves / a Scene-shared tree) +
history-buffer ringing / cross-queue
sync, area 9's culling / multi-queue /
parallel recording, area 10's **cross-compiled cooking**, area 12's **drive
imgui private** + stateful editor-widget classes (the base `Veng::UI` vocab + full
migration delivered by planset-17), and area 15's **build configurations & project
settings** (texture-compression authoring — the developer-control layer planset-32
defers). Each becomes its own planset when taken up.
