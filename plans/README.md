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

- **[future](future/README.md)** — work beyond the current plansets (📝 draft/vision,
  holding area; not a planset). Area 13's **prioritized first slice** — material
  **domains** (Surface + PostProcess), the unified ring-buffered parameter block, the
  PostProcess fullscreen-material path, and the domain-aware node catalog — landed in
  planset-18; its named follow-on, **node→Slang codegen** (every node an expression
  emitter generating the fragment source), and the **editor's scene editor** (area 6,
  sub-area D — its gates met by planset-10/11/12/14/15) are the **prioritized** next
  areas, whichever the next planset takes up. The rest of the remaining work is the
  **event/input** systems (area 4) and the named still-future increments of the areas
  done in part (hot-reload; the task graph; the systems framework +
  `ShaderInterface`/`MaterialField` unification; the über-pipeline batteries — now with
  PostProcess materials as the authorable-effect mechanism — + typed lights; render-graph
  culling/multi-queue; cross-compiled cooking). Each becomes its own planset when taken
  up. The future README carries the detail.
