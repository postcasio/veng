# Plan 02 — const/exposed `Param`, generated `MaterialParams`, the sub-asset pipeline

**Goal:** ship the emit walk's output through the cook. A `Param` gains a **const-vs-exposed** flag;
the emitter generates the per-material **`MaterialParams`** struct and the `.vmat` **field list**
together (so they agree by construction); and the editor writes the generated fragment as a
**checked-in sub-asset** (`<name>.gen.frag.{slang,shader.json}` with a derived `AssetId`), adds its
manifest row, repoints `.vmat` `shaders.fragment`, and drives the existing cook-on-demand →
hot-reload → `MaterialPreview` loop on generated source. This closes the graph→cook→reload→preview
loop end to end. Depends on **Plan 01** (the emit walk) and **Plan 00** (the importable header).

## Why it is its own plan

Plan 01 makes Slang text in the editor; this plan makes that text a cooked, rendering material. The
two halves it adds — the generated `MaterialParams`/field-list (so the cooker's offset-patching has a
struct to reflect) and the sub-asset emit/cook plumbing — are what turn "we can generate a shader"
into "the graph drives the rendered material." Keeping it distinct from the emit walk isolates the
on-disk/manifest/cook-loop changes (the riskier, integration-heavy part) from the pure codegen core.

## What lands

- **`Param` gains a const-vs-exposed flag.** A node property (a `bool`/enum on the `Param` POD): a
  **const** param emits its value inline as a Slang literal (`float4(0.8,0.2,0.1,1)`) — no uniform;
  an **exposed** param contributes a field to the generated `MaterialParams` and emits a `p.<Name>`
  read, runtime-tweakable through the existing ring-buffered block. The flag rides the `"_editor"`
  graph round-trip (an additive property, schema-tolerant on read).

- **The generated `MaterialParams` struct + the `.vmat` field list, produced together.** The emit
  walk now collects, in one pass: each `TextureSample`'s handle slot (`uint <Name>; uint <Name>Sampler;`)
  and each **exposed** `Param`'s field (`float`/`vec2..4 <Name>;`) → the generated struct (written at
  the top of the generated `.slang`, after `#include "Veng/material.slang"`); and the matching
  **field list** — the texture `AssetId`s + the exposed-param default values — for the `.vmat`.
  Because the same walk produces the struct and the list, the cooker's reflected `MaterialParams`
  offsets and the material's packed values **agree by construction** (the invariant the loader's
  offset-patching relies on). `WriteMaterialVmat`/`CompiledField` are reworked to emit *this*
  generated field list rather than one matched against a loaded shader.

- **The generated fragment sub-asset.** On compile/save, the editor writes two files beside the
  `.vmat`:
  - `<name>.gen.frag.slang` — the emitted source (`#include "Veng/material.slang"` + the generated
    `MaterialParams` + the `fsMain` entry from Plan 01);
  - `<name>.gen.frag.shader.json` — names the `.slang`, the `fsMain` entry, and the standard surface
    vertex-layout id (the same `*.shader.json` shape a hand-authored shader uses).

- **A derived, stable `AssetId` + a manifest row.** The generated shader's id is a **deterministic
  transform** of the material's `AssetId` (a fixed bit-mix / reserved high bits — never minted, never
  `generate-id`), so it is stable across regenerations and collision-free against authored ids. The
  editor ensures the pack manifest carries the generated shader's `{ id, type: "shader", source }`
  row (added once, idempotent on resave), and the `.vmat` `shaders.fragment` points at the derived id.

- **The cook-on-demand loop drives generated source.** `MaterialEditorPanel`'s edit → 300ms-debounced
  `RequestCook` now cooks the generated shader (via the existing `ShaderImporter`, with Plan 00's
  include resolving) **and** the material that references it, mounts the result via `MountMemory`, and
  hot-reloads behind the stable `AssetHandle` — the same loop as today, with generated Slang as the
  cook input. `MaterialPreview` re-fetches and renders the regenerated material.

