# Plan 00 — cross-pack Slang includes + the material header split (area 14)

**Goal:** make a consumer (and, next plan, a *generated*) Slang fragment shader able to
`#include` the engine's material contract directly, instead of vendoring a hand-synced copy. Two
coupled changes: the cooker's Slang sessions gain the engine core shader directory on their search
path, and `engine/assets/core/shaders/material.slang` splits into an **engine contract** half
(imported by consumers) and a **per-shader** half (`MaterialParams`, which moves to the authoring
shader). This is [future area 14](../future/README.md#14-engine-owned-material-shader-header--cross-pack-slang-includes--prioritized),
taken as the codegen precursor. It changes **no SPIR-V** — every shader compiles to the same bytes,
so `smoke_golden` does not move. **Foundational; nothing precedes it.**

## Why it is its own plan

A generated fragment shader (Plan 02) must `#include` the engine's bindless/`GBufferOutput`/
push-block declarations and the per-domain fragment-input struct — and **nothing can today**: the
cooker adds only the source file's *own* directory to the Slang search path
([SlangReflect.cpp:209](../../cooker/src/Importers/SlangReflect.cpp),
[SlangReflect.cpp:312](../../cooker/src/Importers/SlangReflect.cpp),
[ShaderImporter.cpp:249](../../cooker/src/Importers/ShaderImporter.cpp), all `searchPathCount = 1`).
That is exactly why [examples/hello-triangle/assets/shaders/material_data.slang](../../examples/hello-triangle/assets/shaders/material_data.slang)
is a byte-for-byte hand-synced copy of the engine header (it even re-inlines `view_constants.slang`
because it cannot reach the core pack). Landing the include path + the header split with
*hand-authored* shaders first proves the import in isolation, so Plan 02's generated shaders inherit
a working `#include` and a clean header to target.

## What lands

- **A shared Slang-session search-path helper.** The three session setups call one helper that sets
  `searchPaths` to `{ sourceFileDir, engineCoreShaderDir }` (the engine core shader dir is the
  directory the cooker already cooks the core pack from, threaded to the importers). `searchPathCount`
  becomes 2. The helper is the single place the search-path policy lives, so the three sites stop
  hand-rolling a one-element array.

- **`material.slang` splits along the engine-contract / per-shader line.** The **engine contract**
  stays in `material.slang` (the importable header): the set-0 bindless declarations
  (`g_Textures`/`g_Samplers`/`g_MaterialParams` + `LoadMaterialParams` + `MaterialParamStride`), the
  `g_ViewConstants` block (via its existing `#include "view_constants.slang"`, now reachable),
  `GBufferOutput`, `DrawData`/`LoadDrawData`/`g_DrawData`, `PushConstants`/`g_PC`,
  `ComputeMotionVector`, and the **per-domain fragment-input struct** (the `VSOutput` interpolants a
  surface fragment reads — `sv_position`/`v_UV`/`v_WorldNormal`/`v_WorldTangent`/`v_MaterialIndex`/
  `v_CurClip`/`v_PrevClip` — and the screen-UV struct a PostProcess fragment reads). A generated or
  hand-authored fragment shader's `fsMain(VSOutput input)` signature comes from here.

- **`MaterialParams` moves out into the authoring shader.** The struct is **per-shader by
  definition** — the cooker reflects each material shader's own `MaterialParams` to pack its fields
  at the reflected offsets, so a single "shared" copy that must stay byte-identical across engine and
  consumer is conceptually wrong (and under codegen it is generated per material). `brick.frag.slang`
  and `character.frag.slang` declare their own `MaterialParams` beside `#include "Veng/material.slang"`.

- **Both vendored `material_data.slang` copies are deleted** (hello-triangle and template), and the
  three example fragment shaders (`brick`, `character`, `flat`) switch their first line from
  `#include "material_data.slang"` to `#include "Veng/material.slang"` and declare their own
  `MaterialParams`. The include spelling (`Veng/material.slang`) is the engine namespace prefix the
  search-path helper resolves.

- **The C++ mirror `Veng/Renderer/MaterialParams.h` is retired.** There is no fixed engine param
  struct (planset-18 made the runtime block reflection-sized), so the `static_assert`-guarded C++
  mirror of the shader struct no longer mirrors anything engine-owned; it is removed and any include
  of it dropped.

## Decisions

1. **The engine header is imported, not vendored.** Material declarations are the engine's concern;
   a consumer `#include`s them. The vendored copies existed only because the cooker could not resolve
   a cross-pack include — this plan removes that constraint and the copies with it.
2. **The split line is engine-contract vs. per-shader.** Anything the engine *defines* and a shader
   *must agree with* (bindless bindings, `GBufferOutput`, the push block, the interpolant struct)
   stays in the engine header. Anything *per-material* (`MaterialParams`) moves to the authoring
   shader. The line is the same one codegen draws (Plan 02 generates a per-material `MaterialParams`).
3. **The search-path helper is the single policy site.** The three session setups share it, so a
   future search-path change (another core dir, a project shader dir) is one edit, not three.
4. **No SPIR-V change.** The split is a textual reorganization — the same declarations compile to the
   same bytes from the same call sites. `smoke_golden` does not move, and the validation gate is
   unaffected (no binding, layout, or output change).

## Files

| File | Change |
|---|---|
| `cooker/src/Importers/ShaderImporter.cpp`, `cooker/src/Importers/SlangReflect.cpp` | Replace the three `searchPathCount = 1` setups with the shared helper adding `{ sourceDir, engineCoreShaderDir }`. |
| `cooker/src/Importers/` (the session helper) | New shared free function building the `SessionDesc` search paths; threaded the engine core shader dir. |
| `engine/assets/core/shaders/material.slang` | Keep the engine contract (bindless, `g_ViewConstants`, `GBufferOutput`, `DrawData`, `PushConstants`, `ComputeMotionVector`, the per-domain fragment-input struct); remove `MaterialParams`. |
| `examples/hello-triangle/assets/shaders/material_data.slang`, `examples/template/assets/shaders/material_data.slang` | Deleted. |
| `examples/hello-triangle/assets/shaders/{brick,character}.frag.slang`, `examples/template/assets/shaders/flat.frag.slang` | `#include "Veng/material.slang"`; declare their own `MaterialParams`. |
| `engine/include/Veng/Renderer/MaterialParams.h` | Deleted; drop its includes. |
| `cooker/CLAUDE.md`, `engine/CLAUDE.md` | Document the cross-pack include resolution + the header split (the vendored-copy note retires). |

## Verification

- Clean build; `ctest` green. The cooker compiles `brick`/`character`/`flat` through the new
  `#include "Veng/material.slang"` with no missing-symbol diagnostics.
- `smoke_golden` does **not** move — the reorganized declarations compile to identical SPIR-V.
- The relocatable-set + launcher smoke tests still pass (the core pack + example packs cook and load
  unchanged).
- Validation gate clean under `VE_DEBUG` (no binding/layout/output change).
- `include_hygiene` unaffected (deleting `MaterialParams.h` removes a header, leaks nothing).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
