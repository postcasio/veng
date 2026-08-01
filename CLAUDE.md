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

## Layout & module guides

Each library lives in its own root subdirectory; the top-level `CMakeLists.txt` is thin
(shared deps + `add_subdirectory` per lib). This file holds the project-wide conventions
every module is written against; each module's architecture lives in its own `CLAUDE.md`:

- **`engine/`** — `libveng`, the runtime. Links only `assetpack` (loader, no importer deps).
  Public headers under `engine/include/Veng/`; the Vulkan implementations live under
  `engine/src/Renderer/Backend/`. **[engine/CLAUDE.md](engine/CLAUDE.md)** covers
  `Application`, game modules, and the project data model, and indexes the per-system docs
  under `engine/src/*/CLAUDE.md` (renderer, scene/gameplay, assets/materials, reflection,
  `Veng::UI`, `Veng::Gui`, networking).
- **`assetpack/`** — `libveng_assetpack`, the shared `.vengpack` archive + cooked-blob format
  (`Veng/Asset/`: `AssetId`, `AssetType`, `Archive`, `CookedBlobs`). Vulkan-free,
  importer-free; linked PUBLIC by `engine` and `cooker`.
  **[assetpack/CLAUDE.md](assetpack/CLAUDE.md)**.
- **`cooker/`** — `libveng_cook` + the `vengc` CLI (stb, assimp, Slang, the texture encoders —
  cooker-only deps, never linked by the engine). Its prefab-cooking path links `veng::veng` to
  `dlopen` a game module and reflect its types — the one place the Vulkan-free cooker relaxes
  its separation. A cook runs its pack's assets across a **bounded work pool**, so an importer —
  including one a consumer's cook module supplies — declares its concurrency class; see
  [The cook is parallel](#the-cook-is-parallel) below.
  **[cooker/CLAUDE.md](cooker/CLAUDE.md)**.
- **`graph/`** — `libveng_graph` (`veng::graph`), the shared node-graph + material-codegen
  library; linked PUBLIC by both `libveng_editor` and `libveng_cook` so editor preview and
  offline cook run the identical emit walk. ImGui-free and Vulkan-free.
  **[graph/CLAUDE.md](graph/CLAUDE.md)**.
- **`editor/`** — `libveng_editor` (the editor framework) plus the single project-agnostic
  **`veng-editor`** exe, launched against a project (`--project <project.veng>`); there is no
  per-game editor binary. **[editor/CLAUDE.md](editor/CLAUDE.md)**.
- **`mcp/`** — `libveng_mcp` (`veng::mcp`), the **optional** MCP server + client library a
  consuming app (game *or* editor) links to expose its live systems to AI agents. Not linked
  by `libveng`. **[mcp/CLAUDE.md](mcp/CLAUDE.md)**.
- **`examples/`** — `hello-triangle` (the maximal sample, consumed **in-tree**) and `template`
  (the minimal sample, consumed **out-of-tree** via `find_package(veng)`) — the two co-migrated
  consumption exemplars. **[examples/CLAUDE.md](examples/CLAUDE.md)** also carries the `HT_SMOKE`
  capture contract, the golden-regeneration runbook, and the relocatable-set check.
- **`tests/`** — `include_hygiene`, `headless_smoke`, `compute_dispatch`, plus the `unit`,
  `death`, `gpu`, and `cooker` suites (and `shaders/`, `support/`).
- **`docs/`** — the Doxygen wiring and the task-oriented guides under
  [docs/guides/](docs/README.md).

## Build & test

```sh
# Default build — VE_DEBUG=ON (Vulkan validation on). Configure once, then build.
cmake -B build-debug -S . -DVE_DEBUG=ON
cmake --build build-debug -j 4
ctest --test-dir build-debug -j 4 --output-on-failure
```

**`VE_DEBUG` selects the build type too.** With no explicit `CMAKE_BUILD_TYPE`, a
`VE_DEBUG=ON` tree configures as **Debug** and a validation-OFF tree as **Release**, so
the debug tree is genuinely unoptimized and debuggable rather than `-O3` wearing the
name. `-g` is added to veng's own targets under `VE_DEBUG` regardless of type, so a
tree pinned to Release still yields `file:line` backtraces out of `libveng`. The
cooker's codec hot loops opt back into `-O2` under Debug, so a debug cook does not run
its encoders unoptimized. Pass `-DCMAKE_BUILD_TYPE=…` to override. **An existing tree
keeps whatever type its cache already holds** — a tree configured before this landed
stays Release until it is deleted and configured afresh.

**Build and test the debug build only — do not build twice.** The `build-debug`
tree above (`VE_DEBUG=ON`) is the one build an agent configures, builds, and tests
by default. It is `-Werror` and runs the validation gate, so it catches strictly
more than the validation-OFF build. The release build (`build/`, validation OFF —
see [The release build](#the-release-build-validation-off)) is **optional**: reach
for it only when you specifically need to check release-only behavior, and never
build both routinely.

**If you parallelize the build, cap it at `-j 4`.** Do not go higher — *unless*
you are building on the main thread with no concurrent subagent builds (no
subagents at all, or only one subagent active at a time), in which case `-j 8` is
fine. The `-j 4` cap exists to leave headroom when multiple agents build in
parallel; a single builder can use the wider lane.

Tests and examples build only when veng is the top-level project
(`PROJECT_IS_TOP_LEVEL`); toggles are `VENG_BUILD_TESTS` / `VENG_BUILD_EXAMPLES`.
The cooker (`vengc`) and `veng::graph` build **unconditionally from source** — veng
*is* the tools. The **editor** (`libveng_editor` + `veng-editor`) is gated behind
**`VENG_INSTALL_SDK`** (default `${PROJECT_IS_TOP_LEVEL}`), which also gates the SDK
export/install of the editor. When Doxygen is installed, `VENG_BUILD_DOCS` (default
`PROJECT_IS_TOP_LEVEL`) adds a `docs` target rendering the public-header Doxygen
comments to HTML under `build/docs/html` (`cmake/Docs.cmake`).

### Formatting

Code style is enforced by `clang-format` against the repo-root `.clang-format`.
The whole tree conforms to it. **`const` is west-placed** (`const T`, not `T const`)
— `QualifierAlignment: Left` normalizes it on format, so the `const` that
`misc-const-correctness` adds lands on the house side automatically. A checked-in
pre-commit hook (`.githooks/pre-commit`) format-checks **only the lines a commit
touches** via `git clang-format --staged`, so a commit stays fast and each changed
line must conform. Enable it once per clone:

```sh
git config core.hooksPath .githooks
```

The hook skips cleanly when `clang-format` is absent. To reformat staged changes
the hook flagged, run `git clang-format --staged`, then re-stage and commit.

**Checking format by hand needs no toolchain setup** — `clang-format` parses the file
against `.clang-format` and never consults the build, so the tool on `PATH` is the
right one and no compile DB, SDK, or PCH is involved:

```sh
clang-format --dry-run -Werror <files>   # non-zero exit + a diagnostic per offending line
git clang-format --staged --diff         # only the staged lines — what the hook checks
```

**A pre-existing format or lint finding you surface is yours to fix — inline, as
part of the same work.** When a `clang-format` or `clang-tidy` finding turns up on a
file while you work — even one you didn't otherwise change, or one pulled in
transitively through a header your change includes — just fix it mechanically:
`git clang-format --staged`, `clang-format -i <file>`, or the one-line tidy fix.
**Never** restructure code, split a header, reorder includes, or otherwise reshape
the work to dodge the diff — the fix is mechanical and the tree's clean-format /
clean-tidy invariant is exactly the point. Working around a trivial format/lint
issue instead of fixing it is a defect, not a scope boundary.

**This generalizes past the linters.** Any small defect you surface while working is
yours to fix in the same pass: if fixing it is smaller than describing it, fix it —
a stale comment, a wrong constant, a dangling doc reference, a misnamed field, a
header comment contradicting the code beneath it. Roughly: a mechanical change under
ten lines gets fixed on sight, with a line in the commit body noting it. A module or
file boundary routes the work, it does not excuse it. Defer only when the fix is
genuinely large, changes a contract other work depends on, or needs a decision you
cannot make — and then say so plainly rather than silently narrowing scope. A plan's
"out of scope" bounds that plan's **deliverable**; it never licenses leaving a known
small defect standing in the tree.

### Linting (clang-tidy)

clang-tidy is configured by the repo-root `.clang-tidy` as a **deliberately small
allowlist** (`-*` then the eight checks below): the whole tree is clean against
exactly these, so an enabled run — or the pre-commit hook — is green and any new
finding is a real regression. These checks enforce mechanical conventions and are
**authoritative** — where one contradicts hand-written style, the check wins:

- `readability-braces-around-statements` — **every control-flow body is braced**,
  even a single statement (`if (x) { return; }`, never `if (x) return;`).
- `misc-const-correctness` — **a local that is never mutated is declared `const`**. Values
  and references only: `WarnPointersAsPointers` is **off**, so a pointer local is never
  flagged. That option asks whether a pointee is written through inside the function body
  and nothing more — it proposes `const T*` without checking that the pointer does not
  escape as a mutable `T*`, so on a pointer that is returned, stored into a `vector<T*>`,
  or handed to a C API the suggested `const` does not compile.
- `readability-redundant-member-init` — drop a redundant `{}` on a member whose type
  already default-constructs (`vector`/`optional`/`Ref`).
- `modernize-use-designated-initializers` — aggregate init uses the designated
  `.Field = value` form, matching the `XInfo` house idiom everywhere.
- `modernize-use-scoped-lock` — `std::scoped_lock` over `std::lock_guard`.
- `modernize-use-ranges` — `std::ranges` algorithms over iterator-pair calls.
- `modernize-use-emplace` — `emplace_back` over `push_back(T{...})`.
- `modernize-use-auto` — `auto` when a cast on the RHS already names the type.

The broader `bugprone-*`/`performance-*`/`readability-*` families are **not**
enabled: they are either noisy against this codebase's style or large stylistic
churn not worth the diff. Re-enabling any is a deliberate, separately-scoped pass.
**Identifier naming is not enforced by clang-tidy** either; the house naming rules
are reviewed by hand. Findings are warnings, never build-breaking.

Three ways to run it, all opt-in:

- **In-build:** configure with `-DVENG_ENABLE_CLANG_TIDY=ON` and clang-tidy runs
  per-TU during the build. It is wired only onto veng's own targets, so third-party
  sources are never linted; the `imgui`/`stb`/`tinyexr` vendor aggregation TUs carry
  a `Checks: '-*'` override (`engine/src/Vendor/.clang-tidy`) and the generated
  core-pack embed is `SKIP_LINTING`. The option degrades to a warning if clang-tidy
  is not found.
- **Pre-commit:** the same `.githooks/pre-commit` hook runs a second stage that lints
  **each staged `.cpp`/`.cc` in full** — the tree is kept clean against the allowlist,
  so any finding on a touched TU is a regression and no changed-line diffing is needed.
  Name the compile DB with `$VENG_TIDY_DB` (see below); absent that it falls back to
  searching `build-debug/`, `build/`, then `cmake-build-debug/`, skipping any tree whose
  `CMAKE_HOME_DIRECTORY` names a different checkout. It applies the
  toolchain fixes below. A changed *header*
  is checked transitively through a staged TU that includes it; a header-only commit
  drives no TU, so use the in-build run for those. Skips cleanly when clang-tidy or a
  compile DB is missing. A staged TU the DB carries no entry for — a source belonging to
  no in-tree target, such as the out-of-tree-only `examples/template` — is dropped from
  the invocation and reported by name: clang-tidy lints an unknown file against default
  flags rather than declining it, and that failure poisons the rest of the batch, so
  filtering is what keeps the run meaningful. A commit whose every staged TU is out of
  tree therefore lints nothing, says so, and is allowed through.
- **By hand:** `scripts/tidy.sh --db <build-dir> <sources>` checks the named files against
  that tree's compile DB — no separate lint build tree, and no rebuild between edits
  (clang-tidy reads the source from disk; the DB supplies only flags). Use it to check
  a file you are working on without a tidy-enabled rebuild.

```sh
scripts/tidy.sh --db build-debug engine/src/Gui/Document.cpp
VENG_TIDY_DB=build-debug scripts/tidy.sh engine/src/Gui/Document.cpp   # same, by env
VENG_TIDY_DB=build-debug git commit                                    # the hook
```

**Always name the build tree explicitly — including when it is one of the three the
search would have found.** The script takes `-p` / `--db`; both the script and the hook
read `$VENG_TIDY_DB` (the hook reads only that, since git runs a hook with no arguments).
A checkout routinely carries several build trees — a debug tree, an optional
validation-OFF `build/`, a coverage or sanitizer tree, a worktree's own — and the search
returns the first that exists, which is not necessarily the one your work is built
against. Naming it removes that guess. This matters most for automated work, where a
lint silently run against the wrong tree reads as a clean result.

A tree named this way is used as given or not at all: if it has no
`compile_commands.json`, or was configured for a different checkout, the run **fails**
rather than falling back to the search, because a silent fallback would lint against a
different tree than the one asked for and report the result as if it were the one
requested. **The search is a fallback for convenience, not the intended path.**

**Do not invoke `clang-tidy -p build-debug` directly — it fails without reporting
findings, which reads as a clean tree.** macOS ships no `clang-tidy`, so the tool is
Homebrew LLVM's, and it runs as libTooling rather than as the compiler driver. Three
things in an ordinary build's compile DB break it, and the script exists to handle them:

- **The PCH.** libTooling cannot consume the PCH CMake built — *not* merely because a
  different toolchain wrote it: it is rejected the same way when the build and the tool
  are the same LLVM, because the two compute different module flags (`builtin headers
  belong to system modules ... disabled in precompiled file but is currently enabled`).
  No `--extra-arg` reconciles it. The script strips the `-include-pch` pair from a
  *copy* of the DB; CMake emits `-include <header>` beside it, so the translation unit
  is unchanged, merely uncached.
- **The Vulkan headers.** In a tree configured *without* the sysroot they sit on a path
  the compiler searches implicitly, so CMake records no `-I` for them and a backend TU
  dies on `'vulkan/vulkan.hpp' file not found`. The script exports `CPATH` from the
  build's own `Vulkan_INCLUDE_DIR` to cover that case. A tree configured with the
  sysroot from the start records `-I/usr/local/include` itself (see the next bullet),
  so there the export is redundant but harmless.
- **The sysroot.** Fixed at configure time, not in the script — and **the top-level
  `CMakeLists.txt` now sets it for you**: on Apple, when neither `CMAKE_OSX_SYSROOT` nor
  the `SDKROOT` environment variable names one, it resolves `xcrun --show-sdk-path` and
  caches that, *before* `project()`. So an ordinary `cmake -B build-debug -S . -DVE_DEBUG=ON`
  already records `-isysroot`, and passing `-DCMAKE_OSX_SYSROOT` by hand is redundant.
  It stays honoured when given, and an explicit value or `SDKROOT` still wins.

  The flag matters because `CMAKE_OSX_SYSROOT` is empty by CMake's default, and the
  compiler resolves the SDK implicitly as a driver where libTooling does not — so without
  it every TU dies on `'cstdint' file not found` or a libc++ `mbstate_t` error. That
  failure reports **no findings**, which reads exactly like a clean lint run, so a tree
  missing the sysroot silently checks nothing. It also fixes the identical failure in
  **clangd** and other libTooling-based editor integrations, and `scripts/tidy.sh` refuses
  to run against a DB missing it rather than emit a misleading result. A secondary benefit:
  every tree now carries the same compile lines, so two trees' ccache entries differ only
  where the source does.

  **It has to be set before `project()`, and never added to an existing tree.** CMake strips
  `-I` for any directory it believes the compiler searches implicitly, and it computes that
  list **once**, caching it in `CMakeFiles/<ver>/CMakeCXXCompiler.cmake`. On this host
  `/usr/local/include` is implicit and is where the Vulkan and Slang headers live, so a
  no-sysroot tree emits no `-I` for them and relies on the implicit search. With the sysroot
  in hand before the language is enabled, CMake computes the list without `/usr/local/include`
  and emits `-isystem /usr/local/include`, so everything resolves explicitly. Add the flag to
  an **existing** tree and the cached list is not recomputed: CMake keeps stripping the flag
  while the compiler has stopped searching the directory, and the cooker dies on
  `'slang/slang.h' file not found` — a failure a full step removed from its cause. A tree
  configured before this default landed therefore still wants a from-scratch reconfigure:

  ```sh
  rm -rf build-debug
  cmake -B build-debug -S . -G Ninja -DVE_DEBUG=ON
  ```

**Confirm a run actually ran before believing a clean result.** A toolchain or compile
failure prints `error:` / `Error while processing` **and no findings**, which is
indistinguishable from a clean tree if you only grep for warnings — as is a run whose
file argument does not exist. `scripts/tidy.sh` and `.githooks/pre-commit` both gate on
exactly that ("could not run, so nothing was checked"); preserve it in anything that
wraps clang-tidy, and treat total silence from a hand-rolled invocation as unproven
until you have seen it flag something.

### Coverage

`VENG_ENABLE_COVERAGE` (default `OFF`) adds the gcov flags (`--coverage -O0 -g`) to
veng's own targets, wired at the same boundary as the clang-tidy option so third-party
sources are never instrumented. It also sets `CMAKE_DISABLE_PRECOMPILE_HEADERS` — a
force-included PCH blurs gcov's line mapping, and the coverage flags defeat the object
cache the primary build is tuned for.

`scripts/coverage.sh` is the whole pipeline: it configures a dedicated `build-coverage/`
with the option on, builds it, runs a CTest selection, and emits
`build-coverage/coverage.json` (the machine-readable gcovr report) plus
`build-coverage/coverage-html/index.html`. Coverage never shares `build-debug` — the
`.gcda` counters a run leaves behind would accumulate in a tree used for ordinary work.

```sh
scripts/coverage.sh                                  # every test that runs a compiled binary
COVERAGE_CTEST_ARGS="-L unit" scripts/coverage.sh    # narrow the selection
COVERAGE_CTEST_ARGS="" scripts/coverage.sh           # everything, CMake-script tests included
```

The selection defaults to every test that runs a **compiled binary** and is a single
variable. The default excludes the **CMake-script tests** — the `add_test`s that run
`cmake -P` — for two distinct reasons:

- **They produce no counters for this tree.** `sdk_conformance_*` configures and builds a
  separate, *uninstrumented* SDK tree; `cook_per_config_invalidation` reconfigures and
  rebuilds; `validation_gate` re-runs gpu binaries the selection already ran.
- **`smoke_golden` covers code without asserting much.** It does run instrumented code
  through the launcher, but its only assertion is that a rendered capture matches a golden
  image. The lines it marks covered are not thereby shown correct, so counting them
  inflates the report without evidencing anything — coverage bought this way is worse than
  no coverage, because it reads as reassurance.

Setting the variable **replaces** the default outright, so an explicit value puts them back.

gcov merges the per-process counters, so changing the selection changes only the numbers.
**Narrow with one `-L` and a regex alternation, not repeated `-L` flags** — CTest requires
a test to match *every* `-L` given, so `-L unit -L gpu` selects the tests labelled both,
which is none, and the run reports success over an empty selection. Prerequisites are
**gcovr** and a gcov-compatible backend, chosen with `--gcov-executable` — GNU `gcov`,
or `xcrun llvm-cov gcov` on macOS (what makes the same report reproducible on either
toolchain). The script fails with an actionable message when either is missing rather
than leaving a half-built tree that reads as success. `_deps/` and `tests/` are excluded
from the report.

### Build-time tracing

`VENG_TIME_TRACE` (default `OFF`) adds clang's `-ftime-trace` to veng's own targets, wired at
the same boundary as the clang-tidy and coverage options so vendored sources are never traced.
The compiler then writes one Chrome-Trace `.json` per translation unit **beside that TU's
object file** — build tree → target object directory → one `.json` named after the object
(`build-trace/engine/CMakeFiles/veng.dir/src/Renderer/Context.cpp.json`). Nothing relocates
it; that layout is the contract anything reading the traces scans.

```sh
cmake -B build-trace -S . -DVE_DEBUG=ON -DVENG_TIME_TRACE=ON
cmake --build build-trace
```

**The tracing tree is `build-trace/`** — a sibling of `build-coverage/`, gitignored, so a
tracing configure never disturbs `build-debug`. Three things about it are load-bearing:

- **The option disables the compiler cache, and arranges that itself.** `-ftime-trace` writes
  a side output ccache does not reproduce, so a cache hit yields an object with *no* trace
  beside it — a partial tree that reads as a complete one. The opt-out goes through the single
  existing `CMAKE_CXX_COMPILER_LAUNCHER` assignment (CMake bakes the first launcher value into
  the compile rule and appends later ones, so a second assignment is silently wrong rather
  than an error). This is also why the option is declared at the *top* of the top-level
  `CMakeLists.txt`, beside `VE_DEBUG`/`VE_PROFILE` rather than with the other `VENG_*` options
  further down: it has to be readable when the launcher is assigned. The launcher variable is
  not the only way a cache gets in, either — a fetched dependency can set a **global
  `RULE_LAUNCH_COMPILE`**, which prefixes every target's compile rule tree-wide; that
  injection is forced off (`ASSIMP_BUILD_USE_CCACHE`) unconditionally beside the ccache block,
  ahead of the fetch that would set it, so a tracing tree really does compile uncached.
- **Every `VENG_TIME_TRACE=ON` configure is a full cold build.** Measured on the reference host
  (Apple M2, `-j 6`, 627 TUs): **3:53 of wall clock, ~1097 s of compile CPU, a 3.3 GB tree
  carrying 216 MB of JSON**. That is why it defaults off, and why the tree is a
  deliberately-made, manually-refreshed artifact rather than a by-product. Clean up by deleting
  `build-trace/` outright — objects and JSON are only ever regenerated as a pair.
- **It is a measurement build.** The flag changes what the compiler *does*, not just what it
  emits. Compare a tracing build's timings only against another tracing build's.

Two properties of the format anything reading the traces has to know:

- **A trace's `ts` values restart at zero in every document.** They order events *within* one TU
  and say nothing about where in the build that TU compiled. The only build ordering the tree
  carries is the **trace files' mtimes**, which is what the build span below is computed from.
- **A namespace-scope instantiation placed in a PCH header is serialised into the PCH and is not
  re-paid per TU.** A precompiled header caches parsed declarations *and* whatever was
  instantiated while it was built, so an instantiation added at namespace scope in a PCH-resident
  header appears once, in the PCH, and nowhere else. An experiment expecting to see a per-TU cost
  has to place the instantiation where the PCH does not reach.

#### The PCH's include surface is a public contract, and a heavy header in it is not free

`engine/include/Veng/Veng.h` is the precompiled header for **every** target in this tree, and it
is also what a downstream project's own PCH carries. **What it includes is therefore part of
veng's public API**, in two distinct senses that both bite:

- **Source-compatibility.** Anything `Veng.h` includes is transitively available to every
  consumer TU, so removing an include is a break for a consumer that relied on the transitive
  supply. That is the ordinary include-what-you-use argument and it is the smaller half.
- **Cost.** A PCH caches parsed *declarations*, not template *instantiations*. So a header
  whose instantiations are paid near-universally and used near-nowhere costs **every TU in the
  tree and every TU in every consumer**, regardless of the PCH — free to re-parse, full price to
  re-instantiate, once per unit. Adding an include to `Veng.h` signs the whole tree, and every
  downstream tree, up for that bill.

**The worked case: `<filesystem>` is out of `Veng.h` and out of `assetpack`'s `Asset/Types.h`.**
`std::filesystem::path` is a non-template class whose non-template inline members odr-use member
templates — `wstring()`/`u16string()`/`u32string()` call `path::string<_ECharT>`, `root_path()`
calls `path::append<>`, and `filesystem_error`'s constructors call
`make_shared<_Storage, path, path>`. Every unit that merely *parsed* the class definition
instantiated all of them at end of translation, for `wchar_t`, `char16_t` and `char32_t`, which
nothing in the tree uses. Both headers included `<filesystem>` solely to declare the `Veng::path`
alias, so the alias moved to a header of its own on each side — `Veng/Path.h` and
`Veng/Asset/Path.h` — and the units that name a path include it directly. The alias, its type and
its spelling are unchanged. **Consumer-facing:** a project that relied on `Veng.h` to supply
`<filesystem>` now includes `<Veng/Path.h>` (or `<filesystem>`) itself.

#### A function body odr-uses its whole call graph where the body is parsed

This is the single mechanism behind most of the standard-library instantiation cost this tree has
removed, and it caught four separate sites — twice after the obvious fix had already been tried.
State it exactly:

> **A call inside a function body — template or not — is odr-used at the point that body is
> parsed, unless the call itself depends on a template parameter.**

Three consequences worth having in hand before reaching for a fix:

- **A non-template inline body in a widely-included header costs every includer.** It does not
  matter that nothing ever calls the function. `FindTypeByName`, an inline convenience free
  function that walked `registry.All()`, instantiated `unordered_map<u64, TypeInfo>` in every TU
  that included the header; out-lining it took that to zero.
- **Wrapping the body in `template <class = void>` is not sufficient.** A member template whose
  body calls something that depends on none of its parameters is still odr-used while the class
  definition is parsed. `FieldCollector::Field_` was already a member template and its
  `Fields.push_back(...)` was still paid by every unit that included `Reflect.h`.
- **So there are exactly two fixes**: make the *call* dependent (name the callee's type through a
  template parameter, as `Enumerators()`/`Alternatives()` do), or declare the callee without
  defining it there (out-line the body into a `.cpp`, as `FindTypeByName`, `FieldCollector::Add`
  and `CapturingTestSink`'s methods do). Choosing between them is usually a link question — see
  the `VE_ENUM` link constraint in
  [engine/src/Reflection/CLAUDE.md](engine/src/Reflection/CLAUDE.md).

#### A registry's container storage lives behind an implementation pointer

**No public class definition spells a `std::unordered_map` — or any other heavyweight container —
as a member.** A container named in a class definition is instantiated by every TU that merely
parses the class, so a singleton's storage detail is charged to hundreds of units' view of the
type. The house shape is an `Impl` struct defined in the registry's own implementation TU, reached
through an owning pointer, with every accessor keeping its exact signature — including an `All()`
overload returning a const reference to the map, since naming a specialisation in a return type
needs no complete type, so a caller pays the instantiation only where it actually iterates.

The three registries built this way are **`AssetTypeRegistry`**
([assetpack/CLAUDE.md](assetpack/CLAUDE.md)), **`TypeRegistry`**
([engine/src/Reflection/CLAUDE.md](engine/src/Reflection/CLAUDE.md)) and **`AssetLoaderRegistry`**
([engine/src/Asset/CLAUDE.md](engine/src/Asset/CLAUDE.md)); all three are **move-only** as a
consequence. veng makes no stable ABI promise across versions, so a layout change of this kind has
a consumer **rebuild rather than relink** — a rebuild, not a source change.

#### Guarding the cost

`docs/build-cost-baseline.md` is the tree's checked-in cost baseline and
`scripts/check_build_cost.py` is what produces and checks it. The script parses a tracing tree's
`-ftime-trace` JSON directly — no external analysis tool, no database — and compares the
whole-tree totals and the per-origin instantiation breakdown against the baseline file.

```sh
cmake -B build-trace -S . -DVE_DEBUG=ON -DVENG_TIME_TRACE=ON
cmake --build build-trace -j 6
python3 scripts/check_build_cost.py --tree build-trace                    # check
python3 scripts/check_build_cost.py --tree build-trace --write-baseline   # refresh
```

Exit codes: `0` pass, `1` regression, `2` skipped, `3` inconclusive. **Run it after any change to
the reflection headers, the include graph, or the PCH set.** Whoever lands such a change owns the
number: either the check passes, or the same commit refreshes the baseline **and its body says
why the number moved** — a refresh with no stated reason turns the tripwire into a rubber stamp.

The thresholds: total compile CPU may exceed the baseline by **5 %**; a single origin's
instantiation total may exceed its baseline by **10 % *and* by 5 s absolute**, both conjuncts,
because 10 % of a small origin is drift while 10 % of a large one is a real header change. A
**provenance mismatch skips rather than fails** — compile CPU varies by a factor of ~2 across CPU
models, and a check that fires permanently on everyone else's machine is a check that gets
ignored. It is deliberately **not** a `ctest` test: it needs a tracing tree no ordinary build
produces, so as a test it would be permanently skipped or would force a cold tracing build into
every run.

Three properties of the instrument decide how a delta is read, all three measured rather than
assumed:

- **An outlier cluster makes a run inconclusive (exit 3), not failed.** `-ftime-trace` durations
  are wall time *inside* the compile, so a machine-level stall lands in them as compile cost with
  nothing to distinguish it — one measured instance inflated four adjacent TUs ~170× and the whole
  tree by 57 %. The script scans for a bounded prefix of TUs an order of magnitude above the next
  and reports it as an artifact, because an artifact and a regression are indistinguishable to a
  threshold and reporting one as the other is worse than reporting neither. Recompile the named
  TUs serially and re-run.
- **The build span is provenance, and it is compared before a delta is trusted.** The baseline
  records first-to-last trace mtime. A tree built in one saturated parallel pass and one built
  with idle stretches are not measuring the same machine; a large span mismatch means the totals
  are not comparable, and the honest move is to say so rather than reason about the difference.
- **Whole-tree totals vary ≈11 % run to run on the reference host**, the excess concentrated in
  the last build deciles — thermal behaviour on a fanless machine, not compile cost. So the 5 %
  total threshold **sits below the noise**, and the per-origin rule is the load-bearing half of
  the check. The limitation is documented rather than tuned away: a threshold widened to the
  spread would stop catching the failure the check exists for. Read the figures in this order of
  trust — **the TU count paying an origin first** (a structural count, immune to timing), then the
  median per-TU ratio against the previous tree, then the per-origin totals, then the whole-tree
  total, for completeness only. Proof that the total alone means nothing: a deliberately
  *regressed* tree, built to exceed its baseline, measured 10 % **below** it.

The PCH opt-outs (`SKIP_PRECOMPILE_HEADERS`, and the debug encode TUs matching the PCH's
`__OPTIMIZE__`) are untouched — the flag is orthogonal to both, and those TUs simply trace as
the no-PCH case, which is itself useful data. `-Werror` is unaffected; `-ftime-trace` produces
no diagnostics. The flag is clang-specific, so the compiler test is
`CMAKE_CXX_COMPILER_ID MATCHES "Clang"` (`MATCHES`, since the primary dev host reports
`AppleClang`), with a `message(WARNING …)` on the else branch rather than a silent no-op.

**A downstream project mirroring this over its own build** follows the same shape: a cache
option defaulting `OFF`, declared ahead of whatever assigns the compiler launcher, opting out
of the cache through that one existing assignment, applied at the boundary that already scopes
warnings and precompiled headers to first-party targets, with the per-object output location
left where clang puts it.

### Consuming veng — three modes through one `veng-config`

A game lives **outside** the engine tree and discovers veng as a normal CMake
package. `find_package(veng)` brings `veng::veng`, the imported `vengc` and
`veng-editor` executables (recreated under their unqualified names so
`$<TARGET_FILE:vengc>` resolves), the `veng::graph` / `veng_editor::veng_editor`
library aliases, and the full authoring vocabulary (`veng_add_project` /
`veng_add_game` / `veng_add_editor` / `veng_add_asset_pack`). The same `veng-config`
resolves against three sources:

```
in-tree         add_subdirectory(veng)            hello-triangle, the tests
build tree      find_package(veng) → veng/build   co-develop engine + game, NO install
install prefix  find_package(veng) → <prefix>     the shipped SDK
```

Mode is captured in `VENG_PACKAGE_MODE` (`INSTALL` / `BUILD_TREE`, unset in-tree);
path variables (`VENG_LAUNCHER_MAIN`, `VENG_CORE_SHADER_DIR` / `VENG_CORE_PACK_JSON`
and their consumer-facing lowercase twins) are mode-resolved through the one config.
The build-tree mode works because the engine's `export(EXPORT vengTargets)` writes a
build-tree `veng-config.cmake` that `cmake --build` refreshes in place. A game repo
that declares veng as a pinned `FetchContent` dependency can redirect to a live
checkout with `FETCHCONTENT_SOURCE_DIR_VENG`. The installed `vengc` / `veng-editor`
carry an `INSTALL_RPATH` and require the host's Vulkan SDK (with its Slang component)
at runtime — Slang is not vendored. The full walkthrough is
[docs/guides/consuming-veng.md](docs/guides/consuming-veng.md).

### Dependencies

Pulled via `FetchContent` with pinned tags (fmt, VMA, nfd, tinyexr, stb, ImGui,
imnodes, zstd) — no system install needed beyond Vulkan, GLFW, glm, and zlib
(`find_package`). The load-bearing linkage facts:

- **PUBLIC on `libveng`:** glm, fmt, ImGui, and **nlohmann/json** (the engine's
  `Veng/Reflection/JsonSerialize.h` names `json` types), so every consumer that links
  `veng::veng` resolves them transitively; the SDK export carries
  `find_dependency(nlohmann_json)`.
- **zstd is the one third-party codec linked into `libveng`** (transitively, PUBLIC
  through `assetpack`, which inflates compressed archive blobs at runtime); it adds no
  public-header include.
- **Backend libs (Vulkan, GLFW, VMA, nfd) link PRIVATE** — guarded by the
  `include_hygiene` test (see the Native idiom below).
- **Cooker-only deps** (assimp, Slang, stb, the `bc7enc_rdo` / `astc-encoder` texture
  encoders) are linked into `vengc` alone, never into `libveng` or its consumers.
- **`veng::mcp`** adds one pinned dep of its own — cpp-httplib, vendored, PRIVATE.

### Build configurations — role on the asset, format on the platform

A texture's codec is a **platform** decision, not a per-asset one: a texture's
`*.tex.json` declares a compression **role** (its intent — Color / Normal / Mask /
HDR / UI), never a raw codec, and the active **`BuildConfiguration`** (one
`*.buildcfg` per ship target, listed by the project's `project.veng`) resolves
role → concrete format per platform. The cook emits, per configuration, its packs
plus a cooked project file (`.vengproj`) — the runtime entrypoint naming the packs to
mount and the startup level. A bare `cmake --build` cooks the **host-matching
configuration** by default (`VENG_BUILD_CONFIG`, host-triple-defaulted in
`cmake/BuildConfig.cmake`); override with `-DVENG_BUILD_CONFIG=windows` (always
allowed — the encoder is CPU), and `cook-all-packs` builds every configuration for
CI / ship. The data model is in [engine/CLAUDE.md](engine/CLAUDE.md), the cook
resolution + CMake selection in [cooker/CLAUDE.md](cooker/CLAUDE.md), and the editor
surface + host-capability preview gate in [editor/CLAUDE.md](editor/CLAUDE.md).

### The cook is parallel

The single-render-thread rule at the top of this file governs the **runtime**. The **offline
cook** is the other way round: `vengc` runs a pack's entries across one bounded work pool, and
`--jobs <n>` is that cook's whole concurrency budget — shared by the asset pool and by any
threading inside an importer.

This reaches anyone consuming veng, because **a project's own cook module supplies importers veng
never sees**: `AssetImporter::Concurrency()` returns `ImporterConcurrency::Serialized` unless
overridden, so an importer that says nothing runs under the cook's serialization lock, and
declaring `Parallel` is an assertion with a contract behind it. The budget, the contract (what
`Cook` may assume, what it must not touch, and what `Parallel` asserts), and the cook-module ABI
handshake are in
[cooker/CLAUDE.md](cooker/CLAUDE.md#the-cook-is-parallel--and-an-importer-declares-whether-it-may-be).

### The release build (validation OFF)

`VE_DEBUG=ON` enables Vulkan validation layers (`VE_ENABLE_VALIDATION_LAYERS`), and
the default `build-debug` above turns it on — so validation runs by default. The
validation-**OFF** build is the *optional* one, in its own `build/` dir (both
`build/` and `build-debug/` are gitignored):

```sh
cmake -B build -S .
cmake --build build -j 4
```

Configure it only when you need to check release-only behavior; the debug build is
the default and catches more. Do not build both routinely.

## Verification — read before you trust a green run

- **A green `gpu` band is not a green render.** The rendered output is checked by the
  `smoke_golden` / `hello_triangle_launcher_smoke` pair over the `HT_SMOKE` capture, which is what
  makes `hello-triangle` the verification floor — the capture's contract, its golden-regeneration
  runbook, and the relocatable-set check are in
  [examples/CLAUDE.md](examples/CLAUDE.md#the-ht_smoke-capture-and-the-two-smoke-tests).
- **A parallel cook is guarded by a warm-cook no-op test, not by a byte comparison.** A
  serial-versus-parallel byte comparison was **rejected**, not merely unavailable; the reasoning,
  and what carries the weight instead (the thread sanitiser and `vengc verify`), is in
  [cooker/CLAUDE.md](cooker/CLAUDE.md#what-is-checked-and-what-was-deliberately-not).
- **Validation errors do NOT fail tests by themselves.** The debug-messenger
  callback (`engine/src/Renderer/Backend/Context.cpp`) only `Log::Error`s on
  validation errors — it never aborts. So a green `ctest` under `VE_DEBUG` only means
  something if the validation gate ran: `ctest --test-dir build-debug -j 4 -L
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
`map`, `optional`, `function`; `u8`/`u32`/`u64`/`f32`/`usize`; glm types
as `vec3`, `mat4`, `uvec2`, `quat`. The public API and sample app are written in
these and they are part of veng's identity.

**`path` is one of the aliases but is not in `Veng.h`.** It lives in `<Veng/Path.h>`
(engine) and `<Veng/Asset/Path.h>` (assetpack) — two headers whose entire content is
the alias and the `<filesystem>` include behind it. Spell it `Veng::path` as before;
just include the header where you name one. The reason is in
[Guarding the cost](#guarding-the-cost): `<filesystem>` in a precompiled header is a
per-TU instantiation bill, not a cached parse.

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
how the code got here or what is planned for it. The roadmap lives outside this
repo; git history records the evolution. Neither belongs in a comment.

There are **two tiers** of comment, and the rules below apply to both:

- **Doc comments** sit on a *declaration* — a class/struct, a method or free
  function, a field, an enum and its enumerators, a macro, a public type alias.
  They are **Doxygen** (see below) and describe the API contract for a caller.
- **Inline comments** sit *inside* a function body. They are plain `//` and give
  the local *why* — never a restatement of what the next line does.

**Forbidden in either tier:**
- **Plan/planset citations.** No `(plan 09)`, `(planset-5/05)`, "see plans/…". The
  reader of the code has no reason to care which plan landed it. Strip the
  reference; keep whatever factual statement remains.
- **Future-work / temporariness.** No "for now", "v1 only / later we will",
  "future work", "not yet supported". If a limitation is real, state it as a
  present-tense fact ("veng is single-threaded; no synchronization is provided")
  with no promise about the future.
- **Decorative version tags.** No `v1`/`v2` sprinkled into prose to mean "and a
  later version will differ". Drop the tag and state what the code *is*. A version
  number that the code actually checks — an on-disk format number rejected on
  mismatch — is a real fact and stays; describe it as such.
- **Historical narrative.** No "used to be special-cased inside Context", "ported
  from…", "extracted from…", "the public API no longer exposes barriers". Describe
  the current structure, not the refactor that produced it. Beware `no longer` /
  `previously` / `used to` that contrast with an *older version of the source* —
  cut them. (`previously`/`later` that refer to an earlier/later moment in
  *program execution* — "clear any previously bound pipeline", "a later graph
  pass" — are factual and stay.)
- **Re-documenting the callee at a call site.** A comment at a *usage* site
  explains why *this* code makes *this* call — the local decision, what this
  app/test demonstrates, the constraint that forced it. It does **not** restate
  the general behavior of the type or function being called; that documentation
  lives on the declaration. Test: if you pasted the comment onto the callee's own
  declaration, would it read as that callee's doc comment? If yes, it is
  misplaced — replace it with the local reason, or delete it if the call is
  self-explanatory. When one engine contract recurs at many call sites, document
  it once and reference it (or say nothing) at the rest.

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
is here; (b) restates a contract documented elsewhere — reference it (`see
ReconfigureScene`) rather than re-deriving it; or (c) inventories structure the code
already shows. Keep the load-bearing why; drop the tour. (Doc comments on a public
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
comment, it is a `@brief` block exactly like a public one. Plain `//`/`///` prose
without `@brief` is only for *inline body* comments, which are never doc comments.

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
  blank `///` line, then any detailed description (rationale prose lives here,
  still bound by the rules above).
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
through `X::Create(const XInfo&)`: synchronous, returning a ready `Ref<T>`. A
higher-level engine **asset** that carries CPU source data and *uploads* it — `Mesh`,
`Texture`, `Material` — is built at runtime through **`AssetManager`**: `Build<T>(...)`
is **async by default** (returns a pending `AssetHandle<T>`), `BuildSync<T>(...)` the
blocking sibling — the same async-default rule as `Load`/`LoadSync` and
`Upload`/`UploadSync`. So the verb tells you the tier *and* the sync/async
expectation: `Create` → a GPU object, now; `Build`/`BuildSync` → an asset, streaming
or blocking. The full asset-tier model (the `Prepare`/`Finalize` seam, `Adopt`) is in
[engine/src/Asset/CLAUDE.md](engine/src/Asset/CLAUDE.md).

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

**Hold engine resources as members and let the destructor release them** — `Application`'s
teardown order guarantees every service a release touches is still alive, and a resource that
outlives the context fails loudly rather than silently. A shutdown *operation* that is not a
resource release belongs in `OnShutdown()`, not in a destructor. The ordering, the `OnShutdown`
seam and the `Disposed` tripwire are in [engine/CLAUDE.md](engine/CLAUDE.md#application).

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
public header while linking only veng's PUBLIC deps. Vulkan, GLFW, VMA, and nfd
link PRIVATE, so if a public header leaks a backend include, this test fails to
build. CMake `PUBLIC`/`PRIVATE` linkage is load-bearing here — keep the PUBLIC
deps PUBLIC and the backend libs PRIVATE.

## Working norms

The **engine roadmap lives at the workspace root** — in `plans/`, a sibling of this repo,
tracked by neither (it is kept out of the public engine tree; see the workspace `CLAUDE.md`).
`plans/README.md` indexes the **plansets** (numbered coherent phases) and `plans/future/` (a
vision/holding area). Each planset/future README carries the detail, decisions, and per-plan
status column.

**Plan work** — one planset per session, on the user's cue, dispatching its plans as
appropriate (independent plans in parallel, dependent plans in sequence, derived from
the plans' direction). Per plan:
1. Implement it.
2. Migrate **both** `examples/hello-triangle` (consumed **in-tree**) and
   `examples/template` (consumed **out-of-tree** via `find_package(veng)`) in the *same*
   pass as the breaking changes — together they are the dual-mode conformance check
   ([examples/CLAUDE.md](examples/CLAUDE.md)). The template is **not** built by the
   default in-tree `cmake --build`, so a template breakage surfaces in the SDK
   conformance tests (`sdk_conformance_install` / `sdk_conformance_buildtree`, the
   `gpu` band), not in a plain build.
3. **Run `clang-format` and `clang-tidy` (`scripts/tidy.sh --db build-debug`) before this
   verifying build, not as a pre-commit afterthought** — clang-tidy edits code (it adds
   `const`, `auto`, …), so running it first lets those edits ride this build and test pass
   rather than forcing a second build+test cycle after. Then verify against the default debug
   build only (clean `build-debug`, `ctest` green, `hello_triangle-launcher` under `HT_SMOKE`
   writes a correct-sized PPM) — don't also do a separate release build. The template renders
   no golden; its conformance tests configure + build it standalone, run `template-launcher`
   under `TEMPLATE_SMOKE` and check both its exit status and what it logged, then probe
   `veng-editor --version`.
4. Update the plan's status column in the roadmap (at the workspace root).
5. Commit the code, one commit per plan in this repo, with a descriptive-sentence
   subject (match `git log`) and a `Co-Authored-By` trailer. Roadmap-only edits — status
   columns, planset drafts — live at the workspace root and are committed to neither repo.

**When a new `AssetId` is needed**, use a clearly-marked placeholder id while
implementing — don't break flow to mint one mid-task. Once the build is working
and verified, mint the real ids with `vengc generate-id` (optionally with
`--reference <pack.json>` flags for existing packs) and replace the placeholders.
Never invent a final id manually. All ids in the codebase, including the core
pack's built-in layout ids, were minted this way.

**Hardcoded `AssetId` literals in C++ are written in uppercase hexadecimal, `0x`-prefixed
and zero-padded to 16 digits** (`AssetId{0x0D49F2A1C03B5E76ULL}`). JSON stores every minted
id (`AssetId`/`SystemId`/`ActionId`) as the *same* zero-padded hex value, as a **string**
(`"0x0D49F2A1C03B5E76"`) — a string round-trips through any JSON tool losslessly, where a
bare number past `2^53` silently truncates through an IEEE-754-double pipeline. The shared
codec is `Veng/Asset/HexId.h`, and `scripts/migrate_ids.py` converts an out-of-repo pack to
this form. `vengc generate-id` prints the id in both spellings — `0x{:016X}ULL` for C++ and
`"0x{:016X}"` for JSON.

**Driving a running MCP server.** When a task needs to inspect or manipulate a live
engine instance — a running game or an open editor — reach it through the MCP client
rather than adding one-off instrumentation. Any exe built with the MCP opt-in
(`veng-editor`, or a game's `<name>-launcher` built with `veng_add_game(... MCP)`)
exposes a `--connect=<port>` client against an already-running server's loopback port.
**Discover the surface before using it, don't assume it**: `--connect=<port> --list`
(optionally `--search <query>`) enumerates every tool with its description — list
first, then call, never guess a tool name or its arguments. The call grammar and
exit-code map are in [mcp/CLAUDE.md](mcp/CLAUDE.md).
