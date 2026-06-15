# Plan 07 — Docs + roadmap re-cut

> **Final plan; depends on all six.** Land after streams A (01–02), B (03–04), and C
> (05–06) are all done — it documents what they delivered. Roadmap-only; no code changes
> (commit `planset-9: …`).

**Goal:** record everything the three streams delivered across the living docs in one
pass — the roadmap index, `CLAUDE.md`, and the future docs — and hand the editor-only
remainder (including the deferred reflection layer) forward cleanly.

## `plans/README.md`

Add the planset-9 line after planset-8, in the established voice. planset-9 bundles three
independent shipping-hygiene streams: **(A)** the game-module build model — a game becomes
`libgame` (shared) + a thin launcher that `dlopen`s it through one C-ABI
`VengModuleRegister` entry (with an ABI version handshake and a relocatable launcher),
registering its `Application` factory; `hello-triangle` ships as `libhello_triangle` + a
launcher and the smoke runs through it; **(B)** a context-owned `VkPipelineCache` with
opt-in disk persistence; **(C)** per-blob content hashes + a table-of-contents digest in the
`.vengpack` format (v2), cooker-written and `vengc verify`-checked. Note that stream A is
**future area 6's first sub-area** (the editor's prerequisite) with its type-reflection
layer **deferred to the editor-shell planset**, and that B and C resolve two **cross-cutting
concerns** (pipeline caching; content hashes).

## `CLAUDE.md`

The README I keep current omits in-flight plansets, so do **not** add a planset-9 bullet
here. The `CLAUDE.md` edits are the durable conventions the three streams introduce:

**Stream A — shared-library game model.** A new short section (near "Application" /
"Assets"): a game is a `libgame` (shared, the runtime) loaded by a thin **launcher** (the
shipped exe) through a single C-ABI `VengModuleRegister(VengModuleHost*)` entry; the module
registers its `Application` factory into a host-owned `ApplicationRegistry`; `Application`
still owns `Context`/`AssetManager`/`TaskSystem` unchanged — the host constructs it via the
factory and calls `Run()`. State the same-toolchain ABI rule (C entry, C++ payload; modules
recompiled with the engine), the `VengModuleAbiVersion` handshake (a mismatch is rejected at
load), the **relocatable trio**: the module resolves beside the launcher via
`$ORIGIN`/`@loader_path` and assets resolve via `ExecutableDirectory()` (the new public
helper) with `veng_add_game` copying the pack beside the launcher — so launcher + lib +
pack move as one directory; the reserved-but-null `EditorRegistry*` seam (the future editor
host's), and that game-code hot-reload is out (restart the session); name
`veng_add_game(...)` as the build entry and `ExecutableDirectory()` as the
executable-relative path helper. Update the **Layout** section (the example is now a shared
lib + launcher, not one binary) and the **Verification** section (the `HT_SMOKE` binary is
now `hello_triangle-launcher`, with the automated `hello_triangle_launcher_smoke` test and
the relocated-trio check).

**Stream B — pipeline cache.** In the renderer/Context area: `Context` owns a
`VkPipelineCache` reused across every pipeline build; persistence is opt-in via
`ApplicationInfo::PipelineCachePath`. State that a stale/foreign cache file is safe (Vulkan
validates the header; veng never parses the blob) and that the cache is touched only on the
single render thread (no external sync; off-thread pipeline creation would need it).

**Stream C — content hashes.** In the **Assets** section: `.vengpack` archives carry a
content hash per cooked blob + a table-of-contents digest (over the serialized TOC bytes;
format v2), cooker-written and checkable with **`vengc verify`**. State that **the loader never verifies** (hashes are
tooling, not the hot path — the runtime trusts its packs) and that the hash function lives
only in the cooker/verify tool (xxh3-128), so `assetformat` and `libveng` gain no hash
dependency.

Keep every edit factual and present-tense (no plan citations, per the comments rule).

## `plans/future/README.md`

- **§6 Editor application** — mark the "Games become a shared library + a launcher" bullet
  delivered by planset-9 (the shared-lib + launcher + C-ABI app registration shipped); note
  the **type-reflection layer (`TypeRegistry`) moved into the editor-shell sub-area**
  (designed against the inspector), and that `libgame_editor`/`EditorRegistry`/the editor
  host are unchanged and still future. Update the **Ordering & dependencies** block and
  **Status**: the editor area's first sub-area is done.
- **Cross-cutting concerns** — mark **"Pipeline caching"** resolved (context-owned cache +
  opt-in `ApplicationInfo::PipelineCachePath`), noting a default cache directory and
  off-thread creation stay future; mark **"Content hashes in the vengpack archives"**
  resolved (per-blob hash + digest, format v2, `vengc verify`, loader untouched), noting
  **dedup** and **incremental cooking** are now unblocked with no further format bump.

## `plans/future/game-module.md`

Trim to enduring seams; mark the build model delivered (pointer to
[planset-9](README.md)). **Retain** the editor-only forward work: the **type-reflection
layer** (now the editor-shell planset's first task), rewritten to the resolved direction
(open `TypeId`, not a closed enum; engine builtins — incl. `AssetHandle<…>` —
pre-registered like a game's own; a closed `FieldClass` meta-kind; single non-virtual
inheritance, base at offset 0, walked base-first); `libgame_editor` + the
`EditorRegistry` definition + the `EDITOR` arm of `veng_add_game`; and the **installed-package
wiring** of `veng_add_game` for downstream `find_package(veng)` consumers (install the helper
`.cmake` files + `launcher_main.cpp`, wire `veng-config.cmake.in`, ship `libveng` beside the
launcher — `veng_add_game` ships in-tree only in planset-9). **Record the resolved
decisions** (single C-ABI entry; inert host registries — no live `Context`/`AssetManager`,
a departure from the doc's sketch, and why; reflection deferred to the editor with the
open-`TypeId` direction; uniform launcher entry + version handshake + relocatable
resolution; smoke through the loader; default-visible — hidden-visibility audit deferred to
the Windows port; hot-reload out).

## `plans/future/editor.md`

Mark sub-area **A (game-module build model)** delivered by planset-9; note **B (editor
shell) now also owns the type-reflection layer** (pulled out of the prerequisite to be
designed against the inspector). C (material editor) and area 7 (scene model) unchanged.

## Acceptance

- `plans/README.md` carries the planset-9 line covering all three streams; `CLAUDE.md`
  documents the shared-lib game model, the pipeline cache, the content-hash format +
  `vengc verify`, and the updated `HT_SMOKE` command; the future docs reflect area 6's
  prerequisite as delivered (reflection handed forward) and both cross-cutting concerns as
  resolved.
- This planset's README status column reads `done` across all seven plans.
- No code changes; `ctest` remains green (docs-only).
