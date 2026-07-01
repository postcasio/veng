# libveng_cook + vengc — the offline cook pipeline

`cooker/` is `libveng_cook` plus the `vengc` CLI: the offline toolchain that turns
hand-written JSON asset sources into the binary `.vengpack` archive the runtime
mounts. It is **never linked by the engine**. The on-disk archive format it emits is
documented in [assetpack/CLAUDE.md](../assetpack/CLAUDE.md); runtime loading of the
result (the `AssetManager`, `AssetHandle`, async/sync `Load`) and the full
shader/material model are in [engine/CLAUDE.md](../engine/CLAUDE.md). Project-wide
conventions live in the [root CLAUDE.md](../CLAUDE.md).

`libveng_cook` links **`veng::graph`** PUBLIC — the shared node-graph + material-codegen
library (see [editor/CLAUDE.md](../editor/CLAUDE.md)). A material graph is walked into Slang
fragment source by the **same** `CompileMaterialGraph` emit walk the editor runs, so the
editor preview and the offline cook generate identical text by construction. The walk
(`EmittedValue`, the per-node-type emit-fns, the schema-independent catalog) lives in
`veng::graph`, not the cooker, so it carries no Slang/JSON dependency of its own.

## Toolchain isolation

The cooker's heavy/toolchain deps — **stb, assimp, Slang** (shader compile +
reflection), and **nlohmann/json** — are **cooker-only**: linked into `vengc`
alone, never into `libveng` or its consumers, which load the *binary* archive and
never parse a source asset. The split is the whole point — the runtime gains no
importer, no source parser, no Slang, and no re-cook path.

`vengc` (and `veng::graph`) build **unconditionally from a source build** — veng
*is* the tools, so there is no build toggle to skip them (the retired
`VENG_BUILD_TOOLS`). A library-only consumer instead uses `find_package(veng)` and
gets `vengc` as a **prebuilt imported executable** in `vengTargets`; `veng-config`
recreates the unqualified `vengc` name so `$<TARGET_FILE:vengc>` resolves in every
consumption mode. The installed `vengc` carries an `INSTALL_RPATH` and **requires the
host's Vulkan SDK / Slang present to run** — Slang is not vendored. A downstream cook
resolves `--reference` / `--shader-include` against the installed core-data source
under `share/veng/core/` (`veng_CORE_PACK_JSON` / `veng_CORE_SHADER_DIR`).

The **content-hash function lives only here** (and in `vengc verify`): the cooker
writes each blob's xxh3-128 hash and the TOC digest into a `.vengpack` (format v5),
so `assetpack` stores the raw 16 bytes and computes nothing, and `libveng` gains no
hash dependency. The loader never verifies — hashing is tooling, not the hot path.

The cooker also **zstd-compresses each blob** before adding it, storing whichever of the
raw or compressed bytes is smaller (an incompressible blob keeps the raw, zero-copy
resolve path). Both `ArchiveWriter::Add` sites route through one `EmitBlob` helper, so
every blob is considered for compression by construction; the content hash covers the
**stored** bytes, so `vengc verify` re-hashes exactly what is on disk with no decode.

The **texture-encoder deps — `bc7enc_rdo` (BC7) and ARM's `astc-encoder` (ASTC LDR) —
are cooker-only**, linked into `vengc` alone and never into `libveng`, like
stb / assimp / Slang. The runtime samples the cooked block-compressed format directly; only
the cook encodes it.

## What `vengc` cooks

An asset pack is a pure `{ id, type, source }` manifest carrying no per-asset
settings; every asset type — texture, mesh, shader, material, prefab — has its own
per-asset JSON source (`*.tex.json` / `*.mesh.json` / `*.shader.json` / `*.vmat.json`
/ `*.prefab.json`) the manifest entry points at. The importers validate those sources
at cook time:

- **Shaders** are authored in **Slang**. The cooker **always** compiles from source
  (there is no precompiled-inline path) and **reflects the shader offline** into a
  serializable `ShaderInterface`; the engine then loads plain SPIR-V. (There is no
  `glslc` / `add_shaders` path — GLSL was removed project-wide.)
- **A fragment shader's source can be a graph, not a `.slang` file.** A `*.shader.json` whose
  `"source"` ends in `.graph.json` (plus a `"domain"`: `surface` | `postprocess`) is cooked by
  walking the graph into Slang fragment text via the shared `veng::graph` emit walk, then
  compiling and reflecting that text through the **same** `ShaderImporter` / `SlangReflect`
  SPIR-V path a hand-authored `.slang` takes — Slang loads it from a source string, since the
  generated text has no file on disk. The walk runs behind a resolver hook
  (`ResolveGraphShaderSource` in `Importers/GraphShaderSource.{h,cpp}`, installed via
  `SetGraphShaderResolver`) so the shader/material importers call it through a function pointer
  rather than statically linking the walk; the bootstrap path with no resolver installed treats
  every source as a plain `.slang`. The emit walk generates the per-shader `MaterialParams`
  struct **and** the matching `.vmat` field list together, so the reflected offsets and the
  material's packed values agree. The **generated Slang and SPIR-V are cook output, never
  checked in** — the authored graph is the single source of truth (the cooker can dump the
  generated `.slang` to the build dir for debugging), and the cooker runs the identical walk the
  editor preview runs, so the two cannot diverge.
