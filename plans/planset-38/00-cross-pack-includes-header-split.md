# Plan 00 — cross-pack Slang includes + the material header split (area 14)

**Goal:** make a consumer (and, next plan, a *generated*) Slang fragment shader able to
`#include` the engine's material contract directly, instead of vendoring a hand-synced copy. Two
coupled changes: the cooker's Slang sessions gain an engine shader-include directory on their search
path, and `engine/assets/core/shaders/material.slang` splits into an **engine contract** half
(imported by consumers) and a **per-shader** half (`MaterialParams`, which moves to the authoring
shader). This is [future area 14](../future/README.md#14-engine-owned-material-shader-header--cross-pack-slang-includes--done-planset-38),
taken as the codegen precursor. It changes **no SPIR-V** — every shader compiles to the same bytes,
so `smoke_golden` does not move. **Foundational; nothing precedes it.**

## Why it is its own plan

A generated fragment shader (Plan 02) must `#include` the engine's bindless/`GBufferOutput`/
push-block declarations and the per-domain fragment-input struct — and **nothing can today**: the
cooker adds only the source file's *own* directory to the Slang search path (the three
`searchPathCount = 1` sites — `SlangReflect.cpp:216`, `SlangReflect.cpp:319`,
`ShaderImporter.cpp:256`; cite the `searchPathCount = 1` token, which won't drift). That is exactly
why [examples/hello-triangle/assets/shaders/material_data.slang](../../examples/hello-triangle/assets/shaders/material_data.slang)
is a byte-for-byte hand-synced copy of the engine header (it even re-inlines `view_constants.slang`
because it cannot reach the core pack). Landing the include path + the header split with
*hand-authored* shaders first proves the import in isolation, so Plan 02's generated shaders inherit
a working `#include` and a clean header to target.

## What lands

- **An engine shader-include directory, threaded to every cook.** The cooker takes a new
  `--shader-include <dir>` flag (an absolute path to the engine core shader dir,
  `engine/assets/core/shaders/`), set by the `add_asset_pack`/`add_project` CMake from a
  `${VENG_CORE_SHADER_DIR}` cache variable and carried on `CookContext`. The relocatable/offline case
  resolves it the same way other shipped paths do (relative to the tool, or an explicit flag). The
  test cooks (`tests/gpu`, `tests/cooker`) pass the flag from their harness.

- **A shared Slang-session search-path helper.** The three session setups call one helper that sets
  `searchPaths` to `{ sourceFileDir, engineShaderIncludeDir }` with `searchPathCount = 2`, **source
  dir first** so a local file always wins over a same-named engine file. The helper is the single
  place the search-path policy lives; the three sites stop hand-rolling a one-element array.

- **`material.slang` splits along the engine-contract / per-shader line.** The **engine contract**
  stays in `material.slang` (the importable header): the set-0 bindless declarations
  (`g_Textures`/`g_Samplers`/`g_MaterialParams` + `LoadMaterialParams` + `MaterialParamStride`), the
  `g_ViewConstants` block (via its existing `#include "view_constants.slang"`, now reachable),
  `GBufferOutput`, `DrawData`/`LoadDrawData`/`g_DrawData`, `ComputeMotionVector`, the **two
  domain-keyed push blocks** (see below), and the **per-domain fragment-input struct** (the `VSOutput`
  interpolants a surface fragment reads — `sv_position`/`v_UV`/`v_WorldNormal`/`v_WorldTangent`/
  `v_MaterialIndex`/`v_CurClip`/`v_PrevClip` — and the screen-UV struct a PostProcess fragment reads).
  A generated or hand-authored fragment shader's `fsMain` signature comes from here.

- **The push block is domain-keyed, in the header.** A Surface fragment reads its material selector
  from the `v_MaterialIndex` interpolant (the surface push block `{ FrameBase; ViewConstantsIndex; }`
  is the vertex stage's), while a PostProcess fragment has no such interpolant and reads
  `g_PC.MaterialIndex` from its own push block (`tonemap.frag.slang` declares one today). The header
  therefore carries **both** push declarations under distinct names (a surface push block and a
  PostProcess push block); a fragment uses the one for its domain. This is the contract Plan 01's
  PostProcess entry-point emit reads from.

- **`MaterialParams` moves out into the authoring shader.** The struct is **per-shader by
  definition** — the cooker reflects each material shader's own `MaterialParams` to pack its fields
  at the reflected offsets, so a single "shared" copy that must stay byte-identical across engine and
  consumer is conceptually wrong (and under codegen it is generated per material). `brick.frag.slang`
  and `character.frag.slang` declare their own `MaterialParams` beside `#include "Veng/material.slang"`.

- **All four vendored `material_data.slang` copies are deleted**, and every shader that included one
  switches to `#include "Veng/material.slang"` and declares its own `MaterialParams`:
  - `examples/hello-triangle/assets/shaders/material_data.slang` — included by `brick.frag.slang`,
    `character.frag.slang`.
  - `examples/template/assets/shaders/material_data.slang` — included by `flat.frag.slang`.
  - `tests/gpu/assets/shaders/material_data.slang` — included by `tests/gpu` `brick.{vert,frag}.slang`.
  - `tests/cooker/fixtures/shaders/material_data.slang` — included by `tests/cooker/fixtures`
    `brick.{vert,frag}.slang`.

  The include spelling (`Veng/material.slang`) is the engine namespace prefix the search-path helper
  resolves; core includes are reached *only* via the `Veng/` prefix.

- **The C++ mirror `Veng/Renderer/MaterialParams.h` and its drift-guard test are retired.** There is
  no fixed engine param struct (planset-18 made the runtime block reflection-sized), so the
  `static_assert`-guarded C++ mirror no longer mirrors anything engine-owned; it is removed, any
  include of it dropped, and **`tests/unit/material_params.cpp`** — whose entire body is
  `#include <Veng/Renderer/MaterialParams.h>` plus `sizeof`/`offsetof` asserts — is deleted along with
  its CMake registration. The layout invariant it guarded becomes a codegen-time ordering rule + a GPU
  test in Plan 02.

## Decisions

1. **The engine header is imported, not vendored.** Material declarations are the engine's concern;
   a consumer `#include`s them. The vendored copies existed only because the cooker could not resolve
   a cross-pack include — this plan removes that constraint and the copies with it.
2. **The split line is engine-contract vs. per-shader.** Anything the engine *defines* and a shader
   *must agree with* (bindless bindings, `GBufferOutput`, the domain push blocks, the interpolant
   struct) stays in the engine header. Anything *per-material* (`MaterialParams`) moves to the
   authoring shader. The line is the same one codegen draws (Plan 02 generates a per-material
   `MaterialParams`).
3. **The push block is domain-keyed.** Surface and PostProcess push different per-draw data; the
   header declares both so a fragment of either domain includes the right one. Plan 01's generated
   PostProcess entry reads `g_PC.MaterialIndex` from the PostProcess block.
4. **The search-path helper is the single policy site, source-dir-first.** The three session setups
   share it, so a future search-path change is one edit; source-dir-first keeps a consumer file from
   being shadowed by a same-named engine file.
5. **No SPIR-V change.** The split is a textual reorganization — the same declarations compile to the
   same bytes from the same call sites. `smoke_golden` does not move, and the validation gate is
   unaffected (no binding, layout, or output change).

## Files

| File | Change |
|---|---|
| `cooker/src/Importers/ShaderImporter.cpp`, `cooker/src/Importers/SlangReflect.cpp` | Replace the three `searchPathCount = 1` setups with the shared helper adding `{ sourceDir, engineShaderIncludeDir }` (source first). |
| `cooker/src/Importers/` (the session helper) | New shared free function building the `SessionDesc` search paths from the threaded include dir. |
| `cooker/include/Veng/Cook/Importer.h`, `cooker/src/` (CLI) | `CookContext` gains the engine shader-include dir; `vengc` gains `--shader-include`. |
| `cmake/` (`add_asset_pack`/`add_project`), test CMake | Pass `${VENG_CORE_SHADER_DIR}` as `--shader-include`; the `tests/gpu`/`tests/cooker` cooks pass it too. |
| `engine/assets/core/shaders/material.slang` | Keep the engine contract (bindless, `g_ViewConstants`, `GBufferOutput`, `DrawData`, both domain push blocks, `ComputeMotionVector`, the per-domain fragment-input struct); remove `MaterialParams`. |
| `examples/hello-triangle/assets/shaders/material_data.slang`, `examples/template/assets/shaders/material_data.slang`, `tests/gpu/assets/shaders/material_data.slang`, `tests/cooker/fixtures/shaders/material_data.slang` | All deleted. |
| `examples/.../{brick,character,flat}.frag.slang`, `tests/{gpu,cooker/fixtures}/.../brick.{vert,frag}.slang` | `#include "Veng/material.slang"`; declare their own `MaterialParams`. |
| `engine/include/Veng/Renderer/MaterialParams.h`, `tests/unit/material_params.cpp` (+ its CMake) | Deleted; drop any includes. |
| `cooker/CLAUDE.md`, `engine/CLAUDE.md` | Document the cross-pack include resolution + the header split (the vendored-copy note retires). |

## Verification

- Clean build; `ctest` green. The cooker compiles every migrated shader through the new
  `#include "Veng/material.slang"` with no missing-symbol diagnostics, and the `tests/gpu`/
  `tests/cooker` cooks resolve the engine include via the flag.
- `smoke_golden` does **not** move — the reorganized declarations compile to identical SPIR-V. Add a
  spot SPIR-V diff (old vs new module for `brick.frag`) to confirm "no change" rather than relying only
  on the fuzzy image compare.
- The relocatable-set + launcher smoke tests still pass (the core pack + example packs cook and load
  unchanged).
- Validation gate clean under `VE_DEBUG` (no binding/layout/output change).
- `include_hygiene` unaffected (deleting `MaterialParams.h` removes a header, leaks nothing).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
