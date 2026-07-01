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
  Never linked by the engine. Links `veng::graph` (the shared material-codegen lib).
  Its **prefab-cooking path** links `veng::veng` and
  reuses `ModuleLoader` to `dlopen` a game module and reflect its types — the one
  place the Vulkan-free cooker relaxes its separation, scoped to that load path
  (the graphics stack is linked but never initialized).
- `graph/` — `libveng_graph` (`veng::graph`), the shared node-graph + material-codegen
  library: the generic node-graph topology core (`NodeGraph`/`NodeType`/`NodeGraphSerialize`),
  the material node catalog, and the topological emit walk that turns a material graph into
  generated Slang fragment source. Links `veng::veng` PUBLIC + `nlohmann/json` PRIVATE; linked
  PUBLIC by both `libveng_editor` and `libveng_cook` so editor preview and offline cook run the
  identical walk. ImGui-free and Vulkan-free.
- `editor/` — `libveng_editor`, the editor framework, plus the single
  project-agnostic **`veng-editor`** exe (not the runtime). Links `libveng` and `veng::graph`;
  the `veng-editor` exe also links `libveng_cook`. It is launched against a project
  (`--project <project.veng>`) and `dlopen`s the module(s) the project names, resolved from
  the project's build-output dir — **discovered** from a gitignored `.veng/build.json`
  sidecar the build writes beside the project. There is no per-game editor binary.
- `examples/hello-triangle/` — the canonical **maximal** sample app and the smoke
  test, and the **in-tree consumption exemplar** (built as part of the engine tree via
  `add_subdirectory`). It is a **game module + launcher**, not one binary: `veng_add_game`
  builds `libhello_triangle` (shared, the app) plus `hello_triangle-launcher` (the exe
  that `dlopen`s it). A root `project.veng` is the entrypoint: it lists the pack(s) under
  `assets/` (hand-written, cooked at build time, copied beside the launcher) and the
  `*.buildcfg` ship targets under `configs/`.
- `examples/template/` — the **minimal** sample: the smallest correct app a new
  developer copies to start (`veng_add_game`, a window + a rotating cube, no debug UI),
  and the **out-of-tree consumption exemplar**. It is a **standalone** project that
  discovers veng with `find_package(veng)` and is **removed from the engine build**
  (`add_subdirectory(template)` is not called) — only the SDK conformance test builds it.
  Its `main.cpp` is a bare `Application` with a managed viewport + game world: the engine
  reads the cooked project, mounts its packs, loads the project's startup level, and drives
  the world — the world is authored as cooked data and bootstrapped by the engine, not built
  or driven in code. Like `hello-triangle` it has a root `project.veng` (listing its pack
  under `assets/`, naming the `startupLevel` the cook writes into the cooked project, and
  referencing a `*.buildcfg` per ship target under `configs/`: macOS / Windows / Linux), so a
  copy starts with the per-platform cook already wired. It is the engine's out-of-tree
  conformance surface — **co-migrated with `hello-triangle` on every breaking change** (see
  **Working norms**); a template breakage surfaces in the `sdk_conformance_*` tests, not in a
  plain `cmake --build`.
- `tests/` — `include_hygiene`, `headless_smoke`, `compute_dispatch`, plus the
  `unit`, `death`, `gpu`, and `cooker` suites (and `shaders/`, `support/`).
- `plans/` — the roadmap. See **Working norms** below.

### Module guides

This file holds the project-wide conventions every module is written against. The
per-module architecture lives in a `CLAUDE.md` inside each library:

- **[engine/CLAUDE.md](engine/CLAUDE.md)** — the runtime: `Application`, game
  modules, `RenderGraph`, `SceneRenderer`, bindless, `Veng::UI`, the Scene/ECS +
  reflection layer, runtime asset loading, and the shader/material model.
- **[editor/CLAUDE.md](editor/CLAUDE.md)** — the editor framework: `EditorHost`,
  panels, the reflection-driven inspector, the node-graph surface, cook-on-demand.