- **The cooker stays graph-agnostic.** It compiles the stored `<name>.gen.frag.slang` like any
  shader; it never sees `NodeGraph`. An offline `vengc cook` with no editor running compiles the
  checked-in generated files exactly as it compiles hand-authored ones. **No cooked-material format
  change and no runtime change** — the material still resolves its fragment by id and loads plain
  SPIR-V + reflection.

## Decisions

1. **Const-vs-exposed is one `Param` with a flag.** Not two node types — the flag cleanly partitions
   "folds inline" from "becomes a uniform," so the generated `MaterialParams` is exactly the set of
   exposed params and the field-list/defaults follow without a second authoring concept.
2. **The struct and the field list come from one walk.** Generating both together is what guarantees
   the cooker's reflected offsets match the packed values — the offset-patching invariant — without a
   cross-check step.
3. **The generated shader is a checked-in sub-asset with a derived id.** Reuses the entire
   `ShaderImporter` cook path and the material→shader reference (zero runtime/format change), keeps an
   offline cook working with no editor, and keeps the **cooker graph-agnostic**. The derived id avoids
   minting on every regenerate; checking the files in keeps a no-editor build buildable. The cost
   (generated files in the tree + manifest) is the conventional, accepted one.
4. **Generation runs in the editor, on save.** The emitter consumes `NodeGraph`, which the cooker
   cannot link — so the editor emits and writes; the cooker only compiles text. This is the same
   `_editor` (edit source) + regenerated-artifact split the system already uses for `fields`.
5. **`MaterialParams` is generated and per-material.** Plan 00 moved the struct out of the engine
   header precisely so codegen can generate it; the cooker reflects it like a hand-authored one.

## Files

| File | Change |
|---|---|
| `editor/src/material/MaterialCatalog.h` / `.cpp` | Add the const-vs-exposed property to `Param`; its emit-fn branches inline-literal vs. `p.<Name>`; collect the generated-struct field set. |
| `editor/src/material/MaterialCompile.h` / `.cpp` | Produce the generated `MaterialParams` struct text + the `.vmat` field list from one walk; rework `WriteMaterialVmat`/`CompiledField` to emit the generated list + `shaders.fragment` = derived id. |
| `editor/src/material/` (a new emit/write helper) | Write `<name>.gen.frag.{slang,shader.json}`; compute the derived `AssetId`; ensure the manifest row (idempotent). |
| `editor/src/panels/MaterialEditorPanel.cpp` | Route edit → emit + write sub-asset → `RequestCook` (generated shader + material) → `MountMemory` hot-reload → `MaterialPreview` re-fetch. |
| `editor/src/material/MaterialShaderInterface.h` | The schema source is now the generated field set, not a loaded shader's `GetFields()` (the binding-model coupling drops). |
| `examples/hello-triangle/assets/` (manifest) | The generated-shader row appears once `brick.vmat` is migrated (Plan 04); the pipeline is exercised here by a round-trip test fixture. |
| `editor/CLAUDE.md` | Document the sub-asset pipeline: derived id, generated files, the cooker-graph-agnostic contract. |

## Verification

- Clean build; `ctest` green. A round-trip test: a material graph → emit → write sub-asset → cook
  (offline, via the cooker) → load → assert the material binds the generated fragment and packs the
  exposed-param defaults at the reflected offsets; a const param produces no `MaterialParams` field.
- The editor edit loop works end to end: editing a `Param` value live-recooks the generated shader
  and updates `MaterialPreview` within the debounce window (manual/preview verification noted).
- An **offline `vengc cook`** of a pack containing a generated material (no editor running) compiles
  the checked-in `<name>.gen.frag.slang` and produces a valid material blob.
- `smoke_golden` does **not** move yet — no shipping material is migrated until Plan 04; this plan's
  fixtures are test-only.
- `include_hygiene` unaffected; validation gate clean (the generated shader binds set-0 bindless +
  the standard surface attachments, identical to a hand-authored material).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
