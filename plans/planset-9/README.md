# planset-9 — game-module build model, pipeline cache & archive hashes

**Phase goal:** make a veng game a **shippable product** and harden the shipping path.
Three streams bundle here because they share that theme. They are **independent in
design** — each can be implemented and reviewed on its own — but not fully file-disjoint:
A and B both edit the sample, and B also touches `Application`/`Context`. They are not
equal in weight either — **Stream A carries the real design risk** (a new build model + ABI
boundary); B and C are smaller shipping-hygiene changes. Review A on its own merits, not
under a blanket "all low-risk, all parallel" banner:

- **Stream A — game-module build model** (the headline). A game stops being a
  self-contained `.exe` and becomes a **`libgame` (shared) + a thin launcher** that
  `dlopen`s it through one C-ABI entry point — the foundational change the **editor**
  (future area 6) stands on.
- **Stream B — pipeline cache.** `Context` gains a `VkPipelineCache` (reused within a run)
  with **opt-in disk persistence** — a warm-start win for the shipped product.
- **Stream C — archive content hashes.** The `.vengpack` format carries a **content hash
  per cooked blob** + a **table-of-contents digest**, cooker-written and `vengc
  verify`-checked — integrity for shipped packs.

This is bigger than a usual planset and is honestly **three threads, not one refactor** —
A is the first sub-area of **future area 6** ([game-module.md](../future/game-module.md),
[editor.md](../future/editor.md)), while B and C each resolve a **cross-cutting concern**
from [future/README.md](../future/README.md). They are independent in design but not fully
file-disjoint: A's plan 02 and B's plan 04 both edit the sample (the one ordering
constraint, below), and B also edits `Application.h`/`Context.*`. C is the only fully
isolated stream. None adds to the engine's render/load hot paths.

---

## Stream A — game-module build model

A game splits into `libgame` (shared, the runtime: `Application` logic, components, custom
runtime asset types; **no editor code**), a thin **launcher** (the shipped exe), and —
reserved for the editor planset — `libgame_editor`. Today `examples/hello-triangle`
subclasses `Application`, defines `main()`, and links `libveng` into one binary; its types
are private to that process. The split lets a host load a game's native code, which the
editor needs.

### Decisions (stream A)

1. **One game becomes three targets; planset-9 builds two.** `libgame` + launcher ship
   here. `libgame_editor` cannot be built yet (`libveng_editor` does not exist) — it is
   **reserved for the editor planset**, the ABI shaped to accept it (decision 4) without
   pulling it forward.

