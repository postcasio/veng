# future — work beyond the current plansets (DRAFT / vision)

> Not a planset and not scheduled — a **holding area** for the larger phases
> ahead. Each area below becomes its **own planset** when taken up and detailed
> planset-1 style. Direction, not plans.

Captured now so the earlier phases stay coherent with where veng is going.

## Areas

Numbered for stable cross-references. **Delivered** areas are documented in their
plansets (see [plans/README.md](../README.md)) and are *not* re-narrated here —
only their **still-future remainder** is kept, plus any delivered capability a
pending area builds on directly. The substance of this document is now the
**remaining** work: **multi-seat input routing + networking (area 4), now that the
event-routed input core is delivered** — and the named still-future increments of the
areas done in part (areas 1, 2, 7, 8, 9, 10, 12, 15). The areas that were the prioritized
next work — **material domains + shader-graph codegen (area 13)**, the editor's **scene
editor (area 6, sub-area D)**, and **engine-owned material shader header + cross-pack Slang
includes (area 14)** — are now **delivered** (plansets 36–39) and documented in their
plansets.

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
[game-module.md](game-module.md). All four sub-areas are delivered — sub-area D
(the scene editor) builds on the first three:

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

**Sub-area D — scene editor — DELIVERED.** The scene-editing surface is the **prefab
editor** (`PrefabEditorPanel`, registered for `AssetType::Prefab`): it `SpawnInto`s a
prefab into a document-owned live `Scene` and hosts a `SceneViewportPanel` (its own
registered `Offscreen` viewport the engine renders), a `PrefabExplorerPanel` scene-graph
tree over the intrusive `Hierarchy`, and the reflection `InspectorPanel`, over one shared
`PrefabEditContext`. The **level editor** (`LevelEditorPanel`) **derives from** it, reusing
the viewport/explorer/inspector unchanged and adding the level-scoped systems + settings
panels (area 7, planset-29). **planset-37** closed the authoring loop: GPU **id-buffer
picking** (`Viewport::Pick`, an off-by-default `SceneRenderer` battery) for click-to-select
of meshes *and* light/camera billboards, **hand-rolled translate/rotate/scale gizmos** over
`ScreenToWorldRay`, a **per-`AssetEditorPanel` undo/redo** command stack, and a
reflection-driven **`Scene` → `.prefab.json` save-back** (preserve-unknown-keys, stable
per-entity ids; the level editor routes its edits to the referenced world prefab). **planset-36**
gave the inspector a **presentation axis** — the `FieldDisplay` cascade (widget kinds, named-enum
combos, collapsible structs/arrays/categories, conditional display). Native-type inspectors reuse
area 10's module reflection; a `Scene` is runtime-only, so the authored asset is the `Prefab`, not
a cooked scene. The current editor is documented in [editor/CLAUDE.md](../../editor/CLAUDE.md).

**The editor is a single shell launched against a project — delivered.** `veng_add_editor`
no longer builds a per-game editor binary: one project-agnostic `veng-editor` exe (in `editor/`)
is launched with `--project <project.veng>`, reads the module(s) the project names
(`ProjectSettings::Module` / `EditorModule`, the `"module"` / `"editorModule"` keys), and dlopens
them from the project's build-output dir — the game ships only its library, referenced from the
project file. The build-output dir is **discovered from the project**: the build writes it into a
gitignored `.veng/build.json` sidecar beside the source `project.veng`, so launching with only a
project resolves it with no CMake in the loop (`--build-dir` stays an override). In-tree the
`<name>-editor` **run target** launches it with the project's source.