- **[cooker/CLAUDE.md](cooker/CLAUDE.md)** — the offline cook pipeline: `vengc`,
  shader compile/reflection, `.vmat` validation, the prefab-cooking relaxation.
- **[assetpack/CLAUDE.md](assetpack/CLAUDE.md)** — the on-disk `.vengpack` archive
  and cooked-blob format shared by the cooker and the runtime.

## Build & test

```sh
# Default build (validation OFF). Configure once, then build.
cmake -B build -S .
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

**If you parallelize the build, cap it at `-j 4`.** Do not go higher — *unless*
you are building on the main thread with no concurrent subagent builds (no
subagents at all, or only one subagent active at a time), in which case `-j 8` is
fine. The `-j 4` cap exists to leave headroom when multiple agents build in
parallel; a single builder can use the wider lane.

### Formatting

Code style is enforced by `clang-format` against the repo-root `.clang-format`.
The whole tree conforms to it. **`const` is west-placed** (`const T`, not `T const`)
— `QualifierAlignment: Left` normalizes it on format, so the `const` that
`misc-const-correctness` adds lands on the house side automatically. A checked-in
pre-commit hook
(`.githooks/pre-commit`) format-checks **only the lines a commit touches** via
`git clang-format --staged`, so a commit stays fast and each changed line must
conform. Enable it once per clone:

```sh
git config core.hooksPath .githooks
```

The hook skips cleanly when `clang-format` is absent. To reformat staged changes
the hook flagged, run `git clang-format --staged`, then re-stage and commit.

### Linting (clang-tidy)

clang-tidy is configured by the repo-root `.clang-tidy` as a **deliberately small
allowlist** (`-*` then the eight checks below): the whole tree is clean against
exactly these, so an enabled run — or the pre-commit hook — is green and any new
finding is a real regression. These checks enforce mechanical conventions that are
tedious to police by hand, and they are **authoritative** — where one of them
contradicted older hand-written style, the tree was migrated to the check:

- `readability-braces-around-statements` — **every control-flow body is braced**,
  even a single statement (`if (x) { return; }`, never `if (x) return;`).
- `misc-const-correctness` — **a local that is never mutated is declared `const`**.
- `readability-redundant-member-init` — drop a redundant `{}` on a member whose type
  already default-constructs (`vector`/`optional`/`Ref`).
- `modernize-use-designated-initializers` — aggregate init uses the designated
  `.Field = value` form, matching the `XInfo` house idiom everywhere.
- `modernize-use-scoped-lock` — `std::scoped_lock` over `std::lock_guard`.
- `modernize-use-ranges` — `std::ranges` algorithms over iterator-pair calls.
- `modernize-use-emplace` — `emplace_back` over `push_back(T{...})`.
- `modernize-use-auto` — `auto` when a cast on the RHS already names the type.

The broader `bugprone-*`/`performance-*`/`readability-*` families are **not**
enabled: a survey found them either noisy against this codebase's style
(anonymous-namespace vs `static`, implicit-bool truthiness, math parenthesization,
C-array interop tables, `std::print` vs fmt) or large stylistic churn not worth the
diff. Re-enabling any is a deliberate, separately-scoped pass. **Identifier naming
is not enforced by clang-tidy** either; the house naming rules are reviewed by hand.
Findings are warnings, never build-breaking.

Two ways to run it, both opt-in:

- **In-build:** configure with `-DVENG_ENABLE_CLANG_TIDY=ON` and clang-tidy runs
  per-TU during the build. It is wired only onto veng's own targets (set after
  the FetchContent deps), so third-party sources are never linted; the `imgui`/
  `stb`/`tinyexr` vendor aggregation TUs carry a `Checks: '-*'` override
  (`engine/src/Vendor/.clang-tidy`) and the generated core-pack embed is
  `SKIP_LINTING`. The option degrades to a warning if clang-tidy is not found.
- **Pre-commit:** the same `.githooks/pre-commit` hook runs a second stage that
  tidies **only the changed lines** of staged C/C++ via `clang-tidy-diff.py`
  against `build/compile_commands.json` (exported unconditionally). It skips
  cleanly when clang-tidy, the diff driver, or a compile DB is missing.

Tests and examples build only when veng is the top-level project
(`PROJECT_IS_TOP_LEVEL`); toggles are `VENG_BUILD_TESTS` / `VENG_BUILD_EXAMPLES`.
The cooker (`vengc`) and `veng::graph` build **unconditionally from source** — veng
*is* the tools, so a from-source build always produces them. The **editor**
(`libveng_editor` + `veng-editor`) is gated behind **`VENG_INSTALL_SDK`** (default
`${PROJECT_IS_TOP_LEVEL}`), which also gates the SDK export/install of the editor.

### Consuming veng — three modes through one `veng-config`

A game lives **outside** the engine tree and discovers veng as a normal CMake
package. `find_package(veng)` brings `veng::veng`, the imported `vengc` and
`veng-editor` executables (recreated under their unqualified names so
`$<TARGET_FILE:vengc>` resolves), the `veng::graph` / `veng_editor::veng_editor`
library aliases, and the full authoring vocabulary (`veng_add_project` /
`veng_add_game` / `veng_add_editor` / `veng_add_asset_pack`). The same `veng-config`
resolves against three sources:

```
in-tree         add_subdirectory(veng)            hello-triangle, the tests        — unchanged
build tree      find_package(veng) → veng/build   co-develop engine + game, NO install
install prefix  find_package(veng) → <prefix>     the shipped SDK
```

- **in-tree** — veng is the top-level project; helpers `include()` from the tree and
  resolve source paths. This is the engine's own build (`hello-triangle`, the tests).
- **build tree** — `-Dveng_ROOT=<veng>/build` (no install). The engine's
  `export(EXPORT vengTargets)` writes a build-tree `veng-config.cmake`; a game
  configured against it discovers veng with no install step, and the engine's
  `cmake --build` refreshes the exported targets in place.
- **install prefix** — `-DCMAKE_PREFIX_PATH=<prefix>` against a `cmake --install`ed
  SDK.

Mode is captured in `VENG_PACKAGE_MODE` (`INSTALL` / `BUILD_TREE`, unset in-tree).
Path variables are mode-resolved through the one config: the internal
`VENG_LAUNCHER_MAIN` / `VENG_CORE_SHADER_DIR` / `VENG_CORE_PACK_JSON` (and the
consumer-facing lowercase `veng_CORE_SHADER_DIR` / `veng_CORE_PACK_JSON` a downstream
cook references) point at source paths in-tree and installed/build-tree paths when
found as a package. A game repo that declares veng as a pinned `FetchContent`
dependency can redirect to a live checkout with `FETCHCONTENT_SOURCE_DIR_VENG`. The
installed `vengc` / `veng-editor` carry an `INSTALL_RPATH` and require the host's
Vulkan SDK (with its Slang component) at runtime — Slang is not vendored. See
[docs/guides/consuming-veng.md](docs/guides/consuming-veng.md).

When Doxygen is installed, `VENG_BUILD_DOCS` (default `PROJECT_IS_TOP_LEVEL`)
adds a `docs` target that renders the public-header Doxygen comments into an HTML
API reference under `build/docs/html` (`cmake --build build --target docs`). The
wiring lives in `cmake/Docs.cmake`; the target is absent without Doxygen.

Dependencies (fmt, VMA, nfd, tinyexr, stb, ImGui, imnodes, zstd) are pulled via
`FetchContent` with pinned tags — no system install needed beyond Vulkan, GLFW,
glm, and zlib (`find_package`). **zstd is the one third-party codec linked into
`libveng`** (transitively, PUBLIC through `assetpack`, which inflates compressed
archive blobs at runtime); it adds no public-header include (the codec is a plain
enum field, all zstd calls confined to `Archive.cpp`). The cooker's heavy/toolchain
deps (nlohmann/json, assimp, and Slang for shader compile + reflection, plus the
**`bc7enc_rdo` / `astc-encoder` texture encoders**) are **cooker-only** — linked into
`vengc` alone, never into `libveng` or its consumers, which load the *binary* archive
and never parse or encode a source asset.

### Build configurations — role on the asset, format on the platform

A texture's codec is a **platform** decision, not a per-asset one. The project owns
the build policy as a small reflected data model (`Veng/Project/`): a
**`ProjectSettings`** (one per project, the JSON file `project.veng`) is the project
entrypoint: it lists the asset **packs** it owns, a list of **`BuildConfiguration`**s
(one per ship target — macOS / Windows / Linux / mobile, each a `*.buildcfg` JSON file
holding a **role → format** table (`RoleToFormat`), a zstd compression level, and an
output-pack suffix), and the startup level. A texture's `*.tex.json` declares a
compression **role** — its *intent*, one of the closed `CompressionRole` set
(Color / Normal / Mask / HDR / UI) — never a raw codec; the active configuration
resolves role → concrete `CompressionFormat` per platform (the raw `"compression"` key
stays as a per-texture escape hatch). The cook reads each configuration and emits, per
configuration, **its packs plus a cooked project file (`.vengproj`)** — the runtime
entrypoint naming the packs to mount and the startup level.

A bare `cmake --build` cooks the **host-matching configuration** by default:
`VENG_BUILD_CONFIG` is a cache variable defaulted from the host triple
(`cmake/BuildConfig.cmake`'s `veng_host_default_config_name`), so building on a Mac cooks
the macOS/ASTC pack with no flag. Override with `-DVENG_BUILD_CONFIG=windows` to cook a
foreign config (always allowed — the encoder is CPU), and the `cook-all-packs` aggregate
target builds every configuration's output for CI / ship. An engine-internal pack cooked
through `veng_add_asset_pack` with **no** configuration uses the zero-config codec default (the
hardcoded ASTC fallback); both example apps drive `veng_add_project` from a root `project.veng`,
so neither relies on it. The data model is in `engine/CLAUDE.md`, the cook
resolution + CMake selection in `cooker/CLAUDE.md`, and the editor surface + host-capability
preview gate in `editor/CLAUDE.md`.

### The validation build (`VE_DEBUG`)

`VE_DEBUG=ON` enables Vulkan validation layers (`VE_ENABLE_VALIDATION_LAYERS`).
The default `build/` has it OFF. Configure a **separate** dir from the repo root
(both `build/` and `build-debug/` are gitignored):

```sh
cmake -B build-debug -S . -DVE_DEBUG=ON
cmake --build build-debug -j 4
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
  runs under the validation gate like the rest of the `gpu` band. The launcher + lib +
  project + pack are a **relocatable set**: copy the launcher, `libhello_triangle.*`,
  `project.vengproj`, and `sample.vengpack` into a fresh directory and run from an unrelated
  working directory — the module (`@loader_path`/`$ORIGIN` rpath), the cooked project, and the
  pack (`ExecutableDirectory()`-relative) resolve beside the launcher, so it still writes a
  correct-sized PPM and exits 0.
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

