# veng plans

Plans are grouped into numbered **plansets**, each a coherent phase of work.

- **[planset-1](planset-1/README.md)** — API rework / insulation (✅ complete,
  2026-06). Ownership, error handling, deferred destruction, vocabulary enums,
  public/backend split, render graph, ImGui module, headless context, typed
  buffers. (Plan 12, runtime shader reflection, was intentionally *not* built —
  superseded by planset-2, see below.)

- **[planset-2](planset-2/README.md)** — rendering API surface cleanup
  (✅ done, 6 plans). Push-constant layout/buffer, attachment formats from render targets,
  retiring the legacy render-pass path, and minor ergonomics. Shader-facing work
  is deferred to the future-work areas below.

- **[planset-3](planset-3/README.md)** — unit testing & test infrastructure
  (✅ done, 6 plans). Delivered the first half of the future "testing" area (area
  5a): a header-only framework (doctest), CTest wiring, a death-test harness, and a
  base suite — pure-logic, type-mapping round-trips, an extracted barrier-decision
  rule + tests, death tests, and a consolidated GPU one-exe band that skips with no
  ICD. The in-process multi-case GPU integration suite (5b) and CI stay in
  `future/`, deferred until after the de-globalize change.

- **[planset-4](planset-4/README.md)** — de-globalize the context, then finish
  testing (✅ done, 6 plans). Removed the `Context::Instance()` singleton (future
  area 3) by threading an explicit `Context&` into every resource `Create`, then —
  on the explicit-device API — delivered the deferred testing work: the in-process
  multi-case GPU integration suite (area 5b, `veng_gpu`) and a local
  `ctest -L validation` gate over the documented validation-error allowlist
  (CI with a hosted software-ICD pipeline was explicitly descoped — local only).
  Order was `5a → 3 → 5b`, as the future roadmap fixed. Stays
  single-threaded/single-context; threading (area 2) is a later planset.

- **[planset-5](planset-5/README.md)** — the synchronous asset system (✅ complete,
  2026-06). Takes up future area 1's **synchronous slice**: a per-lib project
  reorg, a standalone in-repo **cooker** (`vengc`) that turns hand-written JSON
  **asset packs** into binary **archives**, a shared `assetpack` lib, and an
  engine-side `AssetManager` that loads by opaque `u64` `AssetId` via `LoadSync`.
  Delivers the **bindless** descriptor subsystem (set 0 bound once per frame) then
  texture (stb), mesh (assimp), shader (**Slang** + offline reflection), and a thin
  handle-based material on top of it, ending with hello-triangle rendering a cooked
  pack. Cooking is offline-only (no cook-on-demand); async loading (threading) is
  the named follow-on, not in scope.

- **[planset-6](planset-6/README.md)** — threading / task system, async loads
  (✅ done, 9 plans). Takes up future area 2 and closes area 1's remaining async
  half: a `TaskSystem` (fixed worker pool + work queue returning `Task<T>`, owned
  by `Application` and threaded explicitly, pumped once per frame), a dedicated
  **transfer queue** with per-worker command pools, a `TimelineSemaphore`
  primitive, queue-family-aware ownership transfer (the `DecideBarrier` rule
  extended to emit the acquire half on first graphics use), and a transfer-keyed,
  mutex-guarded retire path (`RetireOnTransfer`) for worker-dropped staging.
  `Buffer/Image::Upload` and `AssetManager::Load` become **async by default**;
  the blocking paths survive as `UploadSync`/`LoadSync`. The `Veng.h` contract is
  revised: the render thread stays single, but work runs off it through the task
  system. Hot-reload stays future (its re-cook half conflicts with offline-only
  cooking). MoltenVK's single-queue collapse is the tested path; the dual-queue
  discrete path is exercised by the pure barrier-decision unit test.

- **[planset-7](planset-7/README.md)** — runtime primitive meshes (✅ done, 4 plans).
  A small, self-contained utility planset (not part of any future-area chain). First
  fixes the mesh's runtime material model — a `SubMesh::MaterialIndex` into a resident
  `vector<AssetHandle<Material>>` the `Mesh` owns, with `MeshLoader` eager-resolving
  cooked submesh ids into that list (superseding planset-5's "the mesh does not load
  its materials" rule). Then adds public CPU geometry (`CanonicalVertex` + `MeshData`)
  and a `Mesh::Create(Context&, const MeshData&, const string&)` upload factory
  (blocking `UploadSync`), so a runtime primitive and a cooked mesh are interchangeable
  to every pipeline and draw call. `Primitives::Cube`/`Plane`/`Sphere` generate
  `MeshData` with analytic normals/tangents/UVs and an optional material instance; the
  hello-triangle sample draws a runtime sphere carrying the brick material, no cooked
  mesh required to put geometry on screen. A runtime primitive is not an
  `AssetId`-addressable asset and never touches an archive; custom vertex layouts
  end-to-end stay future.

- **[planset-8](planset-8/README.md)** — compiled `RenderGraph` (✅ done, 4 plans).
  Takes up future area 9: moves `RenderGraph` from immediate-mode (a fresh vector of
  pass structs rebuilt every frame, every barrier re-derived per `Execute`) to a
  **compiled** graph. Splits the resource model — graph-owned **transients**
  (logical `ResourceId` handles the graph allocates and resolves per frame) vs.
  late-bound **imports** (external concrete views supplied per frame to `Execute`) —
  and replaces the bare `CommandBuffer&` callback with a typed **`PassContext`**
  (`Cmd()` + `Resolved(ResourceId)`), the record-time channel aliasing requires.
  `RenderGraph` becomes a pure builder whose **`Compile()`** bakes the
  barrier/transition schedule, transient allocation, per-graphics-pass
  `RenderingInfo`, and one-time validation into a `CompiledGraph` that **replays**
  per frame; the consumer re-`Compile()`s only on a structural change (the explicit
  recompile seam, no internal dirty flag). Transient **aliasing** lands behind a
  pure, device-free, unit-tested live-range rule (mirroring `DecideBarrier`), so
  non-overlapping transients share backing. Builds only on the shipped `RenderGraph`
  and is the enabling prerequisite for the scene renderer (area 8).

- **[planset-9](planset-9/README.md)** — game-module build model, pipeline cache
  & archive hashes (✅ done, 7 plans). Bundles three independent shipping-hygiene
  streams. **(A) Game-module build model** — a game stops being a self-contained
  exe and becomes `libhello_triangle`-style `libgame` (shared, the runtime) + a
  thin **launcher** (the shipped exe) that `dlopen`s it through one C-ABI
  `VengModuleRegister(VengModuleHost*)` entry, into which the module registers its
  `Application` factory (`ApplicationRegistry`); a `VengModuleAbiVersion` handshake
  rejects a stale module at load, and `veng_add_game` emits the lib + launcher as a
  **relocatable trio** (module resolved beside the launcher via `$ORIGIN`/
  `@loader_path`, pack + assets via `ExecutableDirectory()`). `hello-triangle` ships
  as `libhello_triangle` + a launcher and the smoke runs through it. Stream A is
  **future area 6's first sub-area** (the editor's prerequisite); its
  type-reflection layer is **deferred to the editor-shell planset**, designed
  against the inspector, and `libgame_editor`/`EditorRegistry` stay future (the ABI
  carries a reserved-null `EditorRegistry*` for them). **(B) Pipeline cache** — a
  context-owned `vk::PipelineCache` reused across every pipeline build, with opt-in
  disk persistence via `ApplicationInfo::PipelineCachePath`. **(C) Archive content
  hashes** — `.vengpack` format v2 carries a content hash per cooked blob + a
  table-of-contents digest, cooker-written (xxh3-128) and `vengc verify`-checked;
  the loader never verifies and `assetpack`/`libveng` gain no hash dependency. B
  and C each resolve a **cross-cutting concern** (pipeline caching; content hashes)
  from [future/README.md](future/README.md).

