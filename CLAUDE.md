# veng

A C++26 Vulkan rendering engine. Built as a shared library (`libveng`) with a
public API under `engine/include/Veng/` and a Vulkan backend hidden behind it.
Primary dev platform is macOS via MoltenVK; the code is written to be portable
(a Windows port is anticipated, hence `VE_API`).

**The render thread is single.** The render `Context` is constructed explicitly
by `Application` and threaded into every resource; `Context::BeginFrame`/
`EndFrame`, draw recording, `Time`, input, and the ImGui integration all assume
one driving thread. Work runs off the main thread only through the `TaskSystem`
(decode + upload on a worker, the result landing back on the main thread via the
continuation pump). Direct concurrent calls into veng APIs from outside the task
system are illegal.

## Layout

Each library lives in its own root subdirectory; the top-level `CMakeLists.txt`
is thin (shared deps + `add_subdirectory` per lib).

- `engine/` — `libveng`, the runtime. Links only `assetpack` (loader, no
  importer deps).
  - `engine/include/Veng/` — public headers. `Veng.h` is the foundational
    header every other header builds on (std/glm includes + house-style
    aliases).
  - `engine/include/Veng/Renderer/` — public renderer API (`Context`,
    `Buffer`, `Image`, pipelines, `DescriptorSet`, `RenderGraph`, …).
  - `engine/include/Veng/Renderer/Backend/` — backend-only headers
    (`Vulkan.h`, `Natives.h`, `TypeMapping.h`). **Not** part of the
    consumer-facing surface.
  - `engine/src/Renderer/Backend/` — the Vulkan implementations of the public
    renderer classes. (The public class lives in
    `engine/include/Veng/Renderer/X.h`; its impl lives in
    `engine/src/Renderer/Backend/X.cpp` — note the path asymmetry.)
- `assetpack/` — `libveng_assetpack`, the shared archive + cooked-blob
  format (`Veng/Asset/`: `AssetId`, `AssetType`, `Archive`, `CookedBlobs`).
  Vulkan-free, importer-free; linked PUBLIC by `engine` and by `cooker`.
- `cooker/` — `libveng_cook` + the `vengc` CLI (stb, assimp, Slang, JSON).
  Never linked by the engine. Its **prefab-cooking path** links `veng::veng` and
  reuses `ModuleLoader` to `dlopen` a game module and reflect its types — the one
  place the Vulkan-free cooker relaxes its separation, scoped to that load path
  (the graphics stack is linked but never initialized).
- `examples/hello-triangle/` — the canonical sample app and the smoke test. It is
  a **game module + launcher**, not one binary: `veng_add_game` builds
  `libhello_triangle` (shared, the app) plus `hello_triangle-launcher` (the exe
  that `dlopen`s it). `assets/` holds its hand-written asset pack (cooked at build
  time, copied beside the launcher).
- `tests/` — `include_hygiene`, `headless_smoke`, `compute_dispatch`, plus the
  `unit`, `death`, `gpu`, and `cooker` suites (and `shaders/`, `support/`).
- `plans/` — the roadmap. See **Working norms** below.
- `docs/ownership.md` — the resource-ownership rule, in full.

## Build & test