- **The engine material header is imported, not vendored.** Every Slang session (the
  `ShaderImporter` compile and the `SlangReflect` struct/fragment-output reflections) builds
  its search paths through one helper (`BuildSlangSearchPaths`, in `Importers/SlangSession.{h,cpp}`):
  `{ sourceFileDir, engineShaderIncludeDir }`, **source dir first** so a local file always wins
  over a same-named engine file. The engine core shader dir is threaded as `--shader-include <dir>`
  (`CookContext::ShaderIncludeDir`, set by the `veng_add_asset_pack` / `veng_add_project` CMake from
  `${VENG_CORE_SHADER_DIR}`; the editor's cook-on-demand fills it from the core pack's own
  directory). A consumer (or generated) `.slang` therefore `#include`s the engine's material
  contract directly as `#include "Veng/material.slang"` — the engine header at
  `engine/assets/core/shaders/Veng/material.slang` holding the bindless declarations,
  `g_ViewConstants`, `DrawData`, `GBufferOutput`, `ComputeMotionVector`, the domain-keyed push
  blocks, and the per-domain fragment-input struct. The per-shader `MaterialParams` struct lives
  in the authoring shader (the cooker reflects each shader's own struct to pack its fields), not
  the engine header. The core pack itself cooks without `--shader-include` — its shaders reach the
  header through their own source dir.
- **Materials** (`*.vmat.json`) are validated against the fragment shader's reflected
  parameters — the declared, explicitly-typed field list must match — and the
  fragment outputs are validated against the material domain's contract (Surface →
  g-buffer MRT `SV_Target0`..`SV_Target3`; PostProcess → a single `SV_Target0`). A parent
  `*.vmat.json` may declare a top-level **`"defaultInstance"`** id: when present, the cook emits a
  companion **zero-override `MaterialInstance`** at that id beside the parent `Material` blob (the
  `CookDefaultInstanceBlob` helper synthesizes a `{ "parent": <id>, "overrides": {} }` document and
  runs it through the instance importer's shared cook, so the companion is byte-identical to a
  hand-authored zero-override `*.vmatinst.json`). Every direct material reference names that
  default-instance id, so a reference resolves a real `MaterialInstance` archive entry. The id lives
  in the material source, not the pack manifest, so the material editor mints and writes it through
  the same `.vmat` round-trip it already owns; a hand-authored material declares it directly.
- **Material instances** (`*.vmatinst.json`) cook a sparse parameter override over a parent
  `Material` through the `MaterialInstanceImporter`. The source declares `"parent"` (a parent
  `Material` `AssetId`) and a sparse `"overrides"` object (parent-field name → value for a param,
  or → an `AssetId` for a texture field). The importer resolves the parent, reflects its **exposed
  field set** — the parent's own declared `"fields"` list, cross-checked against the parent fragment
  shader's reflected `MaterialParams` for type and offset — and validates each override against it:
  the `.vmat`-against-shader check lifted one level to **instance-against-parent**. An override
  naming a field the parent does not expose (an engine-bound field never appears in the parent's
  declared list, nor does a sampler), or a type mismatch, is a **located cook error**; an omitted
  field inherits the parent default. It emits the `CookedMaterialInstance` blob (the override table
  + a value region of the param overrides' raw bytes); the instance owns no shader or pipeline — the
  parent supplies those. The importer is in the **core** set (it links only the Slang reflection +
  the graph-shader resolver hook, never libveng), so a bare-parent cook stays graph-aware.
- **Skinned meshes, skeletons, and animations** come from a rigged model (FBX, via the
  enabled assimp FBX importer). The `MeshImporter` emits the skinned vertex layout when the
  `*.mesh.json` names a `"skeleton"` id: it caps each vertex to four normalized influences
  (`aiProcess_LimitBoneWeights`), writes `RGBA16Uint` bone indices + `RGBA32Sfloat` weights,
  and stamps `SkeletonId`. The **`SkeletonImporter`** (`*.skeleton.json`) and
  **`AnimationImporter`** (`*.animation.json`, optional `"clip"` index) read the same model;
  all three derive bone indices from one **canonical bone order** (`SkeletonSource`, a DFS of
  the assimp node hierarchy), so a vertex's bone index, the skeleton's bone array, and an
  animation channel's target all agree. A skinned mesh keeps raw model units (bone bind /
  animation translations are not scaled) — scale a character via its entity `Transform`.