- **[planset-10](planset-10/README.md)** — scene / entity model (✅ done, 5 plans).
  Takes up future area 7's **runtime** half: a hand-rolled sparse-set ECS
  (`Scene`/`Entity`, type-erased components, queries), a reflection layer (one stable
  `TypeId` space authored like `AssetId`, `FieldClass`, `VE_REFLECT` describe-blocks
  with editor metadata, a tolerant name-keyed serializer), a transform hierarchy, a
  `Camera`, and **game-defined component types**. The cooked `.scene` asset, a systems
  framework, and the module-ABI `TypeRegistry&` registration seam are held back — the
  cooked scene and seam to area 10 (the prioritized next planset).

- **[planset-11](planset-11/README.md)** — cooker-side module reflection + the
  cooked prefab asset (✅ done, 5 plans). Takes up future area 10. The cooker
  `dlopen`s `libgame` to **reflect its native component types** — realizing the
  additive `VengModuleHost` `TypeRegistry&` seam (ABI `1u→2u`), with the registry
  now **host-owned** (the launcher/cooker constructs it, pre-registers the builtins
  via a GPU-free `RegisterBuiltinTypes`, fills it through `VengModuleRegister`, and
  threads it into the `Application`, which borrows a `TypeRegistry&`). On that, a
  **cooked prefab asset**: a `*.prefab.json` (entities + components + values) cooks
  into an `AssetType::Prefab` blob — **validated against the reflected descriptors**,
  the way materials are validated against shader reflection — that loads through the
  same `AssetManager::Load` path as every asset (a cached `AssetHandle<Prefab>`) and
  **spawns** its entities into a mutable `Scene` (`Prefab::SpawnInto`, entity
  references remapped, `AssetHandle` fields rehydrated). The cooked blob reuses
  planset-10's name-keyed `WriteFields` record encoding; the prefab-cooking path
  links `veng::veng` and reuses `ModuleLoader` (the one scoped relaxation of the
  Vulkan-free cooker). hello-triangle ships a cooked prefab it loads + spawns
  instead of building its world in code. **Supersedes planset-10 decision 4** — the
  `TypeRegistry` is now host-owned, not an `Application` member. Cross-compiled
  cooking (host ≠ target) stays out.