**A project-picker launcher — STILL FUTURE.** A standalone front-end that lists projects and, on
selection, spawns `veng-editor --project <path>` (the editor self-discovers the build dir via the
sidecar above — the seam this builds on). The project list is a **launcher-owned recent-projects
list** (browse to a `project.veng` once, remembered; Unity-Hub style, no build coupling) **combined
with a scanned default projects directory** in the user's home. The launcher stores its own state
(the recent list, settings) in the **per-OS user data directory** — `%APPDATA%` on Windows,
`~/Library/Application Support` on macOS, and the XDG base dir on Linux (`$XDG_CONFIG_HOME`, default
`~/.config`, with `$XDG_DATA_HOME` / `~/.local/share` for non-config state) — fronted by a small
engine path helper resolving the per-platform location. A shared resolver (project path → build dir,
reading the sidecar) is reused by both the editor and the launcher. Multi-build-dir (one project
built in `build/` and `build-debug/`) is **last-configure-wins** today; the launcher could surface
the choice.

**Hosting third-party game modules (the module ABI / SDK freeze) — STILL FUTURE.** The single-shell
editor above is **same-tree only**: a module must be built from the same source tree as the editor,
the `VengModuleAbiVersion` integer handshake rejecting a mismatch loudly at load. Letting a
separately built (third-party) module load into a *shipped* editor needs a **frozen module ABI +
a shipped SDK**:

- a versioned SDK — installed headers + import libs + the `veng-editor` binary + a bundled cooker —
  consumed through `find_package(veng)` (the install wiring named in
  [game-module.md](game-module.md), which the editor shell now shares the need for);
- a **pinned, enforced toolchain contract** — same compiler/STL, and `-fno-exceptions`/no-RTTI
  uniformity (a module built *with* exceptions against `-fno-exceptions` veng is UB at the
  boundary), today only convention;
- **frozen, additive-only boundary interfaces** — the `VengModuleHost` layout and the
  `Application` / `EditorPanel` / `SceneSystem` vtables a plugin derives from — versioned by semver
  or capability negotiation rather than the blunt single integer, so an older module keeps loading
  against a newer editor.

Type / asset / system identity already crosses the boundary as authored `u64` ids (not RTTI or name
mangling), and type registration is GPU-free by contract, so a bundled cooker reflects a plugin's
types with no device — the hard parts of plugin identity are in hand. The gap is **packaging +
interface-freezing discipline**, deferred until the engine surface stops churning each planset.

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

**Delivered — planset-36 (reflection display options):** the reflection layer gained a
**presentation axis** orthogonal to `FieldClass` — a `FieldDisplay` cascade (a type default on
`TypeInfo` + a per-field override on `FieldDescriptor`, resolved field → type → hard default)
carrying widget kind, `Min`/`Max`/`Step`, and collapsible/category state; **enumerator
{name, value} reflection** (`VE_ENUM`, turning the raw-integer enum drag into a named combo and
retiring the hand-written enum combos); and **conditional display** (`VisibleIf`/`EnabledIf`
type-erased predicates). Editor-and-reflection only — no cooked-format or render change.

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

**Container/array fields in the reflection layer — DELIVERED.** `FieldClass::Array` exists and
covers the **dynamic `vector<T>`** case (the hard half), not just fixed-size: the `FieldDescriptor`
carries the type-erased container ops (`ArraySize` / `ArrayElement` / `ArrayElementConst` /
`ArrayResize`) plus the element `TypeId`, authored through the `VE_ARRAY_FIELD` macro
(`FinishArrayField`), and the generic walker / name-keyed serializer / editor inspector all have their
`Array` arm. `ProjectSettings::Configurations` (a `vector<BuildConfiguration>`) is the live consumer.
The `ShaderInterface`/`MaterialField` unification above is still the open reflection-layer follow-on;
container support is done.

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

**The allocation-tier outer loop was removed in [planset-39](../planset-39/README.md).** Its
tier-change `Resize` **hitched** — the dwell made the hitch rare, not absent, and a steady-state
reallocation hitch is worse than running at a fixed allocation. The **inner loop**
(`ComputeDynamicResolutionScale`, per-frame sub-rect scaling over a fixed allocation — no realloc, no
hitch) and the `MaxAllocationScale` HiDPI cap (now a **static** ceiling) are kept; the
`StepAllocationTier` EMA/hysteresis/dwell state and the tier-driven `Resize` are deleted. Dynamic
resolution now adapts **cost** (the sub-rect), not **allocation footprint**.

