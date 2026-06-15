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

- `engine/` — `libveng`, the runtime. Links only `assetformat` (loader, no
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
- `assetformat/` — `libveng_assetformat`, the shared archive + cooked-blob
  format (`Veng/Asset/`: `AssetId`, `AssetType`, `Archive`, `CookedBlobs`).
  Vulkan-free, importer-free; linked PUBLIC by `engine` and by `cooker`.
- `cooker/` — `libveng_cook` + the `vengc` CLI (stb, assimp, Slang, JSON).
  Never linked by the engine.
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
changed); per-frame data never recompiles. See `BuildSceneGraph` /
`BuildCompositeGraph` (compile) and `RenderScene` / `CompositeToSwapChain` (replay)
in the hello-triangle `main.cpp` for the pattern — member compiled graphs held
across frames, imports bound per frame, re-compiled on resize.

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
[]{ … })`, one per module). `Application` still owns
`Context`/`AssetManager`/`TaskSystem` unchanged — the launcher reads the factory
back, constructs the app, and calls `Run()`. `veng_add_game(<name> SOURCES … [ASSET_PACK …])`
is the build entry: it emits `lib<name>` + `<name>-launcher` from one declaration.

- **Same toolchain, one STL, one flag set.** Only the *entry* is C ABI; the payload
  is rich C++ (`string`, `vector`, `Ref<T>` flow across freely). veng is **not** a
  binary-plugin platform — a module is recompiled with the engine from one tree. A
  one-integer `VengModuleAbiVersion` handshake (checked by `ModuleLoader` before the
  entry runs) **rejects a stale module loudly at load**, not later.
- **The relocatable trio.** The module resolves **beside the launcher** via an
  `$ORIGIN`/`@loader_path` rpath, and assets resolve via `ExecutableDirectory()`
  (the public executable-relative path helper) with `veng_add_game` copying the
  cooked pack beside the launcher — so launcher + lib + pack move as one directory.
- **`EditorRegistry*` is the reserved seam for the future editor host.** The host
  struct carries `{ ApplicationRegistry& App; EditorRegistry* Editor; }`; the
  launcher always passes `Editor = nullptr` (the type is only forward-declared in
  `libveng`). Only the future editor host passes non-null.
- **Game-code hot-reload is out** — restart the play session. (Distinct from *asset*
  hot-reload, the async path.)

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
  packs. The hash function lives **only** in the cooker/verify tool, so `assetformat`
  (which stores the raw 16 bytes and computes nothing) and `libveng` gain no hash
  dependency.
- **An asset pack is a pure `{ id, type, source }` manifest.** It carries no
  per-asset settings. **Every** asset type — texture, mesh, shader, material —
  has its own per-asset JSON source file (`*.tex.json` / `*.mesh.json` /
  `*.shader.json` / `*.vmat.json`) that the manifest entry points at; the sampler
  settings, import options, shader source/entry, and material fields live in
  those files.
- **Load is by opaque `u64` `AssetId`** through mounted archives.
  `AssetManager::Load<T>(AssetId)` is **async by default**: it returns a
  not-yet-resident `AssetHandle<T>` immediately and runs the decode + GPU upload
  on the task system (transfer queue, no frame stall); poll `IsLoaded()` before
  using it. `AssetManager::LoadSync<T>(AssetId)` is the **blocking** sibling — it
  runs the whole pipeline inline and returns a resident handle or a structured
  error, `AssetResult<AssetHandle<T>>` (`std::expected<…, AssetLoadError>` —
  branch on `AssetError::Kind`, not a string).
- **`AssetManager` is owned by `Application` and constructed with a `Context&`
  and a `TaskSystem&`.** The async `Load` is the obvious call and the
  non-stalling one; `LoadSync` is the marked-verbose blocking spelling for tests,
  tools, and the smoke path.

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
fields against the fragment shader's reflected `MaterialData` block. At runtime a
`Material` is thin — shader handle + texture **handles** + a `MaterialData` SSBO
entry, bound through set 0 — and `Material::Bind` pushes its per-draw material
index.

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

Types register into the engine-owned **`TypeRegistry`** (`Application::GetTypeRegistry()`,
threaded into `Scene::Create(TypeRegistry&)`), which is generic over *any* reflected
type — leaf field types, nested structs, and components share **one `TypeId` space**.
Each type carries a stable `u64` **`TypeId` authored exactly like an `AssetId`**:
hardcoded `0x…ULL` for engine types, `vengc generate-id` for game types, hex in C++ /
decimal in JSON. It is a compile-time constant (`TypeIdOf<T>()` reads it off a trait,
independent of registration order), persisted directly (a scene stores a component's
`TypeId`, never its name), and byte-identical across the future module boundary; two
types claiming one id is a **fatal collision assert**. The `TypeInfo.Name` string is
logs/editor display only. A game registers its own types through the same path as the
builtins — a **`VE_REFLECT`** describe-block next to the struct, read back by the
zero-arg `Register<T>()` (a referenced type's schema auto-registers from its trait on
first reference, so there is no registration-ordering burden).

The reflection layer (`Veng/Reflection/`): an **open** `TypeId` space (a game adds a
leaf/struct/component with no engine change) and a **closed** `FieldClass`
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
Vulkan clip space) plus `CameraComponent`, and `MeshRenderer` (holds the
`AssetHandle<Mesh>` a draw queries — the mesh owns its materials, so a renderer queries
`(Transform, MeshRenderer)` and draws each submesh with its material).

A `Scene` is **`Unique`, single-owner** — nothing holds a `Ref` to it; the app owns it
and a renderer reads it per frame as a `const Scene&`. Drop it in `OnDispose()` like
any other engine resource. The `TypeRegistry` it was created with must outlive it and
must already have every component type registered.

## Working norms

The roadmap lives in `plans/` — read it there, don't duplicate it. `plans/README.md`
indexes the **plansets** (numbered coherent phases) and `plans/future/` (a
vision/holding area: asset system, threading, events/input, testing). Each
planset/future README carries the detail, decisions, and per-plan status column.

**Plan work** — one plan per session, on the user's cue. Per plan:
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