- **[planset-12](planset-12/README.md)** — the `SceneRenderer`, a deferred
  über-pipeline on `RenderGraph` (✅ done, 5 plans). Takes up future area 8. A
  long-lived, configurable **`SceneRenderer`** (`Unique`, single-owner) that owns an
  offscreen target and renders a `Scene` from a `Camera` through an **internal
  compiled `RenderGraph`** of reusable, self-contained **`ScenePass`** units, handing
  back a sampleable result. Its surface is the **lifetime split** —
  `Create`/`Resize`/`Configure`/`Execute`/`GetOutput` — where `Configure`/`Resize`
  rebuild + re-`Compile()` and `Execute` only replays; the per-frame `SceneView`
  reaches passes through an **opaque user-pointer** channel on `PassContext` (so
  `RenderGraph` stays scene-agnostic). On that shell it delivers a **minimal deferred
  spine** — g-buffer geometry pass (MRT albedo + world-normal + depth, written by an
  opaque material's fragment shader via a `GBufferOutput` contract) → deferred
  directional-lighting pass (→ HDR) → tonemap (HDR → output) — with a `DebugView`
  setting re-wiring the pass set as the settings-drive-recompile proof, plus a
  directional **`Light`** builtin (its `TypeId` minted with planset-11's `vengc
  generate-type-id`). hello-triangle migrates its **main view** onto one
  `SceneRenderer` (composite samples `GetOutput()`, ImGui unchanged); a two-renderer
  interleaved GPU test proves the design-for-N surface, one wired. Prerequisites met:
  the **compiled `RenderGraph`** ([planset-8](planset-8/README.md)) gives the
  builder/`Compile()`/replay lifecycle, the runtime **`Scene`/`Camera`**
  ([planset-10](planset-10/README.md)) the per-frame input, and
  [planset-11](planset-11/README.md) the `TypeId` minter. Taken up **before** the
  editor (area 6) by choice — the editor inherits the multi-viewport consumer solved.
  Held back as named future increments: the rest of the über-pipeline batteries
  (shadows, SSAO, bloom, MSAA, transparent/forward pass, post stack), multiple/typed
  lights + light culling, and parallel pass recording. **Frames-in-flight > 1 is
  delivered in planset-13** by a cross-graph reuse barrier (ring-buffered output
  reserved for a future temporal/async consumer).

- **[planset-13](planset-13/README.md)** — `SceneRenderer` frames-in-flight
  correctness + roadmap re-cut (✅ done). Corrects the false single-in-flight comment
  and closes the cross-graph output reuse hazard with a renderer-owned
  `PrepareForAccess(ColorAttachment)` barrier recorded before each `Execute` — the
  reverse of the consumer's `Sample` transition. The output stays **single-copy** with
  **zero added memory**; a per-frame-in-flight ring is rejected and documented as a
  future escalation (a temporal/history-buffer consumer reading an older frame, or a
  handoff side moving off the single graphics queue). The barrier suffices without a
  semaphore or ring because both halves of the handoff record on the single graphics
  queue in submission order. **Supersedes planset-12 decision 6** (which deferred FIF >
  1 to a ring).

- **[planset-14](planset-14/README.md)** — editor shell + framework (✅ done, 5 plans).
  Takes up [future area 6](future/README.md#6-editor-application) **sub-area B**: the
  first tangible authoring environment. Delivers `libveng_editor` (the editor framework
  library — `EditorPanel` with a `Title()`/`OnImGui()` interface, `EditorRegistry`, and
  `EditorHost`, an `Application` subclass that builds a top-level single-window
  `DockSpace`), a `veng_add_editor` CMake macro (parallels `veng_add_game`, emitting
  `lib<name>_editor` + `<name>-editor`), and a docking host with built-in panels (scene
  viewport, asset browser, inspector, console/log). The **reflection-driven inspector**
  walks a selected entity's components through the host-owned `TypeRegistry` /
  `FieldDescriptor` layer, drawing a built-in widget per `FieldClass`
  (Scalar/Vector/Quaternion/String/AssetHandle/Enum/Struct/Matrix/Reference) with custom
  overrides via `EditorRegistry::RegisterFieldWidget` — reading
  `Scene::ForEachComponent(Entity, fn)`, added this planset. **Cook-on-demand** runs
  off-thread: `libveng_cook` is linked only into the editor exe (not `libveng_editor`,
  not `libgame`), exposed through an injected `CookBackend` so `EditorHost::RequestCook`
  cooks a single source via `TaskSystem` and hot-reloads the result behind the stable
  `AssetHandle` through `AssetManager::MountMemory` (a RAII `MountHandle` shadow-mounting
  an in-memory archive). The **texture editor** (`TextureEditorPanel`) is the first
  end-to-end asset editor: a preview RT via `CreateTexture`/`ImGui::Image`, `.tex.json`
  settings editing (sRGB + sampler filter/wrap), 300ms-debounced live recook, and a JSON
  round-trip on save that preserves unknown keys. `hello_triangle-editor` launches with
  `libhello_triangle`, shows the scene in a docked viewport, and opens the brick texture
  in the texture editor. Held back as named future plansets: the material node editor
  (sub-area C), the scene editor (sub-area D), an undo/redo command stack, and
  multi-viewport OS windows.

- **[planset-15](planset-15/README.md)** — node-based material editor (✅ done, 7 plans).
  Delivers [future area 6](future/README.md#6-editor-application) sub-area C, the **loaded
  `.vmat` path**: a node-graph material editor on a new **named `libveng_editor` surface**
  (`VengEditor/NodeGraph/`), over a precursor that splits material parameters into a fixed
  **engine-supplied** block and a variable-size **authored** block (so a graph can author
  uniforms beyond the one `vec4 Factors`). Four node-graph layers with hard dependency
  ceilings — a pure, device-free, unit-tested **topology core** (`NodeGraph`, generational
  `NodeId`, `PinType`, the mutation vocabulary, direction/arity/acyclicity validation, a
  domain-supplied `CanConnect` hook); a generic **data-driven catalog** (`NodeType` = pins +
  a reflected property struct, node instances stored as bytes walked by the existing
  reflection serializer/inspector widgets, graph (de)serialization to/from JSON); the
  **material specialization** (the `TextureSample`/`Param`/`MaterialOutput` catalog,
  coercion-aware connection, `CompileMaterialGraph` → a `.vmat` field list, and a
  flat-`.vmat`→graph import); and the **`MaterialEditorPanel`** (imnodes canvas,
  node-property inspector reusing the per-`FieldClass` widgets, live
  compile→cook→hot-reload→preview against a reusable `MaterialPreview` sphere). Textures are
  node **properties** of `FieldClass::AssetHandle`, not wired pins (the topology core stays
  asset-agnostic); the graph is embedded under an `"_editor"` key in the `.vmat.json` and
  compile regenerates the `fields` array, reusing planset-14's preserve-unknown-keys
  round-trip. v1 binds parameters to an author-provided shader (no node→Slang codegen).
  hello-triangle's `brick.vmat` opens, edits, previews live, and saves through the editor.
  imnodes is used only by the editor (linked PRIVATE into `libveng_editor`, src-only — never
  a `VengEditor/` public header; its symbols are vendored in `libveng`'s ImGui aggregation
  TU). Held back: shader-graph codegen, wired asset pins, undo/redo, and the scene editor
  (sub-area D).

- **[planset-16](planset-16/README.md)** — grab bag: reflection trait, asset-library
  rename, ImGui composite (✅ done, 4 plans). A small cleanup planset (not a
  future-area chain) bundling three independent wins. Win 1 collapses the reflection
  layer's two type-identity traits (`ReflectLeaf<T>` for leaves/enums, `VengReflect<T>`
  for structs) into a single `VengReflect<T>` every reflected type specialises
  uniformly, deletes the `IsReflectLeaf` SFINAE and the parallel
  `TypeRegistry::EnsureLeaf<T>()` path, and adds a `VE_LEAF` authoring macro beside
  `VE_TYPE`/`VE_REFLECT` — behaviour-preserving (no `TypeId`, `FieldClass`, or on-disk
  format change). Win 2 renames the shared archive library to `assetpack` (its
  CMake target and `veng::` alias), retiring the old name.
  Win 3 adds an engine-provided `ImGuiCompositePass` for the scene-behind-ImGui composite.

- **[planset-17](planset-17/README.md)** — the `Veng::UI` toolkit (✅ done, 6 plans).
  Takes up [future area 12](future/README.md#12-ui-toolkit--vengui): fronts ImGui with a
  thin, opinionated, engine-tier **`Veng::UI`** vocabulary (in `libveng`,
  `engine/include/Veng/UI/`) so UI is authored against an engine surface rather than raw
  `ImGui::` at the call site — for game modules *and* the editor both. The base vocab is one
  `Drag` overloaded on `f32`/`vec2`/`vec3`/`vec4`/`i32`, designated-initializer options
  structs (`DragOptions`/`SliderOptions`) instead of `ImGui*Flags`, `fmt`-preformatted
  `string_view` text, edit widgets returning `[[nodiscard]] bool`, and RAII scope guards
  (`Window`/`TreeNode`/`Table`/`Menu`/`PushId`/`StyleVar`/…) for every begin/end and push/pop
  pair — with **imgui-free public-header signatures** (`<imgui.h>` only under
  `engine/src/UI/`, kept within `include_hygiene`). Then the **complete migration** of every
  widget-authoring `ImGui::` site: hello-triangle's game-module debug panel to **zero** raw
  `ImGui::` (the game-tier proof), the reflection inspector's per-`FieldClass` dispatch routed
  through one overloaded `Drag` (the `Vector` three-way switch gone), and the editor's panels
  + menu bar onto the engine surface. **Wrapper-only:** ImGui stays a PUBLIC dependency this
  round; the imgui-free headers already meet the contract a future "drive imgui private"
  planset needs. ImGui's frame lifecycle and host/dock/present plumbing stay raw in
  `ImGuiLayer`/`EditorHost` (the integration-layer boundary), and the thin key/mouse-button
  query sites stay raw, flagged to converge with the event/input area. Held back: the stateful
  editor-widget-class pattern (a `FileBrowser`-style `Draw()`-returns-event), taken up when a
  widget that holds persistent state needs extracting; and driving ImGui fully private.

- **[planset-18](planset-18/README.md)** — material parameter storage, domains, and the
  PostProcess fullscreen-material path (✅ done, 8 plans). Takes up
  [future area 13](future/README.md#13-material-domains--shader-graph-codegen--prioritized)'s
  **prioritized first slice**. First **reworks material parameter storage**: the fixed engine
  `MaterialData` block and its separate set-0 SSBO are deleted, and a material's bindless
  handle slots + authored params share **one reflection-sized block** per material (set 0
  binding 4), patched by reflected offset + `CookedMaterialField::Kind`, so a material carries
  an arbitrary, shader-defined handle set. `CookedMaterialHeader` gains `Version`
  (`CookedMaterialVersion`, now `2`) + `Domain` + `BlockBytes`; a stale blob rejects loudly.
  The block buffer is **N-buffered (frames-in-flight), host-visible + persistently mapped, and
  ring-written via a dirty-flush**, so a per-frame `SetParam`/`SetTexture` is a direct,
  stall-free write — the current frame's region selected by **folding the frame base into the
  pushed material selector index** (`BindlessRegistry::GetCurrentFrameBase()`), not a dynamic
  descriptor offset (which mistranslates inside set 0's bindless Metal argument buffer on
  MoltenVK). On that foundation a `Material` gains a first-class **`MaterialDomain`**
  (`Surface`/`PostProcess`) — the lowercase `"domain"` `.vmat.json` key (default `surface`) —
  with a cook-time domain↔fragment-output contract check (Surface → g-buffer MRT; PostProcess
  → single `SV_Target0`), and the engine ships the **standard vertex shader per domain** in the
  core pack (`surface.vert`, `fullscreen.vert`; `material.slang` holds the shared declarations),
  the example referencing the core `surface.vert` instead of its own. A **`PostProcessScenePass`**
  stands up the fullscreen-material path in `SceneRenderer` — builds a `GraphicsPipeline` from a
  PostProcess material's fragment shader against a renderer-supplied color format, binds set-0
  bindless, runtime-binds an upstream target as a material handle field, and drives authored
  params — and **tonemap becomes the first PostProcess material** (core `tonemap.vmat`, HDR
  runtime-bound, `Exposure` an exposed per-frame param); the swapchain composite and `DebugView`
  blits stay hardcoded engine passes. The material **node catalog is domain-aware**
  (`MaterialOutput`'s pins follow the domain's output contract). **Node→Slang codegen** — every
  node an expression emitter generating the fragment source — stays the named follow-on planset.

- **[planset-19](planset-19/README.md)** — PBR `SceneRenderer` + über-pipeline batteries
  (✅ done, 7 plans). Takes up
  [future area 8](future/README.md#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries)'s
  named **batteries** increment, folding in the **G2 PBR g-buffer target** the area reserved.
  Turns the minimal deferred spine into a **physically-based deferred renderer**: the g-buffer
  grows a packed **ORM** target (`float4 ORM : SV_Target2` — occlusion/roughness/metallic +
  scalar emissive), `MaterialParams` becomes metallic-roughness, and the Lambert pass is
  replaced by **Cook-Torrance** GGX reconstructing world position from depth, behind a
  ring-buffered **view-constants buffer** (set-0, 512-byte stride — per-view data rides the
  buffer, never push constants). On that spine: **tangent-space normal mapping**, **multiple
  typed lights** (directional/point/spot, a ring-buffered light list the pass loops over), a
  **directional shadow map** (depth-only `ShadowScenePass` + manual PCF, a fixed-size ortho box
  since no scene-bounds facility exists yet), **SSAO** (a fullscreen pass folded into the
  occlusion term), and **bloom authored as a PostProcess material** (the first multi-stage
  authorable post chain: bright-pass → separable blur → composite ahead of tonemap). Each
  battery is a `SceneRendererSettings` toggle driving the `Configure` recompile, and a
  **`DebugView` arm** visualizes every new channel (the packed-ORM `Roughness`/`Metallic`/
  `Occlusion`, the `AO` target, the directional `Shadows` map). **Scene/mesh AABB + bounds** is
  named the next prerequisite (the gate on a tight shadow fit and CSM), and **on-tile/
  subpass-fused deferred** is recorded as a measure-first `RenderGraph`-core change, not a
  battery. The remaining renderer increments — a transparent/forward pass, shadowed punctual
  lights, colored emissive, CSM, and clustered light culling — stay future.

- **[planset-20](planset-20/README.md)** — scene/mesh AABB + bounds, cascaded shadow maps
  (✅ done, 6 plans). Stands up the engine's first **bounds facility** and on it delivers
  **cascaded shadow maps** for the directional light — cashing in the prerequisite
  [planset-19](planset-19/README.md) named (a tight shadow fit and CSM both gate on a real
  scene/mesh AABB) and taking it all the way to CSM rather than the intermediate single-map fit.
  **Foundation-first:** an `AABB` glm-only math primitive in a new `Veng/Math/` home (min/max
  `vec3` pair, union/expand/center/extents/corners/transform algebra, empty sentinel), a
  **local-space bound per `Mesh`** folded from its canonical vertex positions at load (no
  cooked-format change), and a **world-space `SceneBounds(scene)`** unioning every resident
  `(Transform, MeshRenderer)` world bound via `ComputeWorldMatrices` (recompute-on-demand, no
  dirty-flag cache); then a pure, device-free **`ComputeCascades`** turning the camera, light,
  and scene bound into per-cascade light-space matrices + split distances (PSSM log/uniform
  split blend, bounding-sphere fit + texel snapping to kill shimmer, the scene bound extending
  each cascade's near plane toward the light) — both halves fully unit-tested with no ICD. On
  that analytic core: the cascades render into a D32 **atlas** sized to `CascadeCount` (a
  `min(Count,2)×ceil(Count/2)` tile grid, `ShadowResolution²` per tile, default 1024) in **one**
  pass via per-cascade viewports. **Shadows leave bindless:** the closed producer→consumer
  shadow resource moves to a **dedicated set 1** carrying the whole directional-shadow system —
  the atlas, an **immutable comparison sampler**, and a per-frame `ShadowConstants` block
  (`CascadeViewProj[4]` + `CascadeSplits` + `ShadowParams`) bound as a **dynamic uniform** — so
  the lighting pass selects the cascade by view-space depth, remaps to the atlas tile, and uses
  hardware **`SampleCmp`** with a boundary cross-fade, replacing planset-19's manual in-shader
  PCF at its root (a comparison sampler is barred from set 0's Metal argument buffer on MoltenVK,
  but is standard in a dedicated set). Net-new, reusable descriptor infrastructure lands with it
  — immutable samplers, `UniformBufferDynamic` + the `pDynamicOffsets` bind path, and the
  `PassIO` **bound-view** seam delivering a producer's view into a consumer's dedicated set — and
  the set-0 view-constants block is trimmed to material-facing camera/view state (the view/shadow
  shader headers split into `view_constants.slang` + `shadow.slang`, fixing a latent SSAO
  stride/offset bug). A **`DebugView::Cascades`** arm and `CascadeCount`/`CascadeSplitLambda`/
  `ShadowResolution` settings expose the CSM surface. **Shadowed punctual lights** (point/spot
  cubemap/atlas) and **frustum culling** (the other prime consumer of mesh bounds) are the named
  next increments behind the delivered facility; a cached/dirty-tracked scene bound or a BVH is
  the scaling step they share, and the single-pass depth-**array** CSM render path (multiview /
  layered, a quality not perf change on MoltenVK) stays a [future](future/scene-renderer.md)
  follow-on gated on a `RenderGraph` layered-pass seam.

- **[planset-21](planset-21/README.md)** — view-frustum culling (✅ done, 4 plans). Cashes in
  the **other** prime consumer of the [planset-20](planset-20/README.md) bounds facility — the
  one planset-20 named ("**frustum culling** (the other prime consumer of mesh bounds) is the
  named next increment"). **Foundation-first**, mirroring planset-20: a **`Frustum`** glm-only
  math primitive beside `AABB` (`Veng/Math/Frustum.h`) — six bounding planes extracted
  **Gribb-Hartmann** from a view-projection matrix, adapted to **Vulkan ZO clip** (the one
  extraction subtlety the test pins), with a conservative `Intersects(Frustum, AABB)` p-vertex
  test (false only when a box lies wholly outside one plane — never a false cull) — and a
  per-frame **`GatherMeshes`** visibility gather (`Veng/Scene/Visibility.h`) reducing a `Scene`
  to its resident `(Transform, MeshRenderer)` instances (world matrix + world-space `AABB` +
  resident mesh) in one `ComputeWorldMatrices` pass, **subsuming `SceneBounds`** (the union bound
  falls out free); both pure, device-free, and fully unit-tested before any pass consumes them.
  On that core, `SceneRenderer::Execute` gathers **once** into renderer scratch (like the light
  pack) and points `SceneView` at a `std::span<const VisibleMesh>`; the **g-buffer geometry pass
  culls that span by the camera frustum** and the **cascaded shadow pass by each cascade's light
  frustum** (an off-screen caster can still shadow into view, so it must not cull by the camera
  frustum), each draw using the gathered world matrix in place of a per-draw `WorldMatrix`
  re-walk. A `SceneRendererSettings::FrustumCull` knob (default on) toggles it; an example debug-UI
  checkbox + a drawn/total readout (`GetLastDrawnCount()`/`GetLastVisibleCount()`) expose it.
  Culling is a pure optimization — the rendered image is **byte-identical**, so the
  **`smoke_golden` never moved**; a **draw-count** test over a fixture with an off-frustum mesh is
  the guard (a green golden proves nothing about whether culling ran). The gather is the **BVH
  seam**: a **cached/dirty-tracked scene bound or BVH** is the immediate scaling step it shares
  with the `SceneBounds` reduction, and occlusion / per-submesh / GPU culling are the named
  refinements — each behind this delivered foundation.

- **[planset-22](planset-22/README.md)** — recoverable reflection deserialization
  (✅ done, 2 plans). Makes the reflection serializer's **read** side **recoverable**: the
  `ReadFields` truncation/drift guards return a `VoidResult` (`std::unexpected`) instead of a
  fatal `VE_ASSERT`, and the prefab loader propagates a malformed/truncated record as the
  structured `AssetError::Corrupt` it already raises for its own range checks — drawing the line
  veng's error policy implies (untrusted input is recoverable; the schema lookups stay fatal API
  misuse). `WriteFields` is untouched; a `gpu`-band test feeds a truncated cooked prefab blob and
  asserts `LoadSync` returns `Corrupt`, not an abort.

- **[planset-23](planset-23/README.md)** — frustum-cull BVH broadphase (✅ done, 4 plans).
  Cashes in the **shared scaling step** planset-20/21 both named — a **bounding volume hierarchy**
  over the resident draw candidates behind the stable `GatherMeshes` seam, so frustum culling stops
  being a per-view linear scan. A renderer-owned **`SceneBroadphase`** holds the tree and rebuilds it
  only on a frame the scene's spatial state moved, gated by a single **`Scene::GetSpatialVersion()`**
  counter (bumped on spatial-pool — `Transform`/`Parent`/`MeshRenderer` — structural change and
  **non-`const`** access, never on a `const` `View`/`Each` or other pools; a `const` iteration path
  lands with it, dropping the renderer's light-pack `const_cast`). The camera and every shadow cascade
  query the **one tree** by descent — the workload planset-24's per-light shadow views most reward.
  The BVH and the version gate are both pure, device-free, unit-tested (query == linear tight scan
  over randomized builds and frustums) before the renderer consumes them; the rendered image is
  **byte-identical** (the golden never moves; a rebuilt-this-frame stat is the evidence). `DestroyEntity`
  becomes **recursive** (destroys the subtree) with it. **Incremental tree maintenance**,
  **GPU/occlusion culling**, **per-submesh leaves**, and **a Scene-shared tree** are the recorded
  refinements behind the same `Sync`/`Cull` + version-gate seam.

- **[planset-24](planset-24/README.md)** — shadowed punctual lights (✅ done, 5 plans).
  The other named increment behind the delivered `AABB`/`Frustum`/gather facility: it extended the
  shadow system from **directional-only** to **point and spot**. A bounded budget of punctual
  lights (a `MaxShadowedPunctual` constant) cast real shadows — a **spot** through one perspective
  map, a **point** through a six-face **cube** — sampled in the deferred lighting pass with hardware
  `SampleCmp` + PCF and multiplied into each light's contribution. **Set 1 generalized** from "the
  directional system" to "a shadow system" (directional cascades + a shared punctual atlas +
  per-light shadow records), and each shadow view culls its casters off the **shared broadphase**
  against its own frustum (a spot frustum, six cube-face frustums) — the many-shadow-view workload
  that most rewards planset-23's BVH broadphase, its **delivered prime consumer**. **Built on
  planset-23** (one tree, queried once per shadow view); foundation-first (the punctual shadow-view
  math is pure/device-free, `Veng/Renderer/PunctualShadows.h`). The image changed — the **golden was
  regenerated once**. **Clustered/tiled light culling**, **cached/static shadow maps** (the
  highest-value follow-on), and **per-light dynamic resolution / shadow LOD** stay named next.

- **[planset-25](planset-25/README.md)** — occlusion + GPU/compute-driven culling (✅ done,
  6 plans). Takes planset-21's mesh-granularity **CPU** frustum cull all the way to
  **GPU-driven** — the deepest culling refinement. Staged: **per-submesh bounds** + a CPU
  per-submesh cull (a pure refinement, bounds derived at load with no cooked-format change);
  a **hi-Z depth pyramid** (compute **max-Z** mip reduction) + a conservative temporal
  **occlusion test** (drop only the provably hidden, never a visible draw); then **indirect-draw
  infrastructure** (`vkCmdDrawIndexedIndirect` over a `multiDrawIndirect` command buffer — the
  `drawIndirectCount`-free shape MoltenVK supports, net-new engine capability, dev-box-verified)
  and a **compute cull → indirect draw** path that uploads the broadphase's frustum survivors,
  runs **occlusion only** GPU-side (the BVH already did frustum), and gates each draw by writing
  its command's `instanceCount` (1 survivor / 0 culled — no compaction). The CPU and GPU paths
  share one **buffer-indexed** draw (per-instance transform/material, candidate index via
  `firstInstance`), differing only in direct-vs-indirect submission. A `CullMode` setting selects
  CPU/GPU with the CPU path as fallback where `multiDrawIndirect`/`drawIndirectFirstInstance` is
  absent. The image stays **byte-identical** (frustum-identical; occlusion drops only hidden
  draws); draw-count + GPU↔CPU set-equivalence fixtures are the guard. Meshlet/cluster culling,
  two-pass occlusion, GPU compaction into a count buffer, and GPU-driven shadow culling are named
  next; the GPU candidate set is the natural consumer of the BVH broadphase. **Independent of
  planset-24**; builds on planset-23.

- **[planset-26](planset-26/README.md)** — primitive recipes in prefabs (✅ done,
  8 plans). Lets a prefab store the **recipe** of a procedural mesh ("icosphere, radius
  0.8, brick material") rather than a baked mesh or a dangling runtime handle, load it
  through the ordinary asset/prefab path, select it in the editor, and **async-stream** the
  regenerated `Mesh` into the entity a few frames later. The enabling deliverable is a
  reusable **`FieldClass::Variant`** — the first reflection sum type understood end to end:
  the engine serializer (a `TypeId` tag + the active member's record), the cooker
  (`{ "type", "value" }` JSON, validated against the alternatives), and the editor inspector
  (a combo + the active member's fields). On top of it: a `PrimitiveComponent` whose
  `Variant<CubeShape, PlaneShape, SphereShape, IcosphereShape>` is the recipe; an async
  `Mesh::CreateAsync` (built on planset-6's host-visible `Buffer::Upload`) and an
  `AssetManager::CreateAsync` (a pending **detached** cache entry finalized through the
  continuation pump) with a caller-owned `PrimitiveMeshCache` deduping identical primitives
  to one upload; and a `ResolvePrimitiveMeshes` that fills `MeshRenderer.Mesh` from the
  active shape. Builds
  on **planset-7** (the `Primitives::` generators) and **planset-6** (async upload + pump);
  `MeshRenderer` and the cooked mesh format are unchanged.

- **[planset-27](planset-27/README.md)** — self-resolving components (the spawn-resolve
  thunk) (📝 proposed, 7 plans). Closes the one gap in "load an asset or spawn a prefab
  and every dependent streams in with no further intervention": a cooked dependency graph
  already auto-streams, but a **generated** resource (a `PrimitiveComponent`'s mesh — a
  recipe, not an `AssetId`) cannot ride the cascade. The deliverable is a generic
  **per-component spawn-resolve thunk** on `TypeInfo` — the same optional-function-pointer
  mechanism as the variant thunks — that `Prefab::SpawnInto` fires automatically after a
  component is populated, with a public `ResolveComponents(Scene&, Entity, AssetManager&)`
  for the editor's add/edit paths. `PrimitiveComponent` becomes the first rider over a free
  `CreatePrimitiveMesh(AssetManager&, const PrimitiveShapeVariant&)`, and
  [planset-26](planset-26/README.md)'s `ResolvePrimitiveMeshes` scan + caller-owned
  `PrimitiveMeshCache` + orphan-prune are **deleted** (dedup becomes the consumer's to
  share; the editor moves from a per-frame scan to event-driven triggers). Folds in
  the four deferred primitive shapes — **cylinder, cone, torus, capsule** — and a
  **component naming pass** (every component a bare noun: `PrimitiveComponent` →
  `Primitive`, `CameraComponent` → `Camera`, the value-type `Camera` → `CameraView`,
  the rule codified in CLAUDE.md). Finishes and splits the asset-factory surface:
  **runtime async build factories for `Texture`/`Material`** (the `Task<Ref<T>>`
  siblings of `Mesh`'s), then the **`Create` / `Build`** line — low-level GPU resources
  construct from a descriptor (`Create(const XInfo&)`, sync), higher-level engine
  assets are produced from CPU data (`Build` async / `BuildSync` blocking), and
  `AssetManager::CreateAsync` folds into an `Adopt(Task)` overload. No on-disk format or
  cooked-artifact change (`TypeId`s are stable across the renames); the only new
  persisted types are the four shape recipes.

- **[planset-28](planset-28/README.md)** — mip-pyramid bloom (compute dual-filter)
  (✅ done, 5 plans). Replaces the fixed single-level separable-Gaussian bloom
  (planset-19's four full-resolution `PostProcessScenePass` material stages) with a
  **compute mip-pyramid bloom** modeled on the delivered hi-Z compute mip reduction: a
  bright-pass + **Karis-average** firefly suppression into mip 0, a progressive downsample
  down an HDR mip chain, an in-place accumulating upsample, and a composite into linear HDR
  ahead of tonemap — the wide, soft, energy-conserving multi-octave glow the single-level
  blur cannot produce, for **less bandwidth** (one geometric pyramid vs. four full-res
  images, almost all work at ≤¼ resolution). The down/up filter is a **selectable kernel** —
  `BloomKernel::Cod` (the 13-tap/tent dual filter, default and the golden's kernel) or
  `BloomKernel::Kawase` (the bandwidth-optimized Dual Kawase filter for tile-based GPUs) —
  and `Threshold`/`Intensity`/`Radius` are per-frame `SceneView` knobs (`Bloom`/`BloomKernel`
  the topology `SceneRendererSettings` knobs).
  **Supersedes planset-19 decision 6** (bloom is a PostProcess material) and retires the
  planset-18 "first multi-stage authorable post chain" framing — bloom becomes a fixed
  compute battery like SSAO and the shadow atlas (tonemap stays the authorable-material
  exemplar), with a `DebugView::Bloom` arm. Reuses the `CreateHiZ` pattern (one mip-chain
  image, per-mip storage views, per-level dispatch + barriers). Also completes the
  hello-triangle "Scene" debug window's coverage of the renderer's tunable surface — wiring
  the previously-unexposed **Exposure** (relocated to `SceneView` for live tuning) and
  **bloom** (toggle / threshold / intensity / radius / kernel) controls. **Single-pass
  downsample (SPD)** and a separate **FFT/convolution lens bloom** are named future area-8
  increments.

- **[planset-29](planset-29/README.md)** — gameplay control: cameras, input→intent, sim/view
  split, game modes, levels (✅ done, 10 plans). Built the gameplay layer **and the authoring
  surface to wire an actual game**, **from ECS first principles** — shaped by veng's data-oriented
  grain and the **networking it anticipates**, not by an actor hierarchy. *Runtime primitives
  (00–04):* a `Camera` entity selected per **`Viewer`** seat and resolved by the pure
  **`ResolveCameraView`**/**`ResolvePrimaryCameraView`** into the `CameraView` the renderer already
  consumes (renderer untouched); player control as **Input → Intent → Movement** (`PlayerInput` →
  `Intent` → `MovementSystem`, so AI and remote players are drop-in intent producers and the sim is a
  pure function of state + intents, possession via `Possesses` independent of view); a **Sim / View
  tick split** on `SceneSystem`'s `Phase { Sim, View }` driven by `SceneSimulation`, separating
  deterministic/replicable simulation from client-local view derivation (the `CameraRigSystem` the
  first View system); an **`Authority { Tier, Owner }`** annotation marking ownership ahead of the
  net layer; and a **game mode** as a **`Session`** state component + **rule systems** + a
  `GameModeConfig` field (no object, no registry, no ABI bump). *Authoring layer (05–08):* gameplay
  systems written in code and discoverable through a host-owned **`SystemRegistry` catalog**
  (`VE_SYSTEM`/`SystemId`), a thin **`Level`** asset (`AssetType::Level`/`CookedLevelHeader`)
  wrapping a world prefab with the level-scoped wiring (game mode, ordered system set, render
  settings) that **loads into play** via `Level::LoadInto`, a cooker **`LevelImporter`**, a
  **`LevelEditorPanel`** that authors it, and a **`docs/guides/`** tutorial for writing gameplay
  systems — so a game is assembled as data, not hardcoded in `main.cpp`. (Re-cut from an
  Unreal-derived first draft — a `PlayerCameraManager`/`PlayerController`/`GameMode` trinity with a
  registered `GameMode` type and an ABI bump — which imported actor-network structure into an ECS and
  froze the most net-model-specific layer ahead of any net decision; the ECS-native design carries
  **no ABI change**, module ABI stays at version 3.) **Multi-seat input routing** (split-screen,
  AI-vs-player) and the **networking layer** that consumes intent / authority / the sim-view split
  are the **next gate**, named [future area 4](future/README.md#4-event--input-systems)
  increments behind the seams this planset establishes.

- **[planset-30](planset-30/README.md)** — event-routed input with a focus stack. The first
  half of [future area 4](future/README.md#4-event--input-systems): the `Window` becomes the
  single typed-`Event` source, an **`InputRouter`** routes each event to consumers by a **focus
  stack**, and `Input` becomes an event-fed snapshot rather than a global poller. **ImGui is one
  routed consumer** (backend callbacks off; the router forwards under UI focus), so pushing
  **gameplay focus** (the editor's Play, the shipped sample) makes the running game the exclusive
  input owner and captures the cursor until **Shift+Esc** (or window-focus loss) releases it —
  input is finally restricted to the viewport while a game runs. **Multi-seat input routing** and
  the **networking layer** stay the named next area-4 increments, now built on this router.

- **[planset-31](planset-31/README.md)** — viewports: a view owns its rectangle, the engine drives
  the list. A **`Viewport`** (over the delivered `SceneRenderer`) **owns its `ViewportRegion`** (its
  window rect) and renders into its own texture; the engine drives a **central drive-list** of them
  each frame. A **role** gates compositing — `Presented` (a **gather pass** scissor-blits it into its
  region on one assembly target the composite then encodes) or `Offscreen` (a consumer samples it: an
  ImGui panel, or a material via `GetOutputHandle` → `Material::SetTextureHandle`). **RTT is the
  floor**: the game's view, a
  **splitscreen** quadrant, an editor panel, and a monitor are one mechanism. The shipped sample
  drops its hand-wired `SceneRenderer`/composite/sampler/texture boilerplate onto a **managed
  primary viewport** (region = whole window), and the editor's per-panel viewports move onto the
  same list (panels own, the engine drives), feeding their region from the ImGui content rect — so
  *"any number of renderers, including zero"* is one `vector`. Owning the region yields a
  **window↔view mapping** (`WindowToViewport` / `ScreenToWorldRay`): editor picking now, and the
  *pointer* half of **multi-seat input** routing later (the *device* half is gamepad-by-id,
  independent of viewports). The viewport slice of
  [area 8](future/README.md#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries),
  the render-driving slice of [area 6](future/README.md#6-editor-application), and the pointer-routing
  seam of [area 4](future/README.md#4-event--input-systems). **Declared inter-viewport
  dependencies**, **output ringing** for an async/off-queue consumer, and a playable
  **splitscreen / monitor sample feature** stay the named follow-ons.

- **[planset-32](planset-32/README.md)** — the render allocation sizes itself. Dynamic resolution
  already has a fast inner loop (`ComputeDynamicResolutionScale` eases the per-frame sub-rect
  `RenderScale` toward a GPU-frame-time budget); this planset adds the **slow outer loop** that sizes
  the **allocation** those targets live in. A pure, device-free **`StepAllocationTier`** (beside the
  inner-loop controller in `DynamicResolution.h`) folds a multi-second EMA of the sub-rect scale into a
  **quantized allocation tier** through a **hysteresis band** + **asymmetric dwell timers**, and the
  `Viewport` debounces a `SceneRenderer::Resize` on a tier change — so the hand-picked `MaxScale`
  allocation goes away and the allocation tracks what the scene sustains **without thrashing** (the
  expensive knob never reacts to a fast signal; a tier change keeps the rendered pixel count constant, so
  reallocation does not pop). Separately, a **`MaxAllocationScale`** cap decouples the allocation
  baseline from the **swapchain framebuffer extent**, so a small window on a 2× **HiDPI** display is not
  silently supersampled across every render-graph image — the steady-state fix the originating MoltenVK
  perf problem needed (scaling the sub-rect barely helped because the allocation footprint and the
  full-allocation tail — tonemap/upscale, gather + composite, TAA history-copy — did not shrink with it).
  **Safe-moment reallocation**, a **memory-budget-driven initial tier**, and a **TAA history ping-pong**
  stay the named follow-ons.

- **[future](future/README.md)** — work beyond the current plansets (📝 draft/vision,
  holding area; not a planset). Area 13's **prioritized first slice** — material
  **domains** (Surface + PostProcess), the unified ring-buffered parameter block, the
  PostProcess fullscreen-material path, and the domain-aware node catalog — landed in
  planset-18; its named follow-on, **node→Slang codegen** (every node an expression
  emitter generating the fragment source), and the **editor's scene editor** (area 6,
  sub-area D — its gates met by planset-10/11/12/14/15) are the **prioritized** next
  areas, whichever the next planset takes up. The rest of the remaining work is the
  **multi-seat input routing + networking** half of area 4 (its event-routing core landed in
  planset-30) and the named still-future increments of the areas
  done in part (hot-reload; the task graph; the systems framework +
  `ShaderInterface`/`MaterialField` unification; the über-pipeline batteries — now with
  PostProcess materials as the authorable-effect mechanism — + typed lights; render-graph
  culling/multi-queue; cross-compiled cooking). Each becomes its own planset when taken
  up. The future README carries the detail.