```sh
# Default build (validation OFF). Configure once, then build.
cmake -B build -S .
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

**If you parallelize the build, cap it at `-j 2`.** Do not go higher.

Tests, examples, and the `vengc` cooker tool build only when veng is the
top-level project (`PROJECT_IS_TOP_LEVEL`); toggles are `VENG_BUILD_TESTS` /
`VENG_BUILD_EXAMPLES` / `VENG_BUILD_TOOLS`.
Dependencies (fmt, VMA, nfd, tinyexr, stb, ImGui, imnodes) are pulled via
`FetchContent` with pinned tags — no system install needed beyond Vulkan, GLFW,
glm, and zlib (`find_package`). The cooker's heavy/toolchain deps
(nlohmann/json, assimp, and Slang for shader compile + reflection) are
**cooker-only** — gated behind `VENG_BUILD_TOOLS` and never linked into
`libveng` or its consumers, which load the *binary* archive and never parse a
source asset.

### The validation build (`VE_DEBUG`)

`VE_DEBUG=ON` enables Vulkan validation layers (`VE_ENABLE_VALIDATION_LAYERS`).
The default `build/` has it OFF. Configure a **separate** dir from the repo root
(both `build/` and `build-debug/` are gitignored):

```sh
cmake -B build-debug -S . -DVE_DEBUG=ON
cmake --build build-debug -j 2
```

## Verification — read before you trust a green run

- **The `HT_SMOKE` capture is golden-checked.** Smoke mode renders a fixed pose
  (`HelloTriangleApp::SmokeAngle`), so the capture is reproducible run to run; the
  windowed app still rotates by accumulated wall-clock `delta`. The `smoke_golden`
  ctest renders the scene headless and fuzzy-compares it against
  `tests/golden/hello_triangle_scene.png` (`ctest --test-dir build -R
  smoke_golden`). It is labelled `gpu` and skips cleanly with no Vulkan ICD. The
  capture runs through the **launcher** (which `dlopen`s `libhello_triangle`), the
  real shipping path. If a deliberate render change moves the capture, regenerate
  the golden:
  ```sh
  HT_SMOKE=/tmp/ht.ppm build/examples/hello-triangle/hello_triangle-launcher
  sips -s format png /tmp/ht.ppm --out tests/golden/hello_triangle_scene.png
  ```
  The capture is a 1280×720 RGB PPM (≈ 2,764,816 bytes).
- **`hello_triangle_launcher_smoke` covers the shipping path automatically.** It
  runs `hello_triangle-launcher` under `HT_SMOKE` and asserts exit 0 — the one test
  exercising the full `dlopen` → `VengModuleRegister` → registry → `Run()` chain
  end-to-end. Labelled `gpu` (`SKIP_RETURN_CODE 77`), it skips with no device and
  runs under the validation gate like the rest of the `gpu` band. The launcher + lib
  + pack are a **relocatable trio**: copy the launcher, `libhello_triangle.*`, and
  `sample.vengpack` into a fresh directory and run from an unrelated working
  directory — the module (`@loader_path`/`$ORIGIN` rpath) and the pack
  (`ExecutableDirectory()`-relative mount) resolve beside the launcher, so it still
  writes a correct-sized PPM and exits 0.
- **Validation errors do NOT fail tests by themselves.** The debug-messenger
  callback (`engine/src/Renderer/Backend/Context.cpp`) only `Log::Error`s on validation
  errors — it never aborts. So a green `ctest` under `VE_DEBUG` only means
  something if the validation gate ran: `ctest --test-dir build-debug -L
  validation` (the `validation_gate` test) runs the `gpu`-labelled binaries and
  fails on any unallowlisted `Vulkan validation` ERROR line
  (`cmake/ValidationGate.cmake`; allowlist currently empty). The benign MoltenVK
  "buffer robustness" warning is logged at `WARN`, not `ERROR`, and is ignored.

## Core conventions

### Error policy: no exceptions, ever

veng builds with `-fno-exceptions` (any stray `throw` is a compile error). The
split is absolute:

- **Unrecoverable** (API misuse, device loss, OOM, unsupported enum/format, a
  failed Vulkan call) → fatal `VE_ASSERT(cond, "fmt {}", ...)` (see `Assert.h`).
  It logs, breaks into the debugger in debug builds, then `std::abort()`s.
  `[[noreturn]]`.
- **Recoverable** (e.g. loading a shader file that may not exist) →
  `Veng::Result<T>` = `std::expected<T, std::string>` (`VoidResult` for void).
  See `Result.h`. Callers check truthiness, then `.value()` / `.error()`.

No exceptions anywhere — performance is the reason, and the build enforces it.

vulkan.hpp is configured `VULKAN_HPP_NO_EXCEPTIONS` with
`VULKAN_HPP_ASSERT_ON_RESULT` → `VE_ASSERT` (in `Backend/Vulkan.h`). So:
- Value-returning calls (`device.createX(...)`) return `vk::ResultValue<T>` —
  unwrap with `.value`.
- Plain `vk::Result` calls → wrap in `VK_ASSERT(call, "msg")`.
- Raw `VkResult` C calls (e.g. VMA) → wrap in `VK_RAW_ASSERT(call, "msg")`.

### House-style vocabulary

Use the aliases from `Veng.h`, not the std/glm spellings: `string`, `vector<T>`,
`map`, `optional`, `path`, `function`; `u8`/`u32`/`u64`/`f32`/`usize`; glm types
as `vec3`, `mat4`, `uvec2`, `quat`. The public API and sample app are written in
these and they are part of veng's identity.

Renderer code uses engine **vocabulary enums** (`Renderer::Format`, `ImageUsage`,
`ShaderStage`, …) from `Renderer/Types.h`, never `vk::` enums. The backend maps
them to Vulkan in `Backend/TypeMapping.h` with exhaustive switches that assert on
unmapped values — so adding a format is a loud one-line fix, not silent UB.

### Identifier naming — no Hungarian notation

**Hungarian notation is forbidden.** Do not prefix an identifier with a tag that
encodes its *type* or *kind* — neither classic systems-Hungarian (`pszName`,
`dwCount`, `bEnabled`, `nIndex`, `lpData`, `fScale`) nor a "constant" tag
(`kMaxTextures`, `k_ArchiveMagic`). Name things for what they are, in PascalCase:
a constant is `MaxTextures`, not `k_MaxTextures`. The type is the compiler's job,
not the name's.

The **only** prefixes allowed are *scope* prefixes, which encode storage/linkage,
not type: `m_` for members, `g_` for globals, `s_` for file-statics. These are
deliberate house style — keep them.

The sole exception is **the Vulkan API itself**: vulkan.hpp struct fields and
callback parameters (`pNext`, `pWaitSemaphores`, `pUserData`, …) carry upstream
Hungarian we don't control. Never rename those — match the API as given.

### Comments — factual reasons, not planning history

A code comment states a fact about the code as it is *now*. It does not narrate
how the code got here or what is planned for it. The roadmap lives in `plans/`;
git history records the evolution. Neither belongs in a comment.

**Forbidden in comments:**
- **Plan/planset citations.** No `(plan 09)`, `(planset-5/05)`, `(plan 08b)`,
  "the acceptance chain from planset-1/08", "decided in the API rework, plan 07",
  "see plans/…". The reader of the code has no reason to care which plan landed
  it. Strip the reference; keep whatever factual statement remains.
- **Future-work / temporariness.** No "for now", "v1 only / later we will",
  "future work", "a compiled graph is a later upgrade", "not yet supported",
  "before 06-09 add real loaders", "this is not the current direction". If a
  limitation is real, state it as a present-tense fact ("veng is single-threaded;
  no synchronization is provided") with no promise about the future.
- **Historical narrative.** No "used to be special-cased inside Context",
  "ported from the planset-3 one-exe test", "the public API no longer exposes
  barriers", "extracted from Barrier.cpp", "this contradicts plan 01's
  assumption". Describe the current structure, not the refactor that produced it.

**Encouraged:** comments that give the *factual reason* a piece of code is
unusual, surprising, or deliberately restricted — stated plainly, without the
backstory. "Set 0 is reserved across every pipeline layout for the bindless
registry; author-declared sets shift to 1+." "MoltenVK requires this buffer to
be host-visible." "Must run before the swap chain is recreated or the view
dangles." These earn their place precisely because they state *why*, not *how we
arrived at it*.

The test: if a sentence would still be true and useful to someone who has never
seen the roadmap and does not care about the project's history, keep it.
Otherwise cut it.

### Resource ownership & lifetime

GPU resources are constructed **only** through static `X::Create(const XInfo&)`
factories returning a smart pointer (no public constructors — they're private).
`XInfo` structs use designated initializers (`.Name = ...`, `.Usage = ...`).

The pointer type follows one rule (full version in `docs/ownership.md`):
- **`Ref<T>`** (`shared_ptr`) — genuinely shared GPU resources others hold
  references to: buffers, images, views, samplers, shaders, pipelines, descriptor
  sets/layouts, pipeline layouts.
- **`Unique<T>`** (`unique_ptr`) — single-owner primitives nothing else
  references: `Fence`, `Semaphore`, pools, per-frame sync. **When unsure, prefer
  `Unique`.**

**Dropping a resource mid-frame is safe.** Destructors do not call `vkDestroy*`;
they *retire* the handle into the current frame's bin on `Context` via the
resource's stored back-reference (`m_Context.GetNative().Retire(...)`). The
handle is destroyed only after that frame's fence is waited again
(`Context::AcquireNextFrame`), i.e. once the GPU is done with it. No manual
keep-alive lists. The one deliberate exception: `DescriptorSet` holds `Ref`s to
the resources it was written with (`m_BoundResources`) — that's ownership, not
frame-tracking.

Apps must release every engine resource in `Application::OnDispose()` (reset all
Refs/Uniques) — resources outliving the context fail on destruction.

### The Native idiom (public/backend split)

No public header may pull in `vk::`/VMA/GLFW types. Each resource hides its
backend handles in a forward-declared `struct Native;` and exposes
`[[nodiscard]] Native& GetNative() const`. The `Native` struct is defined in the
`.cpp`; the wrapper holds it as `Unique<Native> m_Native`.

`GetNative() const` returning a *mutable* reference is deliberate: the wrapper's
constness describes *its own identity* (name, format, extent), not the GPU state
behind the handle, which command recording mutates regardless.

`engine/include/Veng/Renderer/Native.h` is the **one** public header that exposes raw
handles — free `GetVkX(const X&)` accessors (e.g. `GetVkBuffer`, `GetVkDevice`)
for backend/interop code. Reach for it only when interop genuinely needs the raw
handle.

This split is guarded by the **`include_hygiene` test**, which compiles every
public header while linking only veng's PUBLIC deps (glm, fmt, ImGui). Vulkan,
GLFW, VMA, and nfd link PRIVATE, so if a public header leaks a backend include,
this test fails to build. CMake `PUBLIC`/`PRIVATE` linkage is load-bearing here —
keep glm/fmt PUBLIC and the backend libs PRIVATE.

### RenderGraph: barriers fall out of declared use

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

### SceneRenderer: the deferred über-pipeline

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

The renderer's pipeline images (g-buffer albedo + world-normal, depth, HDR, output)
are **renderer-owned `Image`/`ImageView`s `Import`ed** into the internal graph — not
graph transients — because a fullscreen pass samples an upstream target through the
bindless set-0 array, which needs a `Ref<ImageView>` to `Register` (a transient
exposes only a per-frame `ImageView&`). They are registered into bindless once at
`Create` (re-registered on `Resize`) and reach the sampling pass as `TextureHandle`s
through `PassIO`.

The per-frame `SceneView { const Scene& World; const Camera& Camera; Light Light;
f32 Delta; }` reaches pass callbacks through an **opaque `void* userData`** channel
on `RenderGraph::PassContext` / `CompiledGraph::Execute` — so `RenderGraph` stays
scene-agnostic. A `ScenePass` reads it back through a typed `ScenePassContext`
(`Cmd()` / `View()` / `Resolved(id)`); `View()` asserts the pointer is non-null
before the reinterpret, and `SceneRenderer` sets it on every `Execute`.

A `ScenePass` is a reusable, self-contained pipeline stage (`Configure` / `Resize` /
`Declare(RenderGraph&, const PassIO&)`) that **contributes** one or more
`RenderGraph` passes into the renderer's single internal graph — it is not a
`RenderGraph::Pass`. The renderer owns the **wiring** (which pass reads whose
target, via the named-slot `PassIO`); each pass owns **itself** (sizing, declared
reads/writes, recording). It knows only how to record, never what feeds it.

The über-pipeline is **batteries-included, not extensible**: a bespoke pass graph
still means dropping to `RenderGraph` directly (the composite path the sample
retains). `SceneRendererSettings { DebugView Mode; f32 Exposure; }` carries the
topology/sizing knobs — `Mode` (Final / Albedo / Normal / Depth) re-wires the pass
set through `Configure`, the recompile seam; a per-frame value rides `SceneView`.

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
async/temporal consumer (TAA/history-buffer reads of an older frame) or a handoff side
moving off the single graphics queue.

**The deferred opaque material g-buffer contract.** Going deferred changes what an
opaque material's **fragment shader outputs** — from final swapchain color to
**g-buffer channels**, written through a single engine-provided `GBufferOutput`
struct (`float4 Albedo : SV_Target0; float4 Normal : SV_Target1;`). Albedo is
sRGB-encoded (sampled back as linear); the normal is world-space in a signed float
format. Depth is the depth attachment, also sampled by the lighting pass — the only
depth target read as a texture in the engine. The g-buffer layout (channels,
formats, usage) is fixed in `Renderer/GBuffer.h`, agreed on by the geometry pass's
`RenderingInfo` and every material pipeline. This is the **v1 minimum** channel set
(a G2 PBR target extends the one `GBufferOutput` struct) and the **opaque** contract
(a transparent/forward material outputs final color through a separate fragment
entry, not a change to this one). Set-0 bindless, `MaterialData`, and texture
handles are unchanged — only the fragment shader's outputs move to the g-buffer.

### Pipeline cache

`Context` owns a `vk::PipelineCache` created at device init and threaded into both
the graphics and compute pipeline factories — every pipeline build in a run reuses
it. Persistence is **opt-in** via `ApplicationInfo::PipelineCachePath`: set → seed
from the file at startup + write it back at shutdown; `nullopt` (default) keeps it
in-memory only. A stale/foreign/truncated cache file is safe — Vulkan validates the
cache header and starts cold on a mismatch; veng feeds the bytes as `pInitialData`
and never parses them. The cache is touched only on the single render thread, so it
needs no external sync (off-thread pipeline creation would).

### Application

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

### Game modules: a shared lib + a launcher

A game is a **`libgame` (shared)** — the runtime: `Application` logic, components,
custom runtime types — loaded by a thin **launcher** (the shipped exe). The
launcher `dlopen`s the module and calls one C-ABI entry,
`VengModuleRegister(VengModuleHost*)`; the module registers its `Application`
factory into the host-owned `ApplicationRegistry` (`host->App.RegisterApplication(
[]{ … })`, one per module) **and registers its reflected component/type descriptors
into the host-owned `TypeRegistry`** (`host->Types`, `Register<T>()` per type).
`Application` still owns `Context`/`AssetManager`/`TaskSystem` unchanged — the
launcher reads the factory back, constructs the app, and calls `Run()`.
`veng_add_game(<name> SOURCES … [ASSET_PACK …])` is the build entry: it emits
`lib<name>` + `<name>-launcher` from one declaration. **`veng_add_editor(<name>
SOURCES …)`** is its editor sibling — it emits `lib<name>_editor` (SHARED, links
`libveng_editor`) + `<name>-editor` (the editor exe, links `libveng`,
`libveng_editor`, and `libveng_cook`); see the **Editor** section.

- **Same toolchain, one STL, one flag set.** Only the *entry* is C ABI; the payload
  is rich C++ (`string`, `vector`, `Ref<T>` flow across freely). veng is **not** a
  binary-plugin platform — a module is recompiled with the engine from one tree. A
  one-integer `VengModuleAbiVersion` handshake (checked by `ModuleLoader` before the
  entry runs) **rejects a stale module loudly at load**, not later. The ABI is at
  **version 2** (`VENG_MODULE_ABI_VERSION`); a module built against an older header
  fails the handshake at load.
- **The relocatable trio.** The module resolves **beside the launcher** via an
  `$ORIGIN`/`@loader_path` rpath, and assets resolve via `ExecutableDirectory()`
  (the public executable-relative path helper) with `veng_add_game` copying the
  cooked pack beside the launcher — so launcher + lib + pack move as one directory.
- **`EditorRegistry*` is the editor-host seam.** The host struct carries `{
  ApplicationRegistry& App; TypeRegistry& Types; EditorRegistry* Editor; }`; the
  launcher always passes `Editor = nullptr` (the type is only forward-declared in
  `libveng`). The editor host (`EditorHost`, in `libveng_editor`) passes a non-null
  `&m_EditorRegistry`, activating a module's editor-side registrations.
- **Game-code hot-reload is out** — restart the play session. (Distinct from *asset*
  hot-reload, the async path.)

### Editor

The editor is a separate executable, not part of the runtime. `libveng_editor` is the
editor **framework** library; the `<name>-editor` exe (produced by `veng_add_editor`)
links `libveng`, `libveng_editor`, and `libveng_cook`, and `dlopen`s the game module the
same way the launcher does — but passing a non-null `EditorRegistry*` in
`VengModuleHost::Editor`.

- **`EditorHost`** is an `Application` subclass living in `libveng_editor`. It builds a
  top-level single-window `DockSpace` (`ImGuiConfigFlags_DockingEnable`; multi-viewport
  OS windows are not built — they conflict with the single-offscreen-composite model),
  owns the panel set (open/close state, Window menu, dock layout), and loads the game
  module with `Editor = &m_EditorRegistry`.
- **`EditorPanel`** is the panel base class: a `Title()` / `OnImGui()` virtual interface;
  the host drives it each frame and owns its visibility. Built-in panels: scene viewport,
  asset browser, inspector, console/log.
- **`EditorRegistry`** is defined in `libveng_editor` and **forward-declared** in
  `engine/include/Veng/Module/Module.h` (so `libveng` stays clean). It holds the
  `AssetType`→editor-factory map (double-click an asset opens its editor),
  `RegisterPanel` for game-contributed panels, and `RegisterFieldWidget(TypeId,
  FieldWidgetFn)` for custom inspector widgets. It is non-null in `VengModuleHost` only
  in the editor host.
- **Reflection-driven inspector.** Selecting an entity walks its components through the
  host-owned `TypeRegistry` / `FieldDescriptor` layer (`Scene::ForEachComponent`), drawing
  a built-in widget per `FieldClass`
  (Scalar/Vector/Quaternion/String/AssetHandle/Enum/Struct/Matrix/Reference); a
  `RegisterFieldWidget` entry overrides the built-in for a given `TypeId`. The per-field draw
  is the shared `DrawFieldWidget` helper (`editor/src/FieldWidget.{h,cpp}`, taking a
  `FieldWidgetContext { AssetManager&, const AssetSourceIndex&, const EditorRegistry& }`) —
  the entity inspector and the node-property inspector both call it, so the two share
  identical widget behavior. The `AssetHandle` widget is an asset **picker** (a combo over
  the `AssetSourceIndex` entries of the field's `AssetType`), not a read-only label.
- **Cook-on-demand keeps the importer boundary.** `libveng_cook` is linked **only into
  the editor exe** — never `libveng_editor`, never `libgame` — so the editor framework
  library stays importer-free. The exe injects a `CookBackend` implementation;
  `EditorHost::RequestCook(CookRequest, callback)` cooks a single source off the render
  thread via `TaskSystem` (`CookSession` → `Task<vector<u8>>`), then mounts the resulting
  in-memory archive via `AssetManager::MountMemory` and hot-reloads behind the stable
  `AssetHandle`.
- **The texture editor is the template.** `TextureEditorPanel` previews via a render
  target (`CreateTexture` → `ImGui::Image`), edits `.tex.json` settings (sRGB + sampler
  filter/wrap), recooks live (300ms-debounced), and round-trips the JSON on save —
  patching known keys, preserving unknown ones.
- **`VengEditor/NodeGraph/` is a named, reusable node-graph surface.** A generic,
  imnodes-free, device-free **topology core** (`NodeGraph` — generational `NodeId`, `PinType`,
  the mutation vocabulary, direction/arity/acyclicity validation, a construction-time
  `CanConnectFn`/`PinShapeFn`/`PropertySizeFn` hook set) + a data-driven `NodeType`/
  `NodeCatalog` + graph (de)serialization to/from a JSON document string (the public surface
  stays free of the JSON library type). It is reusable by any editor. The material editor
  (and future editors — the scene editor) consume it from editor src. imnodes is used **only
  by the editor** (linked PRIVATE into `libveng_editor`, src-only — its header never appears
  in a `VengEditor/` public header; its symbols are vendored in `libveng`'s ImGui aggregation
  TU and imported across the PUBLIC `veng::veng` link).
- **Node types are data, not subclasses.** A `NodeType` is pins (typed in/out) + a reflected
  property struct; a node instance stores property bytes the reflection serializer and
  inspector widgets walk, exactly like an ECS component. `NodeTypeId` is editor-local
  (defined in `NodeGraph.h`), distinct from the runtime `TypeId` space; pin data types reuse
  builtin leaf `TypeId`s.
- **The material editor authors a graph compiled to a `.vmat` field list** (v1 binds params
  to an author-provided shader — no node→Slang codegen). The graph (nodes, positions,
  property values, links) is embedded under an `"_editor"` key in the `.vmat.json`, and
  `fields` is regenerated on compile (reusing the texture editor's preserve-unknown-keys JSON
  round-trip). `MaterialEditorPanel` drives an imnodes canvas over the graph and a
  node-property inspector reusing the per-`FieldClass` widgets. Textures are node properties
  (`FieldClass::AssetHandle`), not wired pins, so the topology core stays asset-agnostic.
- **`MaterialPreview` renders one material on a sphere via `SceneRenderer`** into an ImGui
  texture; the edit loop recooks off-thread and hot-reloads behind the stable `AssetHandle`,
  re-fetching the texture after the recompile/resize invalidates `GetOutput()`.

### Assets: cook offline, load by `AssetId`

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
  against a shader's reflected `MaterialData`. A field absent from the source keeps
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
  `Ref` to a GPU resource — see `docs/ownership.md`. Apps drop their handles in
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
  `AssetHandle<Material>`; `Mesh::Create(Renderer::Context&, const MeshData&,
  const string&)` uploads that into a resident `Ref<Mesh>` via the blocking
  `UploadSync`. A runtime primitive is **not** an `AssetId`-addressable asset and
  never touches an archive — it is owned by whoever calls the factory and retires
  through the per-frame deferred-destruction path like any other `Mesh`. It is
  interchangeable with a cooked mesh at every pipeline and draw call, both being in
  the canonical layout (`Mesh::CanonicalLayout()`), and `AssetManager::Adopt`
  wraps its `Ref<Mesh>` in an (id-less) `AssetHandle<Mesh>` so it is equally usable
  anywhere a cooked handle is — e.g. a `MeshRenderer`.
- **A mesh owns its materials; submeshes index them.** A `Mesh` holds a resident
  `vector<AssetHandle<Material>>` (`GetMaterials()`) and each `SubMesh` carries a
  `u32 MaterialIndex` into it (`SubMesh::NoMaterial` = unassigned). The cooked
  on-disk mesh format stores u64 material ids; `MeshLoader` eager-resolves those
  ids into material instances and builds the list, exactly as `Material` resolves
  its own texture/shader dependencies — so every asset eager-loads its
  dependencies. A draw iterates submeshes, binding `GetMaterials()[MaterialIndex]`
  per range.
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

### Bindless: set 0 is the engine's

The engine provides a global `BindlessRegistry` (owned by `Context`, reachable via
`Context::GetBindlessRegistry()`): a few large arrayed, `partiallyBound` +
`updateAfterBind` bindings (sampled images, samplers, storage images, and the
per-material `MaterialData` SSBO) living in **set 0**. `Register(...)` allocates a
free-list slot and returns a typed `u32` handle (`TextureHandle`, `SamplerHandle`,
`StorageImageHandle`, `MaterialHandle`); `Release` defers the slot reclaim through
the same per-frame retire window. **`PipelineLayout` reserves set 0 in every
pipeline** for the registry, bound once per pipeline bind (`registry.Bind(cmd)`),
not per draw — draws select array elements via push-constant indices. Author-
declared descriptor sets shift to **set 1+**.

### Shaders & materials

Shaders are a first-class asset authored in **Slang** — a `*.shader.json` names
its `.slang` source, entry point, and optional vertex-layout `AssetId`. The
cooker **always** compiles from source (there is no precompiled-inline path) and
**reflects the shader offline** into a serializable `ShaderInterface`; the engine
loads plain **SPIR-V** and gains no Slang dependency. (There is no `glslc` /
`add_shaders` path — GLSL was removed project-wide.)

A material (`*.vmat.json`) references its vertex/fragment shaders by `AssetId` and
declares an **ordered, explicitly-typed** field list; the cook validates those
fields against the fragment shader's reflected parameters. At runtime a `Material` is
thin — shader handle + texture **handles** + two SSBO entries, bound through set 0 — and
`Material::Bind` pushes its per-draw material index.

A material's GPU parameters are **two parallel SSBO entries** indexed by the material slot:
the fixed **engine-supplied `MaterialData`** block (the bindless handle slots the loader
patches; `static_assert`-pinned at 16 bytes; `libveng` knows its layout without reflection)
and a variable-size **authored `MaterialParams`** block (the shader's declared scalar/vector
uniforms, reflected per-shader, byte-addressed at `MaterialParamStride` in set 0 binding 4).
The seam between them is `CookedMaterialField::Kind` (handle vs. param), and the cooker
reflects both. `Material::GetFields()` exposes the reflected `MaterialField` table — the
editor's parameter-schema source, so the node editor reads a material's authorable parameters
with no Slang in `libveng_editor`.

### Scene & ECS

A `Scene` is a runtime **ECS world**: a generational `Entity` handle
(`{ u32 Index; u32 Generation; }`, `Entity::Null` empty) over a `TypeId`-keyed,
type-erased **sparse-set** per-component storage, with templated
`Add`/`Remove`/`Get`/`TryGet`/`Has` and the multi-component queries `View<Ts...>`
(range-for, yields `(Entity, Ts&...)`, supports `break`) and `Each<Ts...>`. A query
drives off the smallest participating pool. **Structural changes during iteration are
illegal** — adding/removing components or destroying entities mid-`View`/`Each` is API
misuse; the single-threaded model offers no re-entrancy guard. A stale `Entity` (its
slot recycled, generation bumped) accessed through the API is a fatal `VE_ASSERT`, not
silent UB. A **component is just a reflected type a `Scene` pools** — pools are made
lazily on first `Add` of a type; there is no separate component-id space.
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
types claiming one id is a **fatal collision assert**. The `TypeInfo.Name` string is
logs/editor display only. A game registers its own types through the same path as the
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
`Enum`) a generic walker switches on. `FieldDescriptor`s — authored via
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
`Scale`, never a world matrix), `Parent` (the hierarchy link; `WorldMatrix`/
`ComputeWorldMatrices` walk it as `parent.world * local`, recomputed on demand with no
dirty-flag cache), `Camera` (a value type building view/projection — Y flipped for
Vulkan clip space) plus `CameraComponent`, `MeshRenderer` (holds the
`AssetHandle<Mesh>` a draw queries — the mesh owns its materials, so a renderer queries
`(Transform, MeshRenderer)` and draws each submesh with its material), and `Light` (a
directional light — `Direction`/`Color`/`Intensity`; `SceneRenderer::Execute` selects
the first `Light` entity into the `SceneView`, or a zero-intensity default → flat
ambient when the scene has none).

A `Scene` is **`Unique`, single-owner** — nothing holds a `Ref` to it; the app owns it
and a renderer reads it per frame as a `const Scene&`. Drop it in `OnDispose()` like
any other engine resource. The `TypeRegistry` it was created with must outlive it and
must already have every component type registered.

## Working norms

The roadmap lives in `plans/` — read it there, don't duplicate it. `plans/README.md`
indexes the **plansets** (numbered coherent phases) and `plans/future/` (a
vision/holding area: asset system, threading, events/input, testing). Each
planset/future README carries the detail, decisions, and per-plan status column.

**Plan work** — one planset per session, on the user's cue, dispatching its plans as
appropriate (independent plans in parallel, dependent plans in sequence, derived from
the plans' direction). Per plan:
1. Implement it.
2. Migrate `examples/hello-triangle` in the *same* pass as the breaking changes.
3. Verify (clean build, `ctest` green, smoke binary writes a correct-sized PPM).
4. Update the planset README status column.
5. Commit, one commit per plan: `Plan NN: <summary>` (or `planset-N:` / `future:`
   for roadmap-only changes), with a `Co-Authored-By` trailer.

**When a new `AssetId` is needed**, use a clearly-marked placeholder id while
implementing — don't break flow to mint one mid-task. Once the build is working
and verified, mint the real ids with `vengc generate-id` (optionally with
`--reference <pack.json>` flags for existing packs) and replace the placeholders.
Never invent a final id manually. All ids in the codebase, including the core
pack's built-in layout ids, were minted this way.

**Hardcoded `AssetId` literals in C++ are written in uppercase hexadecimal with a
`0x` prefix** (`AssetId{0x4DD9F2A1C03B5E76ULL}`). This is a representation
convention for hand-written code only; JSON asset packs keep decimal ids, since
JSON has no hex literal. `vengc generate-id` prints both forms of a minted id —
the hex for C++ literals and the decimal for JSON packs.

**Delegate well-scoped chunks to `model: sonnet` subagents** (exploration sweeps,
mechanical multi-file edits, focused sub-task implementation). Keep orchestration,
design decisions, verification, and commits on the main thread. Don't spawn for
trivial single-file edits that are faster inline.