**A component is named as a bare noun**, not suffixed with its kind: `Transform`,
`Light`, `Camera`, `Primitive` — never `TransformComponent` / `PrimitiveComponent`.
`Component` is a kind tag, and the type system already says it is a component; the name
says *what* it is. When a value type would own the bare name, **the value type takes the
precise role-name** so the component keeps the natural noun — the render-ready
view-projection is `CameraView`, leaving `Camera` for the component.

The sole exception is **the Vulkan API itself**: vulkan.hpp struct fields and
callback parameters (`pNext`, `pWaitSemaphores`, `pUserData`, …) carry upstream
Hungarian we don't control. Never rename those — match the API as given.

**Accessors carry a verb prefix.** A getter is `GetFoo()`, a setter `SetFoo()` —
never a bare `Foo()`. This holds for both public APIs and virtual interfaces (e.g.
`GetTitle()` / `GetWindowFlags()`). A **boolean predicate query keeps an `Is`
prefix** — `IsMouseDoubleClicked()`, `IsKeyPressed()`, not bare
`MouseDoubleClicked()`/`KeyPressed()`. A value getter that is not a predicate takes
no prefix beyond `Get` and reads as a plain noun where natural (`PopupMousePosition()`).

### Comments — factual reasons, not planning history

A code comment states a fact about the code as it is *now*. It does not narrate
how the code got here or what is planned for it. The roadmap lives in `plans/`;
git history records the evolution. Neither belongs in a comment.

