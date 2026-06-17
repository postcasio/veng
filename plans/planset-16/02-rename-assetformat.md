# Plan 02 — rename `assetformat` → `assetpack`

**Goal:** rename the shared archive/cooked-blob library from `assetformat` to **`assetpack`**.
A pure rename — directory, CMake target + alias, and the comments that name the library. **No
header path, API, or on-disk format change.** Behaviour-preserving.

## Why `assetpack`

`assetformat` undersells what the library is. It owns more than a serialization layout: the
asset identity types (`AssetId`, `AssetType`), the archive container (`Archive`), and the
cooked-blob definitions (`CookedBlobs`) — the Vulkan-free, importer-free **contract** shared by
the cooker (producer) and the engine (consumer). The archive extension the project already
ships is **`.vengpack`**, so `assetpack` names the library after the concrete artifact it
defines and the rest of the codebase already references. `libveng_assetpack` reads as "the
library that defines packs."

(Alternates weighed and rejected: `assetcore` — generic, says nothing about contents;
`assetwire` — "wire" implies a network format the library is not.)

## Scope

This is a **mechanical rename**, not a restructure. The public include path is **unchanged** —
headers stay under `Veng/Asset/` (`#include <Veng/Asset/Archive.h>` etc. are untouched), because
the include directory is the namespace path, not the library name. Only the library's *own* name
changes.

| In scope | Out of scope |
|---|---|
| Rename directory `assetformat/` → `assetpack/` (`git mv`, preserving the `include/Veng/Asset/` tree) | Any change to `Veng/Asset/` header paths or `#include` lines |
| CMake: `libveng_assetformat` → `libveng_assetpack`, alias `veng::assetformat` → `veng::assetpack`, install export name | Any API / type / `AssetId` / `FieldClass` change |
| Update `add_subdirectory(assetformat)` → `add_subdirectory(assetpack)` in top-level `CMakeLists.txt` | On-disk `.vengpack` format (byte-identical) |
| Update `target_link_libraries` references in `engine/CMakeLists.txt` + `cooker/CMakeLists.txt` | Renaming the `.vengpack` extension (already correct) |
| Update comments naming the library across engine/cooker/test sources (all are comments — see below) | `Veng::` namespace, file names inside the lib |
| Refresh `CLAUDE.md` + `plans/README.md` (the layout bullet + present-tense prose mentions) | Historical `plans/planset-*/` + `plans/future/` files (immutable roadmap history) |
| Verify: clean build, full `ctest` green, smoke PPM correct size + exit 0 | — |

## The edits

**Directory + CMake (the load-bearing changes):**

- `git mv assetformat assetpack`.
- `assetpack/CMakeLists.txt` — rename the target `libveng_assetformat` → `libveng_assetpack`,
  the alias `veng::assetformat` → `veng::assetpack`, the header comment, and the `install(TARGETS
  …)` entry. The `target_include_directories` path stays `include/` (the `Veng/Asset/` tree is
  unmoved within the renamed dir).
- `CMakeLists.txt` (top level) — `add_subdirectory(assetformat)` → `add_subdirectory(assetpack)`;
  update the two ordering comments (line ~158/160) and the unit-band comments (~248–252) that
  name `assetformat`.
- `engine/CMakeLists.txt` — `veng::assetformat` → `veng::assetpack` in `target_link_libraries`
  and the adjacent PUBLIC-linkage comment.
- `cooker/CMakeLists.txt` — `veng::assetformat` → `veng::assetpack` in both
  `target_link_libraries` lines and the header comment.

**Comments only (no code change — every remaining source hit is a comment naming the library):**

- `engine/include/Veng/Asset/AssetLoader.h` — "Cooked blob (assetformat layout)".
- `engine/include/Veng/Reflection/Serialize.h` — "no assetformat dependency".
- `engine/include/Veng/Renderer/ShaderInterface.h`, `engine/src/Asset/Loaders/{Texture,Shader,
  VertexLayout}Loader.cpp` — library-name mentions.
- `cooker/include/Veng/Cook/{Types,AssetPack}.h`, `cooker/src/Importers/*` — "assetformat's
  vendored aliases" / "assetformat's ArchiveWriter" mentions.
- `tests/include_hygiene.cpp` ("come from libveng_assetformat"), `tests/unit/asset_archive.cpp`,
  `tests/cooker/{texture,mesh}_cook.cpp`, `tests/cooker/cook_roundtrip.cpp`.

After the edits, `grep -rn 'assetformat' . --include='*.h' --include='*.cpp' --include='*.txt'
--include='*.cmake'` over the source + build files (excluding `build*/` and `.claude/worktrees/`)
must come back empty — that is the source-of-truth acceptance check.

**Docs:** the roadmap needs care, because the historical plan files legitimately record the
library under its old name:

- **`CLAUDE.md`** — rename the `assetformat/` layout bullet to `assetpack/` and update every prose
  reference (the Layout section, the cooker/engine link descriptions, the Build & test dependency
  note). Keep it a present-tense rename — no "formerly assetformat" narrative (comment/doc policy).
  *(Note: `CLAUDE.md` is also edited by plan 01's reflection paragraph; if 01 and 02 land in
  parallel worktrees their `CLAUDE.md` edits are non-overlapping but will need a trivial merge —
  see the README's dependency analysis. Sequencing 01 before 02 avoids it.)*
- **`plans/README.md`** — update the two present-tense library references (the planset-5 archive
  summary and the planset-9 hash-dependency note) to `assetpack`. This is the living roadmap
  index; its architectural descriptions name the current library.
- **Leave the historical `plans/planset-*/` plan files and `plans/future/` untouched** — they
  record what past plansets delivered at the time; retroactively renaming them would falsify the
  history and is why the grep above excludes `.md` files. `plans/planset-16/`'s own files keep
  using `assetformat` where they describe *this* rename.

## Note on planset fit

planset-16 is a **grab bag** of independent small wins; this rename is win 2. It shares no files
with the reflection refactor (win 1, plans 00/01 — `engine/include/Veng/Reflection/`) or the
composite pass (win 3, plan 03 — the renderer/core pack), touching only the library shell
(directory + CMake target/alias) and comments, so it can land in any order.

## Verification

`cmake -B build -S . && cmake --build build -j 2` clean; `ctest --test-dir build
--output-on-failure` green across the unit/death/cooker bands (and `gpu` where a device is
present); the smoke launcher writes a correct-sized PPM and exits 0. The on-disk `.vengpack`
artifacts are byte-identical (no format touched). `include_hygiene` still builds (linkage names
changed, not the public-header surface).

## Acceptance

The library builds and links as `libveng_assetpack` / `veng::assetpack`; no `assetformat` token
survives in source/build files (`*.h`/`*.cpp`/CMake, excluding `build*/` + `.claude/worktrees/`);
the `Veng/Asset/` include path and the `.vengpack` format are unchanged; full `ctest` green;
`CLAUDE.md` and `plans/README.md` read `assetpack` throughout (historical plan files unchanged).
Commit: `Plan 02: rename assetformat → assetpack (library shell + comments; no API/format change)`.