- **Textures cook mipped and block-compressed.** The `TextureImporter` generates a full
  **mip chain** offline — halving with `stbir_resize_uint8_srgb` / `_linear` (sRGB-correct for
  an sRGB source, linear otherwise) down to 1×1, setting `MipCount = floor(log2(max(W, H))) + 1`
  — then encodes **every** level to a GPU block format and packs the levels largest-first behind
  the `CookedTextureHeader`. Offline mips are mandatory for a block format (a compressed image
  cannot be GPU-blit-mipgen'd) and sRGB-correct; `generate_mips: false` opts back out to a single
  mip. The **codec defaults to ASTC 4×4 LDR** (`ASTC4x4Srgb` / `ASTC4x4Unorm` by the source's sRGB
  flag) — the Metal-blessed, broadly-supported codec on the primary MoltenVK platform; **BC7**
  (`BC7Srgb` / `BC7Unorm`) is selectable through a minimal internal codec seam for the anticipated
  Windows target. Both ride a documented encoder quality preset, since the smoke golden is
  codec-dependent. The header's `Format` integer (hand-synced to the `Renderer::Format` ordinals)
  is what the loader bridges back. **The codec is chosen by a build configuration's role table,
  not the manifest** — a texture declares a `role`; see [build configurations](#build-configurations--role--format-resolution) below.
- **Textures** take an optional `"max_size"` that downscales the decoded image (aspect-
  preserving, sRGB- or linear-correct) before packing, so high-resolution scan art does not
  bloat the blob.
- **Environments** (`*.env.json`) are equirectangular HDR panoramas: the `EnvironmentImporter`
  decodes an OpenEXR `"image"` with **tinyexr** (linked into the cooker, the one runtime-staged
  vendor lib the cooker also uses), optionally downscales by `"max_size"` (linear), and packs
  half-float `RGBA16Sfloat` texels behind a `CookedEnvironmentHeader`. The runtime generates the
  IBL cubemaps from the panorama on the GPU, so the cook stays decode-only.

## Build configurations — role → format resolution

The texture codec is a **per-platform** choice owned by a build configuration, not the
manifest. The reflected data model — `ProjectSettings` (the config list + the active one)
and `BuildConfiguration` (a `CompressionRole → CompressionFormat` table + target + zstd
level + output suffix) — lives in `libveng` (`Veng/Project/`, see
[engine/CLAUDE.md](../engine/CLAUDE.md)); the cooker **hand-parses** the `project.veng` /
`*.buildcfg` JSON authoring files into those structs (`ParseBuildConfiguration` in
`Cooker.cpp`), exactly as it hand-parses every other source — enums by name through the
shared `ToString`/`Parse` tables, never ordinal. The runtime carries no JSON parser.

- **`CookContext` gains `const BuildConfiguration* Config`** (beside `Types` / `Systems`).
  `vengc cook … --config <file>` parses one `*.buildcfg` and threads it through. With no
  `--config` the field is null.
- **The `TextureImporter` resolves `role → format` through it.** A `*.tex.json` declares a
  `role` (the intent — Color / Normal / Mask / HDR / UI); the importer reads the config's
  `RoleToFormat` table for that role and lowers the resulting `CompressionFormat` to the
  cook's encode-path codec. The resolution chain is **raw `"compression"` (the escape
  hatch) wins, else the config's role table, else the hardcoded ASTC zero-config default** —
  so a pack with no configuration cooks exactly as it did before configurations existed. The
  config's `CompressionLevel` drives the output archive's zstd.
- **The configuration file is one central depfile input.** The cooker records the
  `--config` file in the depfile centrally (like the pack JSON), not as a per-importer
  edge: a config edit re-cooks the whole pack anyway, so a fine-grained per-asset edge would
  buy nothing. Because **each configuration is its own output pack**, editing
  `windows.buildcfg` re-cooks only that config's pack — per-config invalidation falls out
  for free, with no shared mutable "active codec" to reason about.

## The prefab-cooking relaxation

The **prefab-cooking path** is the one place the Vulkan-free cooker relaxes its
separation: it links `veng::veng` and reuses `ModuleLoader` to `dlopen` a game module
and reflect its types — scoped to that load path (the graphics stack is linked but
never initialized). `vengc cook --module <lib>` reflects the module's component types
into a `TypeRegistry` (ABI-version check included), so the `PrefabImporter` validates a
prefab's components against the **real** reflected descriptors — an unknown component,
a wrong field type, or a malformed value is a located cook-time error. A field absent
from the source keeps its default-constructed value (schema tolerance): omission is
allowed, type-mismatch is not.

A **`FieldClass::Variant`** field is authored as `{ "type": <fully-qualified name>, "value":
{…fields…} }`; the importer matches `"type"` against each of the variant's alternatives by
its `TypeInfo.QualifiedName` (`TypeNameMatches`, strict — a leading `::` is tolerated but a
bare unqualified name is not) — (a name not among them is a located error), selects that
alternative, and recurses `BindField` into `"value"`, emitting the same `TypeId`
tag-plus-record bytes the engine reader expects. An absent or empty-`"type"` variant
stays empty.

This rests on the **GPU-free type-registration contract** (`RegisterBuiltinTypes`,
`Register<T>()`, a module's `VengModuleRegister` touch no `Context`/device): the
headless cooker reflects a module's types with no ICD present, and a no-device cooker
test pins the contract.

The **`LevelImporter`** cooks a `*.level.json` (a world prefab reference + the ordered
system set + the game-mode/render config) into the `CookedLevel` blob, beside the
`PrefabImporter` and on the same module-reflection relaxation. It requires the
`--module`-loaded `TypeRegistry` **and** `SystemRegistry` (absent → a "requires
`--module`" error), and validates the level against the **real** reflected/registered
surface: the world-prefab reference resolves, each `systems` id resolves against the
catalog, and the `gameMode`/`render` config validate against their reflected struct
descriptors — the same located-error discipline `PrefabImporter` applies to components. It
emits the two config records through libveng's `WriteFields`, so the cooker and the runtime
loader share one encoder.

## `vengc` subcommands

- **`cook`** — build a `.vengpack` from a single manifest (`--module <lib>` to reflect a
  game module's types **and systems** for prefab and level validation; `--config <file>`
  to select the build configuration whose role → format table the texture cook resolves
  through; `--shader-include <dir>` to add the engine core shader dir to every Slang session's
  search path so a consumer shader resolves `#include "Veng/material.slang"`). Engine-internal
  packs (the core pack, the editor icons) cook this way.
- **`cook-project`** — cook a whole **project** for one configuration: `vengc cook-project
  <project.veng> --config <name> --out-dir <dir> [--module <lib>] [--reference <pack>]...
  [--shader-include <dir>]`.
  `ParseProject` hand-parses the project's `packs`, `configurations`, and `startupLevel`;
  the named configuration is matched by `BuildConfiguration.Name`; each pack cooks into
  `<stem><suffix>.vengpack` and a `<projstem><suffix>.vengproj` (`WriteCookedProject`) names
  the packs' un-suffixed mount names + the startup level — the runtime entrypoint. One
  combined depfile covers every pack source + the project + the buildcfg. **A project's packs
  share one AssetId namespace:** each pack is cooked with the project's *other* packs (plus the
  CLI `--reference` packs) as references, so an asset in one pack may reference an asset declared
  in a sibling — resolution is by-id over source manifests, needing no cooked sibling and no
  build-order edge between packs. `veng_add_project` wires it.
- **`verify`** — re-hash a `.vengpack`'s blobs + TOC digest and exit nonzero on any
  mismatch.
- **`generate-id`** — mint a collision-free `AssetId` (prints hex for C++ literals and
  decimal for JSON packs; `--reference <pack.json>` to avoid existing ids). The same mint is a
  callable in-process API — `GenerateAssetId(span<const path> referencePackPaths)` (`Cooker.h`,
  over `ParseAssetPack` + the `AssetPack`-checking overload) — so the editor mints the
  `defaultInstance` id without shelling out to the CLI.
- **`generate-type-id`** — the `TypeId` analogue of `generate-id`.
- The tool can also emit a **type manifest**.

## Build wiring

`veng_add_asset_pack(... MODULE <lib>)` grows a `lib → cook → bundle` build-order edge so a
pack containing prefabs cooks after its game module is built; packs without prefabs
stay module-independent. `veng_add_game` wires the example's prefab pack to depend on
`libhello_triangle`.

`veng_add_project(... PROJECT <project.veng> OUTPUT_DIR <dir>)` (`cmake/Project.cmake`) is the
game-project entry: it reads `packs` + `configurations` from `project.veng` at configure
time and, **per configuration**, issues one `vengc cook-project --config <name>` command
producing that config's packs + `.vengproj` (suffix from each buildcfg's `outputSuffix`,
the single source of truth). Each `${target}-<configname>` is registered into
`cook-all-packs`; the host-default configuration's target is returned as
`${target}_HOST_TARGET` and carries the properties `veng_add_game` reads to copy the
project + packs beside the launcher and `veng_add_editor` reads to bake the editor's project
path. `cmake/BuildConfig.cmake` owns `VENG_BUILD_CONFIG` (host-triple-defaulted), the
`cook-all-packs` aggregate, and `veng_register_all_packs_target`. Both examples declare a
macOS / Windows / Linux configuration set and cook the host-default one by default;
`veng_add_asset_pack` remains for engine-internal single packs (the core pack, editor icons).
