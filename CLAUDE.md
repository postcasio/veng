# veng

A C++26 Vulkan rendering engine. Built as a shared library (`libveng`) with a
public API under `engine/include/Veng/` and a Vulkan backend hidden behind it.
Primary dev platform is macOS via MoltenVK; the code is written to be portable
(a Windows port is anticipated, hence `VE_API`).

**veng v1 is single-threaded by design, not by accident.** The render `Context`
is constructed explicitly by `Application` and threaded into every resource;
`Time`, input, and the ImGui integration all assume one driving thread. Do not
call veng APIs concurrently. (De-globalizing the context is done — see
`plans/planset-4/`; a task system remains future work — see `plans/future/`.)

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
  format. Vulkan-free, importer-free; linked by both `engine` and `cooker`.
- `cooker/` — `libveng_cook` + the `vengc` CLI (stb, assimp, Slang, JSON).
  Never linked by the engine.
- `examples/hello-triangle/` — the canonical sample app and the smoke test.
- `tests/` — `include_hygiene`, `headless_smoke`, `compute_dispatch`.
- `plans/` — the roadmap. See **Working norms** below.
- `docs/ownership.md` — the resource-ownership rule, in full.

## Build & test

```sh
# Default build (validation OFF). Configure once, then build.
cmake -B build -S .
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests/examples build only when veng is the top-level project
(`PROJECT_IS_TOP_LEVEL`); toggles are `VENG_BUILD_TESTS` / `VENG_BUILD_EXAMPLES`.
Dependencies (fmt, VMA, nfd, tinyexr, stb, ImGui, imnodes) are pulled via
`FetchContent` with pinned tags — no system install needed beyond Vulkan, GLFW,
glm, and zlib (`find_package`).

### The validation build (`VE_DEBUG`)

`VE_DEBUG=ON` enables Vulkan validation layers (`VE_ENABLE_VALIDATION_LAYERS`).
The default `build/` has it OFF. Configure a **separate** dir from the repo root
(both `build/` and `build-debug/` are gitignored):

```sh
cmake -B build-debug -S . -DVE_DEBUG=ON
cmake --build build-debug -j
```

## Verification — read before you trust a green run

- **The `HT_SMOKE` PPM is non-deterministic.** The hello-triangle smoke render
  rotates the triangle by accumulated wall-clock `delta`, so two runs of the same
  binary produce different pixels. Do **not** golden-compare it. Verify instead
  with: clean build, `ctest` green, and the smoke binary exiting 0 having written
  a correctly-sized PPM (1280×720 RGB ≈ 2,764,816 bytes).
  ```sh
  HT_SMOKE=/tmp/ht.ppm build/examples/hello-triangle/hello_triangle
  ```
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

Dom rejects exceptions as not performant for a game engine. This governs **all**
veng code, not just plan work — no exceptions anywhere.

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
these and they are considered part of veng's identity (decided in plan 07 — they
stay).

Renderer code uses engine **vocabulary enums** (`Renderer::Format`, `ImageUsage`,
`ShaderStage`, …) from `Renderer/Types.h`, never `vk::` enums. The backend maps
them to Vulkan in `Backend/TypeMapping.h` with exhaustive switches that assert on
unmapped values — so adding a format is a loud one-line fix, not silent UB.

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

Don't hand-write layout transitions/barriers. Declare a pass with the views it
writes (`.Color(...)`) and reads (`.Sample(...)`); the graph derives the layout
transitions and drives `BeginRendering`/`EndRendering`. See `RenderScene` /
`CompositeToSwapChain` in the hello-triangle `main.cpp` for the pattern.

### Application & shaders

Subclass `Application`, override `OnInitialize` / `OnUpdate(delta)` / `OnRender` /
`OnDispose`, and `Run(args)`. ImGui is opt-in (on by default; `nullopt` to skip),
and a `Headless` flag runs windowless to `RequestExit()` instead of a window
close — that's the CI/smoke path.

Shaders are GLSL compiled to SPIR-V by `glslc` via the `add_shaders(target,
srcs...)` CMake function (`cmake/Shaders.cmake`), output to
`<build>/shaders/<name>.spv`. Shaders load at runtime through `Shader::Create`,
which returns a `Result` (file may be missing) — `VE_ASSERT` on it at call sites.

## Working norms

The roadmap lives in `plans/` — read it there, don't duplicate it. `plans/README.md`
indexes the **plansets** (numbered coherent phases) and `plans/future/` (a
vision/holding area: asset system, threading, de-globalizing the context,
events/input, testing). Each planset/future README carries the detail, decisions,
and per-plan status column. As of 2026-06: **planset-1** done (12 plans),
**planset-2** in progress (rendering API surface cleanup).

**How Dom runs plan work** — one plan per session, on his cue. Per plan:
1. Implement it.
2. Migrate `examples/hello-triangle` in the *same* pass as the breaking changes.
3. Verify (clean build, `ctest` green, smoke binary writes a correct-sized PPM).
4. Update the planset README status column.
5. Commit, one commit per plan: `Plan NN: <summary>` (or `planset-N:` / `future:`
   for roadmap-only changes), with a `Co-Authored-By` trailer.

**Delegate well-scoped chunks to `model: sonnet` subagents** (exploration sweeps,
mechanical multi-file edits, focused sub-task implementation). Keep orchestration,
design decisions, verification, and commits on the main thread. Don't spawn for
trivial single-file edits that are faster inline.