**Delivered — planset-39 (emissive + atmospheric sky + dynamic ambient):** **color-decoupled
emissive** as an additive forward `EmissiveScenePass` into the lit HDR target (RGB, replacing the
scalar ORM emissive term — and *not* a fourth g-buffer target, the form this area had reserved); a
**Bruneton precomputed atmospheric sky** (the first real `Type3D` volume-texture consumer — the 4D
scattering table packed as a 3D texture) rendered in the skybox slot; and a **dynamic SH ambient**
that projects that sky into order-2 spherical harmonics each frame as the third ambient arm
(`IBL : skylightSH : flat constant`), so the no-environment ambient is directional and tracks the
sun. An order-2 `SphericalHarmonics.h` math primitive and the `Type3D` create/storage-write/sample/
retire lifecycle land foundation-first with it. **Aerial perspective** and a **sky-driven specular
prefilter** are named follow-ons.

**Still future (after the tier removal):** a **memory-driven fixed-allocation choice** — picking the
one fixed allocation up front from a device memory-budget query
(`VkPhysicalDeviceMemoryProperties`) so a memory-starved device starts smaller; this replaces the
removed perf-driven tier as the footprint lever and is the seam where the allocation work meets
planset-33's texture compression, since ~8:1 block compression materially changes the texture VRAM
residency the query reads. A **TAA history ping-pong** to remove the TAA history-copy from the
full-allocation tail — the one full-res cost the sub-rect cannot reduce — stays the next lever if TAA
is too expensive at the fixed allocation. (**Safe-moment reallocation** is **dropped**: with the
tier-driven `Resize` gone there is no footprint reallocation left to defer; only a genuine
region/window extent change reallocates, which is correct, not a hitch to schedule away.)

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
future: **history-buffer ringing** for
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

### 13. Material domains + shader-graph codegen — DELIVERED (plansets 18, 38, 39)

Both halves of this area are now delivered. **Design overview:**
[material-codegen.md](material-codegen.md).

**Delivered — planset-18 (material domains).** A material's parameters are **one
reflection-sized, ring-buffered block** (the fixed engine `MaterialData` struct deleted; an
arbitrary shader-defined handle set), and a `Material` carries a first-class **`MaterialDomain`**
(Surface + PostProcess) selecting the output contract (g-buffer channels vs a single final
color), inputs, pipeline shape, and invocation site — the standard cross-engine factoring
(Unreal `MaterialDomain`, Unity targets, Godot `shader_type`). The PostProcess **fullscreen-
material path** (`PostProcessScenePass`) stands up in `SceneRenderer`, the engine ships the
**standard vertex shader per domain** (`surface.vert`, `fullscreen.vert`), **tonemap is the
first PostProcess material** (authorable exposure), and the node catalog is **domain-aware**
(`MaterialOutput`'s pins follow the domain's output contract). Fixed plumbing composites
(`SwapChainCompositePass`, the debug blits) and fixed-dataflow batteries (bloom, SSAO, the
shadow atlas) stay hardcoded engine passes — a PostProcess material is for *tunable effects*,
not plumbing.