There are **two tiers** of comment, and the rules below apply to both:

- **Doc comments** sit on a *declaration* — a class/struct, a method or free
  function, a field, an enum and its enumerators, a macro, a public type alias.
  They are **Doxygen** (see below) and describe the API contract for a caller.
- **Inline comments** sit *inside* a function body. They are plain `//` and give
  the local *why* — never a restatement of what the next line does.

**Forbidden in either tier:**
- **Plan/planset citations.** No `(plan 09)`, `(planset-5/05)`, `(plan 08b)`,
  "the acceptance chain from planset-1/08", "decided in the API rework, plan 07",
  "see plans/…". The reader of the code has no reason to care which plan landed
  it. Strip the reference; keep whatever factual statement remains.
- **Future-work / temporariness.** No "for now", "v1 only / later we will",
  "future work", "a compiled graph is a later upgrade", "not yet supported",
  "before 06-09 add real loaders", "this is not the current direction". If a
  limitation is real, state it as a present-tense fact ("veng is single-threaded;
  no synchronization is provided") with no promise about the future.
- **Decorative version tags.** No `v1`/`v2` sprinkled into prose to mean "and a
  later version will differ" ("the v1 reflection surface", "v1 flattens every
  mesh"). Drop the tag and state what the code *is*. A version number that the
  code actually checks — an on-disk format number rejected on mismatch — is a real
  fact and stays; describe it as such.
- **Historical narrative.** No "used to be special-cased inside Context",
  "ported from the planset-3 one-exe test", "the public API no longer exposes
  barriers", "extracted from Barrier.cpp", "this contradicts plan 01's
  assumption". Describe the current structure, not the refactor that produced it.
  Beware `no longer` / `previously` / `used to` that contrast with an *older
  version of the source* — cut them. (`previously`/`later` that refer to an
  earlier/later moment in *program execution* — "clear any previously bound
  pipeline", "a later graph pass" — are factual and stay.)
- **Re-documenting the callee at a call site.** A comment at a *usage* site
  explains why *this* code makes *this* call — the local decision, what this
  app/test demonstrates, the constraint that forced it. It does **not** restate
  the general behavior of the type or function being called; that documentation
  lives on the declaration. Test: if you pasted the comment onto the callee's own
  declaration, would it read as that callee's doc comment? If yes, it is
  misplaced — replace it with the local reason, or delete it if the call is
  self-explanatory. When one engine contract recurs at many call sites, document
  it once and reference it (or say nothing) at the rest, rather than restating it
  each time.

**Encouraged:** comments that give the *factual reason* a piece of code is
unusual, surprising, or deliberately restricted — stated plainly, without the
backstory. "Set 0 is reserved across every pipeline layout for the bindless
registry; author-declared sets shift to 1+." "MoltenVK requires this buffer to
be host-visible." "Must run before the swap chain is recreated or the view
dangles." These earn their place precisely because they state *why*, not *how we
arrived at it*.

**Be concise — one line is the default.** A comment that earns its place still
states its point in the fewest words that carry the *why*. Two lines only when a
genuine non-obvious reason needs them; three or more inline lines is a smell. Cut a
sentence that (a) describes what the called function does instead of why *this* call
is here — that belongs on the callee's declaration; (b) restates a contract
documented elsewhere — reference it (`see ReconfigureScene`) rather than re-deriving
it; or (c) inventories structure the code already shows — `m_A.reset();
m_B.reset();` needs the one reason the block exists, not a line-item map of who owns
what. Keep the load-bearing why; drop the tour. (Doc comments on a public
declaration are the exception: a full `@brief` plus contract is the goal there.)

The test: if a sentence would still be true and useful to someone who has never
seen the roadmap and does not care about the project's history, keep it.
Otherwise cut it.

#### Doc comments are Doxygen, and the API is fully documented

Every declaration in a **public header** (`engine/include/Veng/`, and the public
headers of `assetpack`/`cooker`/`editor`) carries a Doxygen doc comment — the
surface is documented end to end so a doc generator produces a complete reference.
**Every public member is documented, even when the comment is obvious** — a
one-arg setter, a plain getter, a `Configure(settings)` override all still get a
`@brief`, however short (`/// @brief Configures the pass from the settings.`).
"Self-evident" is never a reason to skip a *public* declaration; it only excuses a
truly trivial *private* helper. Internal headers (`*/src/`) document every
non-trivial declaration; a self-evident private helper may go without.

**A documented declaration is in Doxygen form regardless of visibility.** Private
members are not second-class: when a private field, helper, or member gets a doc
comment, it is a `@brief` block exactly like a public one — a bare `///` prose
comment sitting on a *declaration* is not acceptable (`/// @brief Frustum-query
scratch.` followed by a blank `///` and the detail, not two lines of plain `///`
prose). Plain `//`/`///` prose without `@brief` is only for *inline body* comments,
which are never doc comments.

The house Doxygen style:
- **One doc comment documents exactly one declaration — never a group.** A `@brief`
  attaches only to the single declaration immediately below it; the next
  declaration needs its own. Two getters, two overloads, or two fields sharing one
  comment leaves the second *undocumented* — write a separate `///` block for each.
  If two members are genuinely parallel, say so in each
  ("@brief Number of atlas tile columns." / "@brief Number of atlas tile rows."),
  don't fold them into one comment over both.
- **`///` line comments**, not `/** … */`. Tags are `@`-prefixed (`@brief`), not
  `\`-prefixed.
- **First line is `@brief`** — one sentence, the summary a doc index shows. Then a
  blank `///` line, then any detailed description (the existing rationale prose
  lives here, unchanged in spirit, still bound by the rules above).
- **Document the contract:** `@param` per parameter, `@tparam` per template
  parameter, `@return` for a non-void return, `@pre`/`@post` for ordering or state
  requirements, `@warning` for a footgun, `@see` to cross-reference. A `@param` is
  not mandatory when the brief already says everything (a one-arg setter); use
  judgment — the goal is a complete, non-redundant reference, not boilerplate.
- **Never use a trailing `///<`.** *Every* doc comment — including a field or
  enumerator doc, however short — sits on its own `///` line(s) **before** the
  declaration, in `@brief` form. There is no same-line doc-comment form in veng.

```cpp
/// @brief Stages and copies data into the image, blocking until the copy completes.
///
/// Runs the host memcpy + WaitIdle path, not the async transfer queue. Used by the
/// sync loaders, tests, and the smoke render; prefer Upload() off the render thread.
/// @param commandBuffer  Command buffer the copy is recorded into.
/// @param data           Source pixels, in the image's format.
/// @pre The image was created with ImageUsage::TransferDst.
void UploadSync(CommandBuffer& commandBuffer, std::span<const std::byte> data);
```

### Resource ownership & lifetime

GPU resources are constructed **only** through static `X::Create(const XInfo&)`
factories returning a smart pointer (no public constructors — they're private).
`XInfo` structs use designated initializers (`.Name = ...`, `.Usage = ...`).

**`Create` constructs a GPU resource; `Build` produces an engine asset.** A
low-level GPU resource — `Buffer`, `Image`, `ImageView`, `Sampler`, `Shader`, the
pipelines, `DescriptorSet`, `Fence`, `Semaphore` — is constructed from its descriptor
through `X::Create(const XInfo&)`: synchronous, returning a ready `Ref<T>`, because
constructing the object is immediate and has no async form. A higher-level engine
**asset** that carries CPU source data and *uploads* it — `Mesh` (from `MeshData`),
`Texture` (from `TextureData`), `Material` (from `MaterialInfo`) — is built at runtime
through **`AssetManager`**: `Build<T>(...)` is **async by default** (runs the worker-legal
upload off the render thread, lands the render-thread-only finalize — bindless
registration — on the continuation pump, returns a pending `AssetHandle<T>`), and
`BuildSync<T>(...)` is the blocking sibling (builds + finalizes inline on the render
thread, returns a resident handle) — the async-default rule of `Load`/`LoadSync` and
`Upload`/`UploadSync`. So the verb tells you the tier *and* the sync/async expectation:
`Create` → a GPU object, now; `Build`/`BuildSync` → an asset, streaming or blocking. A
bindless-registered asset's own construction is a private two-phase **`Prepare`** (worker-
legal: construct + upload, unregistered) → **`Finalize`** (render-thread: register into
bindless) seam that `AssetManager` and the loaders drive — not app-facing. The low-level
`Create(const XInfo&)` GPU-construct-from-already-uploaded-handles step stays `Create` and
public only where a from-handles path is real (`Mesh::Create(MeshInfo)`), distinct from
building the asset from data. A runtime resource enters the `AssetManager` cache through
**`Adopt`** — `Adopt(Ref<T>)` for a resident one, `Adopt(Task<Ref<T>>)` for a streaming one;
`AssetManager::Build<T>` is the convenience that builds *and* adopts in one call.

The pointer type follows one rule:
- **`Ref<T>`** (`shared_ptr`) — genuinely shared GPU resources others hold
  references to: buffers, images, views, samplers, shaders, pipelines, descriptor
  sets/layouts, pipeline layouts.
- **`Unique<T>`** (`unique_ptr`) — single-owner primitives nothing else
  references: `Fence`, `Semaphore`, pools, per-frame sync. **When unsure, prefer
  `Unique`.**

`Ref` is for *real* sharing, never a correctness crutch — deferred destruction,
below, already makes it safe to drop a resource the GPU is still using.

**Dropping a resource mid-frame is safe.** Destructors do not call `vkDestroy*`;
they *retire* the handle into the current frame's bin on `Context` via the
resource's stored back-reference (`m_Context.GetNative().Retire(...)`). The
handle is destroyed only after that frame's fence is waited again
(`Context::AcquireNextFrame`), i.e. once the GPU is done with it. No manual
keep-alive lists. An async upload's staging buffer instead retires on the
**transfer timeline** (`RetireOnTransfer`), since its copy completes on the
transfer queue, not the frame fence; off-thread drops make the whole retire path
mutex-guarded. The one deliberate exception: `DescriptorSet` holds `Ref`s to the
resources it was written with (`m_BoundResources`) — that's ownership, not
frame-tracking.

`AssetHandle<T>` and bindless handles (`TextureHandle`, …) sit *above* this rule
and are not `Ref`s: an `AssetHandle` is refcounted indirection into the
`AssetManager` cache, a bindless handle is a plain `u32` slot id whose owning
`Ref` lives in the `BindlessRegistry`. Both release through the same per-frame
retire path; the GPU `Ref`s *inside* an asset still follow the rule above.

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

## Working norms

The roadmap lives in `plans/` — read it there, don't duplicate it. `plans/README.md`
indexes the **plansets** (numbered coherent phases) and `plans/future/` (a
vision/holding area: asset system, threading, events/input, testing). Each
planset/future README carries the detail, decisions, and per-plan status column.

**Plan work** — one planset per session, on the user's cue, dispatching its plans as
appropriate (independent plans in parallel, dependent plans in sequence, derived from
the plans' direction). Per plan:
1. Implement it.
2. Migrate **both** `examples/hello-triangle` (the maximal sample, consumed
   **in-tree**) and `examples/template` (the minimal one a new developer copies,
   consumed **out-of-tree** via `find_package(veng)`) in the *same* pass as the
   breaking changes — together they are the dual-mode conformance check. The template
   is **not** built by the default in-tree `cmake --build`, so a template breakage
   surfaces in the SDK conformance tests (`sdk_conformance_install` /
   `sdk_conformance_buildtree`, the `gpu` band), not in a plain build.
3. Verify (clean build, `ctest` green, `hello_triangle-launcher` under `HT_SMOKE`
   writes a correct-sized PPM). The template has no smoke/PPM path; its conformance
   tests configure + build it standalone and probe `veng-editor --version`.
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