2. **A single C-ABI entry point: `VengModuleRegister(VengModuleHost*)`.** One exported
   symbol, resolved by name after load, the same for every module and host — C++ has no
   reflection, so a module hands the host a table by **registering** through that call.
   (Resolves the doc's "single entry vs. multiple exports" → single.)

3. **`VengModuleHost` carries inert registries, not live engine objects.** Registration is
   a **factory** (no GPU work), so the entry point needs **no live `Context`/`AssetManager`**:
   the host is `{ ApplicationRegistry& App; EditorRegistry* Editor; }` (no
   `Context&`/`AssetManager&`), and **`Application`
   keeps owning `Context`/`AssetManager`/`TaskSystem` unchanged** — the launcher constructs
   it via the factory and `Run()` builds the engine objects as today. This **departs from
   the design doc's sketch** (`Context&`/`AssetManager&` on the host): passing live objects
   in would force inverting `Application`'s ownership for no v1 benefit and contradict the
   doc's "nothing new in the engine runtime." The editor planset adds its reflection
   registry to the host **additively** (decision 5). One consequence is explicit:
   registering **custom asset-type loaders** (which `future/game-module.md` cites as the
   reason for the host's `Assets&`) genuinely needs a live `AssetManager`, so it is
   **out of scope** here, not merely "not needed" — there is no custom asset type to
   drive it yet, and it returns with the host's `AssetManager&` when one does.

4. **`EditorRegistry*` is forward-declared and always null here.** A pointer to an
   incomplete type `libveng` only forward-declares — fixing the ABI seam the editor planset
   fills in **now**, without building `libveng_editor` now. The launcher passes null; only
   the future editor host passes non-null. This keeps editor code out of `libgame` and this
   planset while keeping the entry signature stable across both hosts.

5. **Type reflection is the editor planset's first task, not this one's.** A reflection
   layer (`TypeRegistry`/`FieldDescriptor`/`TypeDescriptor`) has **no consumer in
   planset-9** — its clients are the editor's auto-inspectors and a future serializer — so
   designing it here is the speculative-infrastructure trap the roadmap warns against. It is
   **deferred to the editor-shell planset**, designed against the real inspector. The
   direction is recorded so that planset does not rebuild it blind: field types are an
   **open `TypeId`** (engine builtins — incl. `AssetHandle<Texture/Mesh/Material>` —
   pre-registered identically to a game's own asset types), **not** a closed engine enum, so
   a game shipping a new asset type extends the vocabulary with no engine change; a small
   closed `FieldClass` meta-kind carries generic handling; inheritance is **single,
   non-virtual, base at offset 0**, walked base-first.

6. **The launcher is generic and veng-provided; both hosts use the uniform entry.** veng
   ships one `launcher_main.cpp`; `veng_add_game` compiles it per-game with the module name
   baked in. The module registers its `Application` factory through `VengModuleRegister`;
   the launcher loads, calls the entry, constructs the app, and runs it.

7. **The smoke capture routes through the launcher + loader — one code path.** The manual
   `HT_SMOKE` capture runs the launcher (which `dlopen`s `libhello_triangle`), exercising
   the real shipping shape. (The registered `headless_smoke` ctest links `veng::veng`
   directly and is untouched.)

8. **Visibility stays default-visible; no hidden-visibility flip.** The top-level CMake
   deliberately sets `CMAKE_CXX_VISIBILITY_PRESET default`, so module and launcher already
   see the engine's symbols. This planset adds the module-entry export path but does **not**
   flip `libveng` to `-fvisibility=hidden` — that hardening bites only on the Windows port
   (where `VE_API`'s dllimport/dllexport is load-bearing) and is bundled there.

9. **Same toolchain, one STL, one flag set — not a binary-plugin platform.** Module and
   host are built by the same compiler/STL/`-fno-exceptions`, recompiled from one tree —
   what makes passing `string`/`vector`/`Ref<T>` safe. The entry is C-ABI; the payload is
   C++. A **one-integer ABI version handshake** (`VengModuleAbiVersion`, checked by the
   loader before the entry runs) makes "a stale module fails loudly at load" concrete.

10. **Game-code hot-reload is out; v1 restarts the play session.** Distinct from *asset*
    hot-reload (the async/editor path), which is unaffected.

---

## Stream B — pipeline cache

Both pipeline-creation sites pass `nullptr` for the cache today
(`GraphicsPipeline.cpp:183` / `ComputePipeline.cpp:37`), so nothing is reused even between
two pipelines in one process. A context-owned cache fixes that, with opt-in persistence.

### Decisions (stream B)

1. **In-memory cache always; disk persistence opt-in.** `Context` creates a
   `vk::PipelineCache` at device init unconditionally (intra-run reuse is free); persisting
   it is opt-in — a library writing files by default is surprising.
2. **The app names the path; veng does not guess a cache directory.** Driven by
   `ApplicationInfo::PipelineCachePath` (`optional<path>`): set → seed + write; `nullopt`
   (default) → in-memory only. Platform cache-dir policy is the app's/launcher's call.
3. **Trust Vulkan's header validation.** A stale/foreign/truncated blob is ignored by the
   driver (cold rebuild); veng feeds it as `pInitialData` and never parses it.
4. **Seed at init, write once at shutdown.** No periodic flush, no multi-cache merge.
5. **Pipeline creation is main-thread, so the cache needs no external sync** — a
   present-tense constraint, not a feature; off-thread creation would need it.
6. **Headless behaves identically** (device-level, swapchain-independent) — the smoke path
   proves the round-trip hardware-free.

---

## Stream C — archive content hashes

The `.vengpack` format (`Archive.h`) is at `ArchiveFormatVersion = 1` with a per-asset TOC
and already rejects a version drift with `VersionMismatch` — so a hash field is a clean
bump to **2**. Locking the field in now is the point (the roadmap: *"cheaper now than a
format-version bump later"*).

### Decisions (stream C)

1. **Per-blob hash primary; a table-of-contents digest rides along** — the per-blob
   granularity enables integrity, dedup, and incremental cooking. The digest covers the
   **serialized TOC bytes** (not a hash-of-the-per-blob-hashes), so it catches TOC-level
   tampering — reordered entries, edited `id`/`offset`/`size`, a per-blob hash field
   swapped — that re-hashing the blobs alone cannot, and it needs no separate sort step.
2. **The loader never verifies; verification is a separate `vengc verify` tool.** Hashing on
   load is slow and the hashes are tooling, not the hot path — the runtime trusts its packs.
3. **A fast non-crypto hash (xxh3-128); this is not a security feature.** The hash function
   lives **only** in the cooker/verify tool; `assetformat` stores the 16 raw bytes and
   computes nothing, so it (and `libveng`) gain **no dependency**.
4. **Bump the version; require a re-cook — no v1 reader.** Packs cook at build time; v2
   supersedes v1.
5. **Dedup + incremental cooking are enabled, not built** — they build on the field with no
   further format bump.
6. **Endianness assumption unchanged** (the format already assumes a shared cook/run host).

---

## Scope of this phase

| In scope | Out of scope (later / other phases) |
|---|---|
| **A:** the C-ABI `VengModuleRegister`/`VengModuleHost` (app factory + null `Editor` slot), the ABI version handshake, `ModuleLoader`, `ApplicationRegistry`, a relocatable generic launcher, `ExecutableDirectory()` + executable-relative asset/cache resolution + pack-beside-launcher copy, `veng_add_game(...)` (in-tree), the `hello-triangle` migration + an automated launcher smoke test | **Custom runtime asset-type / loader registration** (the host carries no `AssetManager&`; deferred until a custom asset type exists to drive it — see decision 3); **installed-package wiring** of `veng_add_game`/launcher for downstream `find_package(veng)` consumers (`Game.cmake`/`AssetPack.cmake`/`launcher_main.cpp` install + `veng-config`); the type-reflection layer (editor planset); `libgame_editor` + the `EDITOR` arm of `veng_add_game`; `EditorRegistry` + the editor host; inverting `Application`'s ownership; hidden-visibility audit / Windows port; game-code hot-reload |
| **B:** a context-owned `vk::PipelineCache` threaded into both factories; opt-in `ApplicationInfo::PipelineCachePath` seed+write; sample opts in | A veng-chosen default cache directory; off-thread pipeline creation (would need cache sync); periodic flush / multi-cache merge |
| **C:** per-blob `ContentHash` + table-of-contents digest (format v2); cooker computes (xxh3-128) + writes; `assetformat` round-trips (no hash dep); `vengc verify`; re-cook packs | Dedup (content-addressed storage); incremental cooking (input-hash cache); the loader verifying on load; a crypto/tamper hash; v1 backward reading |

## Plans

| # | Plan | Stream | Summary | Status |
|---|---|---|---|---|
| 01 | [Module ABI + `ModuleLoader`](01-module-abi-loader.md) | A | `VengModuleHost` + the `extern "C" VengModuleRegister` entry + the export macro + the `VengModuleAbiVersion` handshake + the `ApplicationRegistry` app-factory slot (defined here — the host references it by value, so it must be complete); `ModuleLoader` (cross-platform `dlopen`/`LoadLibrary`) that loads, verifies the ABI version, resolves the entry, surfaces failures as `Result`. Proven by a test module + a wrong-version variant + a loader integration test. No sample change. | done |
| 02 | [Launcher + `veng_add_game` + sample migration](02-host-cmake-sample.md) | A | A generic launcher (consuming plan 01's `ApplicationRegistry`) that `dlopen`s its module **by name** via an `$ORIGIN`/`@loader_path` rpath; `veng_add_game(...)` → `lib<name>` + `<name>-launcher` side-by-side + the pack copied beside the launcher; assets resolve via a new `ExecutableDirectory()` so the launcher + lib + pack are a **relocatable trio** (in-tree only; installed-package wiring is out of scope). Migrate `hello-triangle`; `HT_SMOKE` through the launcher + an automated launcher smoke test. | proposed |
| 03 | [Context-owned in-memory cache](03-in-memory-cache.md) | B | A `vk::PipelineCache` in `Context::Native` + a `GetVkPipelineCache` accessor, threaded into both pipeline factories. No disk; pixels unchanged. | done |
| 04 | [Disk persistence (opt-in)](04-disk-persistence.md) | B | `ApplicationInfo::PipelineCachePath`: seed from the file at init, write at shutdown; `nullopt` keeps plan 03 behaviour. Sample opts in; headless smoke proves the round-trip. | proposed |
| 05 | [Format v2 + cooker writes hashes](05-format-and-write.md) | C | `ContentHash` per TOC entry + a header digest over the serialized TOC bytes; `ArchiveFormatVersion` → 2; `assetformat` round-trips (no hash dep); cooker computes xxh3-128 + writes; loader reads-but-ignores; re-cook packs. | done |
| 06 | [`vengc verify`](06-verify-tool.md) | C | A `vengc verify <archive>` subcommand: re-hash blobs + digest, report per-asset, exit nonzero on any mismatch. Cooker-side only. | proposed |
| 07 | [Docs + roadmap re-cut](07-docs-roadmap.md) | — | One pass over `plans/README.md`, `CLAUDE.md`, and the future docs for all three streams: area 6's game-module prerequisite delivered (reflection handed to the editor), pipeline-caching + content-hashes cross-cutting concerns resolved. | proposed |

## Dependencies & dispatching

Three internally-ordered chains converge on the docs plan:

```
Stream A (game-module):   01 ──► 02 ─┐
Stream B (pipeline cache): 03 ──► 04 ─┼──► 07  (docs, last)
Stream C (archive hashes): 05 ──► 06 ─┘

A, B, C are independent in design; C is fully file-isolated, A and B share only the
sample (02 before 04). 07 depends on all six.
```

**Subsystem ownership** (why the streams don't collide):

- **A** — `engine/include/Veng/Module/*`, `engine/src/Module/*`, `engine/src/Launcher/*`,
  the `VE_MODULE_EXPORT` macro in `Veng.h`, `cmake/Game.cmake`, the top-level `CMakeLists`
  include, `examples/hello-triangle`.
- **B** — `Context.cpp`, `Native.h`, `GraphicsPipeline.cpp`, `ComputePipeline.cpp`,
  `Application.h` (`ApplicationInfo`), `examples/hello-triangle`.
- **C** — `assetformat/Archive.{h,cpp}`, the cooker (`libveng_cook` + `vengc`, xxHash
  vendor), the cooked packs. **Fully isolated** — no shared files with A or B.

**The one merge point:** A's plan 02 and B's plan 04 both edit
`examples/hello-triangle/main.cpp` (A moves the `ApplicationInfo` into `VengModuleRegister`;
B adds `PipelineCachePath` to it). So **land 02 before 04** — then 04 sets the path inside
the migrated entry. (`Application.h` is touched only by B, no conflict.) C shares nothing.

**Dispatching plan:**

- **Recommended single-threaded order:** `01 → 02 → 03 → 04 → 05 → 06 → 07`. This respects
  every intra-stream edge and the 02-before-04 merge point, and matches the house "one plan
  per session" cadence. Streams may be reordered freely (e.g. C first) **except** keep 02
  before 04.
- **If parallelizing** (separate sessions or worktree subagents): **C is safe to run fully
  in parallel** with A and B — it shares no files. **A and B can overlap** except their two
  sample-touching plans (02, 04) must be serialized (02 then 04) or rebased; the rest of B
  (03) is independent of A.
- **Keep on the main thread:** each stream's contract-setting first plan — **01** (the ABI
  contract), **03** (the cache object + threading + the no-sync decision), **05** (the
  format/version decision) — plus **02** (the launcher/CMake/sample shape) and **07** (docs).
- **Good `model: sonnet` delegation** once those contracts are fixed: 01's platform
  `dlopen`/`LoadLibrary` wrapper; **04**'s file I/O + opt-in plumbing (but keep the teardown
  ordering and the Vulkan-fetch-fatal/file-write-recoverable split — plan 04's `Dispose()`
  sequencing — on the main thread); 05's xxHash vendor + per-blob compute loop; **06** (the
  `verify` subcommand + its cooker-suite tests). Spawn C's delegations in a separate worktree
  to exploit its isolation.
- **Commits:** one per plan (`Plan NN: …`), intra-stream order preserved, **07 last**.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the same
pass (plans 02 and 04 touch it) → verify (clean build, `ctest` green, smoke binary writes a
correct-sized PPM, 1280×720 RGB ≈ 2,764,816 bytes) → update this table → one commit per
plan, `Plan NN: <summary>` with a `Co-Authored-By` trailer (`planset-9:` for the docs
plan).

- **Public headers stay backend-free.** A's module-ABI headers (C-ABI entry, C++ payload —
  `Module/Module.h`, `Module/ModuleLoader.h`, `Module/ApplicationRegistry.h`, **hand-added
  to `tests/include_hygiene.cpp`**, which is a manual `#include` manifest, not
  auto-discovered), B's `Native.h` accessor (the raw cache handle lives only there), and
  C's `Archive.h` POD field are all covered by the **`include_hygiene`** test — every plan
  keeps it green.
- **Plans 01, 05, 06 add no GPU work** (ABI/loader; archive format; cooker tool) — their
  tests are driver-free (`add_test` for `loader_test`; the cooker cases live in `veng_unit`
  under `-L unit`, there is no separate `cooker` label). **Plans 02, 03, 04 draw GPU
  resources** and must pass the `VE_DEBUG` validation gate
  (`ctest --test-dir build-debug -L validation`); none changes the render path materially,
  so **no plan may widen the allowlist** (it is empty).
- **The smoke PPM is non-deterministic** — verify size + exit 0, never golden-compare. After
  plan 02 the manual capture runs `hello_triangle-launcher`; plan 07 updates that command in
  `CLAUDE.md`.
- **The engine gains no hash dependency** (C) and **no reflection layer** (A) — both are held
  out deliberately; `assetformat`/`libveng` stay as light as today.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

A game builds as a shared library a thin launcher loads through one versioned C-ABI entry
(stream A); `Context` reuses pipeline compilation across a run and optionally across runs
(B); `.vengpack` archives carry verifiable content hashes (C); `hello-triangle` ships as
`libhello_triangle` + a launcher exercising all three. Update
[plans/README.md](../README.md) with the planset-9 line, mark **area 6's game-module
prerequisite delivered** (reflection handed to the editor-shell planset) and both the
**pipeline-caching** and **content-hashes** cross-cutting concerns **resolved** in
[future/README.md](../future/README.md). The editor area's remaining sub-areas, **area 7**
(scene/entity model), and the editor host land later against the ABI seam this planset
fixed; **dedup** and **incremental cooking** are unblocked by C's hash field.