**Delivered — planset-38 (node→Slang codegen + the material-instance split).** The node
material editor stopped *wiring* a hand-authored fragment shader and started *generating* it.
Every node is now an **expression emitter**, `CompileMaterialGraph` is a **topological emit
walk** threading a thin typed **`EmittedValue`** (the graph *is* the AST; Slang is the
compiler — no parsed-AST intermediate), and `MaterialOutput` emits the domain entry point
(`GBufferOutput` for Surface, `SV_Target0` for PostProcess). A `Param` gained a **provenance**
(const folds inline / exposed becomes an author-tweakable `MaterialParams` field / engine-bound
is a field the engine writes by name), and the emit walk generates the `MaterialParams` struct
(large-alignment-first) + the `.vmat` field list **together** so the cooker's offset-patching
agrees by construction. The emit walk lives in a shared **`veng::graph`** lib both the editor
and cooker link, so editor preview == offline cook; a fragment shader's *source* changed from a
`.slang` file to a **graph** (no new asset, no checked-in generated file). On that, the
**material vs. material-instance** split: a generated parent shader + its exposed-param
**schema** is a parent `Material` (the pipeline); a new **`MaterialInstance`** is a cheap sparse
override over it (its own SSBO slot + texture set, the parent's pipeline), so 30 tinted bricks
are 1 shader + 30 instances. The instance half (the per-material slot + stall-free `SetParam`)
**moved** off the fused `Material`; a runtime instance + per-frame `SetParam` is the **MID**.
**Supersedes planset-15 decision 9** — codegen is committed direction, delivered.

**Delivered — planset-39 (material-instance ids).** Retired planset-38 Plan 05's parent-id
**overload** (one `AssetId` naming both a `Material` parent and its implicit zero-override
`MaterialInstance`) for explicit minted **`defaultInstance`** ids the cook emits real instances
at and every reference is rewritten to, restoring "one id ⇒ one asset of one type"; the editor
mints that id on material create/save.

**Still future — graph-editing refinements.** Named follow-ons the codegen model leaves open:
**pure-shader graph editing**, **wired asset pins**, **custom-expression nodes**, **subgraph/
function nodes**, **vertex-stage codegen**, **static switches** (a param that changes the
compiled shader — a parent-variant permutation key, not an instance override), **draw-sort by
parent pipeline**, and **instance-of-instance chains**.

### 14. Engine-owned material shader header + cross-pack Slang includes — DONE (planset-38)

Delivered as planset-38's **Plan 00** (the codegen precursor it was named the precursor to).
A consumer's material fragment shader no longer `#include`s a **vendored copy** of the engine's
material declarations: a shared cooker Slang-session helper adds the engine core shader-include
directory to every session's search paths (the three former `searchPathCount = 1` sites), so any
`.slang` can `#include "Veng/material.slang"` and resolve the engine header directly. The engine
header (`material.slang`) was split to keep only the **engine's** contract — the set-0 bindless
binding declarations, `g_ViewConstants`, the `GBufferOutput` g-buffer contract, `DrawData`, the
domain-keyed push blocks, and the per-domain fragment-input struct — while **`MaterialParams`
moved out into the authoring shader** (it is per-shader by definition, reflected per material).
All **four** vendored `material_data.slang` copies, the `Veng/Renderer/MaterialParams.h` C++
mirror, and its drift-guard test were deleted. Same SPIR-V — `smoke_golden` did not move.

### 15. Build configurations & project settings — remaining: footprint specialization, persistence, cross-compile

Delivered by [planset-35](../planset-35/README.md): the developer-control layer planset-33
shipped the codec plumbing for and deferred — a **project-settings** concept owning a set of
**build configurations** (one per ship target), each a **role → concrete-format** table; a
texture declaring a compression **role/intent** (Color / Normal / Mask / HDR / UI) rather than
a raw codec; the **coarse per-config cook dependency** (the config a central depfile input,
one output pack per config); the **host-default CMake selection** (`VENG_BUILD_CONFIG`,
host-triple-defaulted; `cook-all-packs`); and the editor's **host-capability preview gate**
(building any config unrestricted, previewing one bounded by `IsBlockCompressionSupported()` /
`IsAstcSupported()`, so "ASTC on Windows" is structurally impossible). The reflected settings
structs (`Veng/Project/`) draw their editor panels free through `DrawFieldWidget`. **Design
overview:** [build-configurations.md](build-configurations.md).

**Delivered — planset-39 Plan 03 (BC5/BC4 channel specialization).** `Normal → BC5`
(two-channel) and `Mask → BC4` (single-channel) on the role → format table, with the **ASTC
normal-packing convention** (XY + Z reconstruct, since ASTC has no two-channel mode) and one
shared codec-agnostic normal-unpack shader helper. The first per-codec channel specialization
of the settled role taxonomy.

**Still future — more formats/encoders and orthogonal concerns, not settings-tier work.** The
role *taxonomy* is settled and its first channel specialization is delivered; the remaining
per-codec footprint choices ride the role → format table a build configuration already owns:

- **Wider ASTC footprints** (6×6, 8×8) — more compression at lower quality, a per-role choice.
- **HDR ASTC** — the cooked block codec is LDR-only; HDR sources keep their `RGBA16Sfloat`
  panorama path.
- **Uncompressed fallback pack** — for a device that supports neither cooked codec (today such a
  device gets `AssetError::Unsupported` per texture).
- **Editor active-config persistence** — per-project vs per-user editor state.
- **The Windows cross-compile constraint** — a foreign-platform pack is CPU-only and fine, but
  `--module` prefab reflection still loads a *host* lib (area 10's recorded cross-compiled-cooking
  limit), so a fully cross-compiled build inherits that latent constraint.

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

### 17. Cook performance — parallel texture encode — ✅ DELIVERED

**Motivated by the texture cook dominating build wall-clock.** The cook's cost is texture
block-encoding, and all three levers are pulled in the cooker build: the ASTC codec builds the **SIMD
ISA** (NEON on Apple Silicon, SSE4.1 on x86_64) instead of the scalar `none` reference, the ASTC/BC7
encoders + the `TextureImporter` mip/block loops are compiled **`-O2` even in Debug** (they were
inheriting `-O0`, the dominant cost in a CLion Debug build), and **`EncodeAstcLevel` now threads the
encode** across `std::thread::hardware_concurrency()` workers.

`EncodeAstcLevel` allocs the `astcenc_context` for N threads and runs N `astcenc_compress_image` calls,
each from a distinct thread under its own `[0..N-1]` index (the encoder's documented multithreading
model — blocks dynamically scheduled across the threads); the worker count is capped at the level's
block count so a tiny mip does not spawn idle threads. **Golden-safe by construction:**
`ASTCENC_INVARIANCE` is on, so the encode is independent of thread count and the smoke golden does not
move (verified: `smoke_golden` passes with the blocks byte-identical). On the dev machine the sample
cook saturates ~6 cores (623% CPU, ~17 s of work in ~2.8 s wall). A natural extension is cross-texture
parallelism (a thread pool over the importer), but within-texture threading is the cleaner first step
and is what astc-encoder is built for. Optionally pairs with a content-addressed per-asset cook cache
(area 15, Decision 3) so an unchanged texture is not re-encoded at all.

### 18. Cook-time dead-asset pruning — asset tree-shaking

**Motivated by [planset-39](../planset-39/README.md)'s default-instance question.** Plan 01 emits a
companion zero-override `MaterialInstance` for every parent material; a material referenced *only* as a
parent (never directly) ends up with a default instance nobody loads. That is one symptom of a general
missing capability: the cook does not **prune unreferenced assets** from a pack. The real item is
linker-style **tree-shaking** over the cooked asset graph:

- **Mark-and-sweep from roots.** Roots are what the project entry-points (the startup level, plus any
  explicitly-exported assets). Transitively follow every asset reference — level → prefab → component
  `AssetHandle` fields, mesh → material instances, instance → parent material, material →
  shader/textures — and drop anything unreached rather than cooking it into the pack. The
  default-instance prune falls out as a special case.
- **The interesting part is cross-pack visibility.** An asset can be unreferenced *within its own
  pack* but named from another pack, so naïve per-pack pruning would drop a live asset. The fix is the
  linker **exported-symbol** concept: an asset is a root if it is project-reachable *or* marked
  public/exported, so cross-pack consumers keep it alive. Whole-project reachability vs. an explicit
  export marker is the design decision this area settles.
- **Composes with existing cook infrastructure** — the content-hash + TOC (planset-9), the per-config
  cook (planset-35), and a content-addressed cook cache (area 17's note): pruning is another cook-time
  pass over the same reference graph.

A cooker-pipeline item like area 17, deferred until taken up.

## Ordering & dependencies

The order to *take the remaining areas up* (each becomes its own planset), not a
schedule. The areas that were the prioritized next work are now delivered:

- **Material domains + shader-graph codegen (area 13) — delivered.** The domain concept
  (planset-18), node→Slang codegen + the material-instance split (planset-38), and the
  explicit default-instance ids (planset-39).
- **Editor — scene editor (area 6, sub-area D) — delivered.** The prefab + level editors
  (the scene-editing surface), closed by planset-37's picking/gizmos/undo/save-back and
  planset-36's reflection display options.
- **Engine-owned material shader header + cross-pack Slang includes (area 14) — delivered**
  as planset-38's codegen precursor (Plan 00).

**Event & input + networking (area 4)** is now the **next gate** the delivered gameplay layer
(area 7, planset-29) motivates — multi-seat input routing and a net layer consuming the
`Intent`/`Authority`/Sim-View seams, built on the event-routed input core (planset-30). The
remaining named still-future increments of the areas done in part (1, 2, 7, 8, 9, 10, 12, 15)
are each independent and off the critical path — slot in whenever wanted.

## Cross-cutting concerns

Considerations that span the work above and are cheaper to decide early than to
retrofit.

- **Process discipline.** Keep planset-1's cadence — small, sample-verified,
  per-plan increments. Every planset to date has followed it; the same discipline
  applies to area 4 (events/input + networking) ahead.

## Status

Vision only beyond what is delivered in the plansets
([plans/README.md](../README.md)).

**Delivered (planset-29):** area 7's **gameplay + authoring layer** — the systems
framework (`SceneSystem`/`SceneSimulation`, the Sim/View tick split), camera selection
per `Viewer` seat resolved by a pure function, the Input → Intent → Movement pipeline, the
`Authority` annotation, game modes as `Session` state + rule systems, the systems catalog
(`SystemId`/`VE_SYSTEM`/`SystemRegistry`), the thin `Level` asset + its cooker importer +
editor `LevelEditorPanel`, and the `docs/guides/` tutorial. It motivates and shapes area 4.

**Delivered (planset-18 + planset-38 + planset-39):** area 13's **material domains + shader-
graph codegen**, both halves. Domains (Surface + PostProcess), the unified ring-buffered
parameter block, the PostProcess fullscreen-material path, the per-domain vertex shaders, and
the domain-aware node catalog (planset-18); **node→Slang codegen** — every node an expression
emitter, the topological emit walk in a shared `veng::graph` lib, const/exposed/engine-bound
params, generated-Slang compile target — and the **material vs. material-instance split**
(planset-38), with explicit minted `defaultInstance` ids (planset-39).

**Delivered (planset-38, Plan 00):** area 14 — **engine-owned material shader header + cross-
pack Slang includes**. The cooker resolves a consumer shader's `#include "Veng/material.slang"`
into the engine core pack, `MaterialParams` moved to the authoring shader, and all four vendored
copies + the C++ mirror were deleted (the codegen precursor).

**Delivered (planset-36 + planset-37):** area 6's **scene editor (sub-area D)** — the prefab
editor (the scene-editing surface) + the level editor deriving from it, closed by id-buffer
picking, hand-rolled gizmos, per-document undo/redo, and `Scene` → `.prefab.json` save-back
(planset-37), with the reflection inspector's presentation axis — the `FieldDisplay` cascade,
named-enum combos, collapsible structs/arrays/categories, conditional display (planset-36).

**Delivered (planset-39):** renderer additions — **color-decoupled emissive** (additive forward
pass), a **Bruneton precomputed atmospheric sky** (the first `Type3D` consumer), a **dynamic SH
ambient**, and **BC5/BC4 channel-specialized codecs** on the role → format table; plus the
removal of planset-32's allocation-tier outer loop (it hitched).

**The next gate:** area 4 (**events/input + networking**) — multi-seat input routing and
a net layer over the `Intent`/`Authority`/Sim-View seams planset-29 established, built on the
event-routed input core (planset-30).

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
migration delivered by planset-17), and area 15's remaining **footprint specialization**
(wider ASTC, HDR ASTC, an uncompressed fallback pack — BC5/BC4 channel specialization
delivered by planset-39) + **editor active-config persistence** + the **Windows cross-compile
constraint** (the build-configuration developer-control layer delivered by planset-35). Each
becomes its own planset when taken up.
