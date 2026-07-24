# libveng_cook + vengc — the offline cook pipeline

`cooker/` is `libveng_cook` plus the `vengc` CLI: the offline toolchain that turns
hand-written JSON asset sources into the binary `.vengpack` archive the runtime
mounts. It is **never linked by the engine**. The on-disk archive format it emits is
documented in [assetpack/CLAUDE.md](../assetpack/CLAUDE.md); runtime loading of the
result (the `AssetManager`, `AssetHandle`, async/sync `Load`) and the full
shader/material model are in [engine/src/Asset/CLAUDE.md](../engine/src/Asset/CLAUDE.md).
Project-wide conventions live in the [root CLAUDE.md](../CLAUDE.md).

`libveng_cook` links **`veng::graph`** PUBLIC — the shared node-graph + material-codegen
library (see [graph/CLAUDE.md](../graph/CLAUDE.md)). A material graph is walked into Slang
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
*is* the tools, so there is no build toggle to skip them. A library-only consumer
instead uses `find_package(veng)` and
gets `vengc` as a **prebuilt imported executable** in `vengTargets`; `veng-config`
recreates the unqualified `vengc` name so `$<TARGET_FILE:vengc>` resolves in every
consumption mode. The installed `vengc` carries an `INSTALL_RPATH` and **requires the
host's Vulkan SDK / Slang present to run** — Slang is not vendored. A downstream cook
resolves `--reference` / `--shader-include` against the installed core-data source
under `share/veng/core/` (`veng_CORE_PACK_JSON` / `veng_CORE_SHADER_DIR`).

The **content-hash function lives only here** (and in `vengc verify`): the cooker
writes each blob's xxh3-128 hash and the TOC digest into a `.vengpack` (format v6),
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

- **Shaders** are authored in **Slang** — Slang only, there is no GLSL path. The cooker
  **always** compiles from source (there is no precompiled-inline path) and **reflects
  the shader offline** into a serializable `ShaderInterface`; the engine then loads
  plain SPIR-V.
- **A fragment shader's source can be a graph, not a `.slang` file.** A `*.shader.json` whose
  `"source"` ends in `.graph.json` (plus a `"domain"`: `"Surface"` | `"PostProcess"` | `"Sky"` |
  `"Translucent"` | `"GuiFill"`, `MaterialDomain`'s exact enumerator spellings) is cooked by
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
  directory). A consumer (or generated) `.slang` therefore `#include`s its domain's engine material
  contract directly — `#include "Veng/surface.slang"` (or `Veng/postprocess.slang` / `Veng/sky.slang` /
  `Veng/guifill.slang`)
  — the per-domain engine headers under `engine/assets/core/shaders/Veng/`, each holding the bindless
  declarations, `g_ViewConstants`, and its domain's push block + fragment-input struct (`surface.slang`
  additionally `DrawData`, `GBufferOutput`, `ComputeMotionVector`). The per-shader `MaterialParams`
  struct lives in the authoring shader (the cooker reflects each shader's own struct to pack its fields), not
  the engine header. The core pack itself cooks without `--shader-include` — its shaders reach the
  header through their own source dir.
- **Materials** (`*.vmat.json`) are validated against the fragment shader's reflected
  parameters — the declared, explicitly-typed field list must match — and the
  fragment outputs are validated against the material domain's contract (Surface →
  the five-target g-buffer MRT `SV_Target0`..`SV_Target4` — albedo/normal/ORM, velocity, and
  emissive; PostProcess, Sky, Translucent, and GuiFill → a single `SV_Target0`). Because the
  Surface contract's output set is part of what a cooked material *means*, a change to it bumps
  `CookedMaterialVersion`
  (`assetpack`'s `CookedBlobs.h`), so a stale blob cooked against an older output set rejects
  loudly at load rather than binding a pipeline whose color-target list no longer matches. A parent
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
- **UI documents and stylesheets** (`*.vui.xml` / `*.vuss`) cook through the
  **`UIDocumentImporter`** and **`StyleSheetImporter`** — pugixml over the markup and a CSS
  tokenizer over the sheet, both cooker-only, so no XML or CSS parser reaches the runtime. The
  markup becomes a pre-order recipe element tree (kind, id, classes, text, inline style, unresolved
  `{obj.field}` bindings, named handlers) and the sheet becomes flattened, selector-resolved rules
  plus their pseudo-state variants, with colors resolved sRGB→linear and each `background-gradient`
  baked to a shape + a ramp LUT. Both importers share `Importers/StyleParse.{h,cpp}`, which owns the
  per-property value grammar and the **fill-source exclusivity diagnostic**
  (`CheckExclusiveFillSources`): `background-material`, `background-gradient`, `background-image`,
  and `background` are one exclusive fill source, so a block authoring two is a **located cook
  error** rather than a silently-ignored declaration. An asset-valued declaration (`font`,
  `background-image`, `background-material`, an `<Image src>`) cooks its `AssetId` into the
  declaration's `Handle` slot; **residency is the runtime loaders' job**, not the cook's — each
  blob's loader scans its decoded declarations for handle-valued properties and eager-loads them, so
  a rule-authored and an inline-authored reference both reach the document resident. A `@use`d sheet
  is the one *file* dependency these importers record, so editing a theme re-cooks every sheet that
  imports it. The runtime half is [engine/src/Gui/CLAUDE.md](../engine/src/Gui/CLAUDE.md).
- **Input maps** (`*.inputmap.json`) cook an `InputMappingContext` (`AssetTypes::InputMap`)
  through the **`InputMapImporter`**. The source declares its `"actions"` (each an unsigned
  `id`, a `name`, and an enum `kind`) and its `"bindings"` (a raw `source` device/control, a
  target `action` id, an `axis` component, and a `scale`); the importer decodes them into an
  `InputMapData` and emits it through the shared `WriteFields` encoder — the reflected
  actions + bindings, no bespoke binary format. Its core check is **binding → action
  validation**: a binding must name an action the context's own `"actions"` declares (the
  typo-catch a global registry would otherwise miss), and a duplicate action id, a null id,
  or an unknown device/axis/kind name is a **located cook error** — the same discipline the
  `MaterialImporter` applies to `.vmat` fields and the `PrefabImporter` to components. It
  references only engine builtins (`InputAction`/`Binding` and their enums), so it needs no
  game module.
- **Tables** cook as a pair. A `*.tableschema.json` (`AssetTypes::TableSchema`) declares
  `"columns"` — each a `name` and a `"type"` naming a **registered reflected type by its
  fully-qualified name** (`"Veng::i64"`, `"Veng::vec4"`, `"Veng::AssetHandle<Texture>"`,
  `"MyGame::Cadence"`), the same spelling a variant alternative's `"type"` tag matches against —
  plus the `"key"` column, whose type must have a total order and a stable cooked encoding (the
  integer scalars, or string). The **`TableSchemaImporter`** resolves each column's type name and
  then hands the set to libveng's **`LayOutTableSchema`**, which owns the validation *and* the
  layout pass: cells are packed in declaration order at their encoded widths, and a column keeps a
  constant offset only while every preceding column is fixed-size. That function is engine-tier
  precisely so the editor's schema panel enforces the same rules without linking the cooker. A `*.table.json` (`AssetTypes::DataTable`) names its
  `"schema"` by hex id and lists its `"rows"`; the **`DataTableImporter`** resolves that schema
  through `CookContext::Resolve`, re-parses it, and binds every cell with `JsonReadFieldValue`
  then encodes it with `WriteFieldValue` — the shared walkers, not a table-specific codec — so a
  cell authors exactly as the same type authors as a struct field, and a malformed one is located
  down to its inner field. An unknown column, a missing column, a malformed value, a duplicate
  key, an unresolvable asset reference and one whose target is the wrong asset type are all
  **located cook errors**. Every asset-handle cell resolves through `Resolve` too, which is what
  puts the schema and each referenced asset into the cooked dependency graph; the runtime never
  loads what a cell names.
  Because a column is a reflected type, **a table cook requires `CookContext::Types`**: with no
  registry threaded in (no `--module`) both importers fail loudly rather than guess a layout.
  The blob is a sorted, unique key index, then a `u32` row directory (omitted when every column is
  fixed-size), then the row region; the importer fails the cook if that region would cross the
  4 GiB a `u32` offset can address. Both importers live in `libveng_cook`, which is legal because
  the embedded core pack carries no tables.
- **A model's named attachment points survive the cook as mesh sockets.** The `MeshImporter`
  walks the imported node hierarchy and records every node that **carries no mesh and no camera,
  is not referenced by a skin's joints, and is not the scene/export root** as a
  `CookedMeshSocket` — name plus transform. That is the whole rule: an empty node in a model is
  there to mark a place, so it needs no naming convention, no prefix, and no per-project
  configuration (a project's own `Foo_` convention is then a project matter, unblocked rather than
  imposed). **The joint exclusion is load-bearing, not a refinement** — a skeleton joint is
  *exactly* a node with no mesh and no camera, so without it an entire rig becomes sockets, bloating
  the blob and making a lookup by a joint's name return a static mesh-space point that silently
  disagrees with the animated joint. A socket's transform is its **full ancestor chain composed
  down to the root**, then translated by `import.scale` — the same treatment the vertices get, since
  the importer flattens `scene->mMeshes` without applying node transforms, so a socket read from its
  own node alone would land in a different space than the geometry. Two sockets sharing a name (or
  sharing a name after truncation to the cooked 64-byte capacity) is a located cook error: a lookup
  by name has to resolve to one place. The table is emitted sorted by name.
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
- **Collision shapes** (`*.collision.json`) turn an authored model into collision geometry. The
  source names a `"model"` (relative to the source JSON, as a `*.mesh.json` does — there is no
  `"part"` concept at cook time) and a `"mode"`: `"convex"` reduces the model to its **convex
  hull's vertices**, `"mesh"` welds its triangles by position into an indexed triangle soup. An
  optional `"import": { "scale": … }` matches the mesh importer's, so a collision shape and the
  render mesh cooked from one model stay the same size. Welding is the point of both modes — a
  render mesh splits a vertex per normal and per UV seam, which a solver has no use for. The hull
  is the **cooker's own** implementation (`Importers/ConvexHull.{h,cpp}`), which is why
  `veng_cook_objs` / `libveng_cook` **link no physics library**: the cook emits neutral geometry
  and the runtime builds the solver's shape from it, so the cooker's dependency set is unchanged
  and there is no silent-ABI hazard from a physics library's compile-time defines having to match
  across two targets. A hull exceeding `MaxConvexHullPoints` (2048) is a located cook error rather
  than a silently expensive asset. The blob layout is in
  [assetpack/CLAUDE.md](../assetpack/CLAUDE.md); the runtime half in
  [engine/src/Physics/CLAUDE.md](../engine/src/Physics/CLAUDE.md).
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

## The cook cache

An incremental re-cook skips the expensive importer + compression work for any asset whose inputs
are unchanged, copying its **final stored (compressed-or-raw) bytes** straight into the new archive
instead of re-encoding. The store is `CookCache` (`Cook/CookCache.{h,cpp}`, in the veng-free
`veng_cook_objs` core), enabled by `vengc cook … --cache-dir <dir>` (and `cook-project`); with no
`--cache-dir` the field is null and the cook runs exactly as before. `veng_add_asset_pack` /
`veng_add_project` pass `${CMAKE_BINARY_DIR}/vengc-cache`, so the cache lives **inside the build
tree** — `build-debug/` and `build/` never share one.

- **The cache is a pure optimization, never consulted for correctness.** The runtime never reads it
  and `vengc verify` ignores it; a miss (or any validation failure) simply re-cooks. A cache hit
  produces a **byte-identical** archive to a fresh cook — `MakeStoredBlob` is the one place a blob's
  stored form is chosen, so the compressed bytes agree whether freshly encoded or replayed.
- **It is ccache-style: a direct key selects a per-entry manifest, which is then validated.** The
  key folds the tool tag (the cache-format version plus a path/size/mtime fingerprint of **every
  image the cook runs code from** — the `vengc` executable, the `--module` runtime module whose
  reflected field layouts the prefab/level/table encoders walk, and the cook module supplying a
  game type's importer — so rebuilding any of them invalidates everything), the manifest entry
  JSON, the pack directory, the
  active configuration's fingerprint, and the shader-include dir. A hit is trusted only after every
  recorded **source dependency** is confirmed unchanged **and** every recorded cross-asset
  **resolution** (`AssetId → source path`) still maps identically — the id→source-remap check a
  content hash alone cannot express. The dependency set is exactly what the depfile records (the
  entry's own source, importer-recorded payloads/includes, resolved reference sources), so the cache
  is as complete as the depfile, and a hit re-records those paths so the **depfile stays complete**
  even though the importer never ran.
- **Validation is stat-fast-path, content-hash-authoritative.** Each dependency stores its size,
  mtime, and xxh3-128. On a lookup the file is `stat`'d first: an unchanged size+mtime is trusted as
  unchanged and the file is **not read** — this is what makes an all-hit re-cook cheap (reading the
  sources back to hash them dominates otherwise, and the largest assets are the models/catalogs). A
  differing stat (a `touch`, a branch switch) falls back to re-hashing the contents; a still-matching
  hash is a hit, so a mtime change without a content change never forces a re-cook. The one case the
  fast path trusts is content that changes while size **and** mtime both stay identical — the same
  assumption the build's own depfile makes when it decides whether to run the cook at all, so it adds
  no trust the build doesn't already place in mtime. Within one cook, content hashes are memoized by
  path, so a file many entries share (an engine shader header, a model several meshes extract from)
  is hashed at most once.
- **Storage is two-level and content-addressed** under `<dir>/`: `entries/<key>.json` holds a cook's
  dependency + resolution manifest and its emitted blob **descriptors** (id/type/codec/size/hash, no
  bytes); `blobs/<hash>.blob` holds each blob's stored bytes, addressed by their content hash so
  identical outputs across entries or configurations share one file. Every write is atomic (a killed
  cook strands no torn cache file).
- **An unchanged pack is recognized from metadata alone and its write is skipped.** Because
  `cook-project` cooks every pack in one invocation, a change to *one* pack re-runs the whole command
  and would otherwise rewrite every pack. To avoid that, a hit loads only the entry **metadata** (no
  blob bytes); the cook lays out the archive TOC from those descriptors (`BuildArchiveToc`) and hashes
  it into the same digest a full build would produce, then compares that digest + total size against
  the existing pack file's header (`ReadArchiveIdentity`, a 32-byte read). If every entry hit and the
  identity matches, the pack on disk is already byte-for-byte what would be written — so **no blob is
  read and nothing is written**. Only when an entry actually changed (or the file is absent/different)
  are the hit blobs read back from the cache and the pack rewritten. This is why the digest lives in
  the TOC (over each blob's content hash): the whole pack's identity is checkable without its bytes.
- **Different build configs never collide, by key not by directory.** Because the configuration's
  fingerprint (its role → format table, zstd level, name/target) is folded into the key, one cache
  dir holds a pack's macOS and Windows (or any two configs') blobs under distinct keys — a shared
  cache dir hands each cook only its own bytes. One entry can emit **several** blobs (a parent
  `Material` and its default `MaterialInstance`); all of them are stored and replayed together under
  the single entry key.

## The cook is parallel — and an importer declares whether it may be

The driver runs a pack's entries across **one bounded work pool**, not a serial loop. A
content-heavy cold cook was measured at 1.20 of 6 available cores before this existed; the
same cook now runs at ~3.8 effective cores. What decides whether two entries overlap is the
importer's own **concurrency declaration**, and the default is not to overlap.

### The concurrency budget: `--jobs <n>`

**One number for the whole cook**, shared by the per-asset pool and by any threading an importer
does inside its own `Cook`. `cook` and `cook-project` take `--jobs <n>`; the default (`0`) is
`std::thread::hardware_concurrency()`.

- **`--jobs 1` means one thread in the whole process** — a strictly sequential asset loop *and*
  a texture encode on the calling thread. The texture importer used to spawn
  `hardware_concurrency()` workers per mip level regardless of how the driver ran, so a serial
  cook was never actually single-threaded. It is now.
- **An importer that threads internally reads `CookContext::ThreadBudget`** (never below 1) and
  must not exceed it. Under the pool it is 1: the outer loop already supplies the parallelism, so
  the correct resolution of the nesting is that the importer encodes on its calling thread rather
  than opening a second pool on a machine the first already saturates. Stacking the two
  oversubscribes badly on a fanless machine that throttles under sustained all-core load.
- **Results are written into per-asset slots, never appended in completion order.** This is a
  cook-cache requirement, not a determinism one: the cache recognises an unchanged pack by laying
  out the archive TOC from cached descriptors and hashing it, so a completion-ordered TOC would
  make every warm cook rewrite packs it did not need to. Slots also make a reported error the
  first failing entry rather than whichever worker lost the race.
- **The cook cache needs no concurrency of its own.** Cache lookup runs on the driver thread
  before the pool starts and cache stores after it joins, so the parallel region contains no
  cache access at all.
- **Diagnostics from concurrent importers interleave**, and their order varies run to run. Each
  located error still names its file and field; only the ordering is scheduling-dependent.

### The importer thread-safety contract

A consumer's cook module supplies importers veng never sees (see
[Cook modules](#cook-modules--a-games-own-importers)), so this is a **consumer-facing contract**,
not an internal note.

**What the driver guarantees to `AssetImporter::Cook(context, entry)`:**

- It is called **at most once per manifest entry**, on a worker thread — not necessarily the
  thread that constructed the importer.
- `context` and `entry` are that call's own. `context.Resolve` and `context.RecordDependency`
  record into that entry's private slot and are safe to call from the worker.
- `context.AssetTypes`, `Types`, `Systems` and `Config` are **read-only for the whole cook**.
- `context.ThreadBudget` (≥ 1) is how many threads this call may spawn. It is **one shared budget
  for the whole cook**, not a per-importer allowance.
- The cook cache is never touched while `Cook` runs.

**To declare `ImporterConcurrency::Parallel`, `Cook` must not:**

- Write state that outlives the call — no file-scope or function-local `static` mutable data, no
  importer member, no global.
- Call a library carrying **process-wide mutable state** reachable by another concurrent `Cook`
  unless that state is itself ordered. **Lazily-initialised global tables are the trap**, and
  *"it only initialises once"* is a claim to verify against the library's source, not to assume —
  both real defects the audit found were exactly this shape.
- Write outside its own returned buffer.
- Assume any ordering relative to other assets, or any interleaving order for its diagnostics.

**Opting out is doing nothing.** `AssetImporter::Concurrency()` defaults to
`ImporterConcurrency::Serialized`, which runs the call under the cook's single serialization lock.
Opting *in* is the explicit override, so an unaudited importer is safe by construction and an
importer that cannot be *shown* safe is never assumed safe — a cook that is fast and occasionally
wrong is far worse than one that is slow.

**Nothing in this contract asks an importer to be deterministic.** A cooked asset is required to
be **valid**, not to be one particular valid encoding, and two runs may legitimately produce
different bytes (the ASTC encoder is built without `ASTCENC_INVARIANCE`). Reproducibility is not
a property the cook promises and not one an importer owes.

### What the audit found

Every first-party importer landed in one of two classes; **nothing landed in "unknown but let
through"**.

| importer | class | why |
|---|---|---|
| Texture | **Parallel** | `stb_image`'s failure reason is `thread_local`; `stb_image_resize2`'s tables are read-only; the block encoders' process-global tables are now ordered (below) |
| Raw | **Parallel** | a file read into a fresh buffer, driving no library |
| Mesh, Skeleton, Animation, CollisionShape | Serialized | assimp — per-call `Importer` instances are the documented pattern, but the `DefaultLogger` singleton and per-format loader state were not established |
| Shader, Material, MaterialInstance | Serialized | Slang — `createGlobalSession` per call, reentrancy not established |
| Font | Serialized | FreeType / msdfgen |
| Environment | Serialized | tinyexr |
| Prefab, Level, TableSchema, DataTable | Serialized | the module-reflected `TypeRegistry` / `SystemRegistry`; read-only during a cook, but confirming that was more work than serialising them |
| StyleSheet, UIDocument, InputMap, VertexLayout | Serialized | free — 0.02 s combined across 23 assets on the measured pack |

**Two real defects came out of it, both process-global encoder state:**

- **`astcenc` rebuilds its global `sin_table`/`cos_table` at the end of *every*
  `astcenc_context_alloc`, not once.** Allocating a context while another thread encodes therefore
  races with that thread's reads. The encoder context is now **cached per thread** and reused
  across mips and textures (`astcenc_compress_reset`), with a `std::shared_mutex` ordering
  allocation (exclusive) against encoding (shared).
- **`bc7enc_compress_block_init()` and `rgbcx::init()` fill process-global tables on every call**,
  now behind `std::call_once`. Found by inspection rather than by the sanitiser — the macOS ASTC
  path does not reach them.

**The serialized band, measured on a content-heavy pack:** 26.66 s of true work (every importer
but Texture), of which Mesh alone is 15.10 s, against 235.57 s of worker-seconds in the Parallel
band — **18.5 % of the pre-change cook**. That is the ceiling on any further gain from
parallelising more importers, and at the current wall time it is *not* the binding constraint:
a 26.7 s serial floor against a ~60 s wall means throughput binds, not serialisation.

### What is checked, and what was deliberately not

- **The warm-cook no-op test** (`tests/cooker/cook_cache.cpp`) cooks a 24-entry pack at 8 jobs,
  cooks it again, and asserts the second cook **rewrote nothing**. That is the property the TOC's
  stability exists to serve — an unchanged pack recognised from metadata and skipped — tested
  directly rather than through a proxy, and it is indifferent to the encoder's byte-level
  variability.
- **A byte comparison between a serial and a parallel cook was rejected, not merely unavailable.**
  Recorded so it is not re-proposed: it catches a race only when the race happens to perturb
  output on that run, which is strictly weaker than a thread-sanitiser run, while `vengc verify`
  already covers output integrity and the suite covers semantics. It would also have been
  meaningless here — without `ASTCENC_INVARIANCE` a texture-bearing pack does not cook to the same
  bytes twice even from an unchanged binary, so the comparison would have measured encoder jitter
  rather than correctness.
- **A thread-sanitiser run over a cook** exercising every reachable first-party importer is the
  standard this is held to, and it is clean. **Skeleton and Animation are not exercised** — the
  repo carries no rigged-model fixture — so what establishes those two is the serialization lock,
  not the sanitiser.

### Cook-module ABI 1 → 2

`AssetImporter` gained a virtual (`Concurrency()`) and `CookContext` a trailing field
(`ThreadBudget`), both of which a module built against ABI 1 would misread — a stale vtable and a
short context. `VENG_COOK_MODULE_ABI_VERSION` is therefore **2**, so the handshake rejects a stale
cook module loudly instead. A consumer **rebuilds; no source change is required.**

### Reading a `--timing` report

`--timing[=<file>]` on `cook` and `cook-project` (see [`vengc` subcommands](#vengc-subcommands))
prints an operator summary and optionally writes the full per-asset table as CSV. With no flag
nothing is sampled and the cook is behaviour-for-behaviour what it is without it.

- **Per-asset and per-importer figures are *work*, never elapsed.** A `Serialized` importer's
  elapsed includes the time it spent queued on the serialization lock; billing that to the
  importer inflates a cheap one by however long the expensive ones held the lock and makes the
  shares sum past the cook. Queueing is reported in the roll-up's own **`queued`** column and in
  the CSV's `serialized_wait_s`, and is excluded from every total, mean and ranking.
- **Above one job the asset seconds are a sum across workers**, not a share of the wall clock, so
  `total − assets` is not a remainder and is not offered as one. The report says so, states the
  budget, and gives a `workers busy` occupancy figure instead. Per-importer shares are taken
  against that worker sum, which is what makes them add to 100 %.
- **At one job the accounting closes**: the report gives the total, the per-asset sum, the named
  driver phases (module load, project/manifest parse, TOC + digest, archive write, cache
  bookkeeping, depfile) and an explicit **unattributed** line, so a serial share is stated rather
  than left as a subtraction the reader has to compute.

## The prefab-cooking relaxation

The **prefab-cooking path** is the one place the Vulkan-free cooker relaxes its
separation: it links `veng::veng` and reuses `ModuleLoader` to `dlopen` a game module
and reflect its types — scoped to that load path (the graphics stack is linked but
never initialized). `vengc cook --module <lib>` reflects the module's component types
into a `TypeRegistry` (ABI-version check included), so the **`PrefabImporter`** binds
each component through the shared `JsonReadFields` walker
(`Veng/Reflection/JsonSerialize.h`) against the **real** reflected descriptors — an
unknown component, a wrong field type, or a malformed value is a located cook-time
error naming the walker's dotted field path, prefixed with the importer's own
file/entity/component context. A field absent from the source keeps its
default-constructed value (schema tolerance): omission is allowed, type-mismatch is
not. What is genuinely the importer's own is supplied as `JsonFieldHooks`:
`ValidateAssetId` runs the pack-resolve type check
(`AssetTypeRegistry::FindByHandleField` to turn the field's reflected leaf `TypeId` into
the asset type it references, then `AssetHandleFieldAccepts` against the resolver,
reporting the expected asset type **by name**). A leaf no registered asset type claims is
itself a located error — the type's registration failed to set `HandleFieldType`, and
skipping the check would let the field accept an id of any type. `ReadReference` maps a JSON value to the
prefab-local entity index. Everything else — the entity/component table walk, `TypeId`
resolution against the registry, and the `WriteFields` blob emission — stays the
importer's own.

A **`FieldClass::Variant`** field is authored as `{ "type": <fully-qualified name>, "value":
{…fields…} }`; the walker matches `"type"` against each of the variant's alternatives by
its `TypeInfo.QualifiedName` (`TypeNameMatches`, strict — a leading `::` is tolerated but a
bare unqualified name is not) — a name not among them is a located error — selects that
alternative, and recurses into `"value"`, emitting the same `TypeId` tag-plus-record bytes
the engine reader expects. An absent or empty-`"type"` variant stays empty.

This rests on the **GPU-free type-registration contract** (`RegisterBuiltinTypes`,
`Register<T>()`, a module's `VengModuleRegister` touch no `Context`/device): the
headless cooker reflects a module's types with no ICD present, and a no-device cooker
test pins the contract.

The **`LevelImporter`** cooks a `*.level.json` (a world prefab reference + the ordered
system set + the game-mode/render config) into the `CookedLevel` blob, beside the
`PrefabImporter` and on the same module-reflection relaxation, and binds its config
records (`GameModeConfig`, `LevelRenderSettings`, the session seed) through the
**same** `JsonReadFields` walker — no hooks of its own (a `Reference` field in level
config is a located error, a deliberate posture rather than an unsupported-field-class
gap). It requires the `--module`-loaded `TypeRegistry` **and** `SystemRegistry` (absent
→ a "requires `--module`" error), and validates the level against the **real**
reflected/registered surface: the world-prefab reference resolves, each `systems` id
resolves against the catalog, and the `gameMode`/`render` config validate against
their reflected struct descriptors — the same located-error discipline `PrefabImporter`
applies to components. It emits the two config records through libveng's `WriteFields`,
so the cooker and the runtime loader share one encoder.

## `vengc` subcommands

- **`cook`** — build a `.vengpack` from a single manifest (`--module <lib>` to reflect a
  game module's types **and systems** for prefab and level validation, which also loads the
  sibling cook module for the game's own importers — see [Cook
  modules](#cook-modules--a-games-own-importers); `--cook-module <lib>` to name that module
  explicitly instead, which *implies* `--module` — a cook module links its runtime module, so
  the runtime module beside it is loaded too and the importers' asset-type names resolve;
  `--config <file>`
  to select the build configuration whose role → format table the texture cook resolves
  through; `--shader-include <dir>` to add the engine core shader dir to every Slang session's
  search path so a consumer shader resolves `#include "Veng/surface.slang"`; `--cache-dir <dir>` to
  serve unchanged assets from the cook cache instead of re-encoding them, see [The cook
  cache](#the-cook-cache); `--jobs <n>` to bound the cook's whole concurrency budget and
  `--timing[=<out.csv>]` to emit the per-asset / per-importer report, see [The cook is
  parallel](#the-cook-is-parallel--and-an-importer-declares-whether-it-may-be)). Engine-internal
  packs (the core pack, the editor icons) cook this way.
- **`cook-project`** — cook a whole **project** for one configuration: `vengc cook-project
  <project.veng> --config <name> --out-dir <dir> [--module <lib>] [--cook-module <lib>]
  [--reference <pack>]... [--shader-include <dir>] [--cache-dir <dir>] [--jobs <n>]
  [--timing[=<out.csv>]]`.
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
  mismatch. `--module <lib>` is presentation only: the verdict is a byte re-hash that consults
  no registry, but without the module a game-defined type prints as a raw hex id instead of its
  registered name. `VerifyArchive` itself stays registry-free; only the CLI presenter takes it.
- **`generate-id`** — mint a collision-free `AssetId` (prints the zero-padded hex in both
  spellings: `0x{:016X}ULL` for C++ literals and `"0x{:016X}"` for JSON packs;
  `--reference <pack.json>` to avoid existing ids; `--module <lib>` when a reference pack names a
  game-defined asset type, whose name resolves only against that module's registrations). The same mint is a
  callable in-process API — `GenerateAssetId(span<const path> referencePackPaths)` (`Cooker.h`,
  over `ParseAssetPack` + the `AssetPack`-checking overload) — so the editor mints the
  `defaultInstance` id without shelling out to the CLI.
- **`generate-type-id`** — the `TypeId` analogue of `generate-id`.
- **`generate-asset-type`** — mints a collision-free `AssetTypeId` (the same two spellings),
  checked against the engine builtins and, with `--module <lib>`, a module's own registrations.
  There is deliberately **no `--reference`**: a pack manifest carries type *names*, not minted
  type ids, so a pack has nothing to check against. `GenerateAssetTypeId(const AssetTypeRegistry&)`
  (`Cook/AssetPack.h`) is the in-process form.
- **`generate-family-id`** — mints a `StoreFamilyId` for a durable-store family
  (`Veng/Persistence/Store.h`), in the same two spellings. It takes **no options**: a family id
  lives in a consumer's own source, in no registry this tool can load, so there is nothing to
  collision-check against — the id is 64 bits of randomness and a collision is fatal at
  registration.
- The tool can also emit a **type manifest**.

## Cook modules — a game's own importers

A game defines whole asset types of its own, and the offline half arrives through a second
dlopened image: **`lib<game>_cook`**, emitted by `veng_add_game(... COOK_SOURCES ...)`. Both
`cook` and `cook-project` load it beside `--module`'s argument (`SiblingCookModulePath`: same
directory, same extension, stem suffixed `_cook`); `--cook-module <path>` replaces that lookup
entirely. An absent sibling simply means the game defines no importers; an explicit path that
fails to load, or any image that fails the handshake, is fatal.

**`--cook-module` alone is a complete cook.** The importers are keyed on asset-type ids whose
*names* live in the runtime module, so a cook module with no runtime module beside it would
register importers for types the manifest cannot name. `SiblingRuntimeModulePath` inverts the
convention and the runtime module is loaded explicitly — not resolved through the cook module's
own handle, because `dlsym` searches an image's dependents and `GetProcAddress` does not.
`--module` alone stays valid (a game with reflected components and no custom importers), and
passing both is an explicit override of the sibling lookup.

- **It must not link `libveng_cook`.** That static library carries the cooker's machinery and its
  process-wide state — the Slang session, the graph-shader resolver hook — and a second copy of
  both would ride into the dlopened image beside the tool's own. The cook module links
  **`veng::cook_interface`** instead: an INTERFACE target carrying the importer contract as
  headers only (`Cook/Importer.h`, `Cook/CookModule.h`, nlohmann-json, `veng::assetpack`,
  `JSON_NOEXCEPTION`). `libveng_cook` consumes the same headers, so there is one contract and no
  duplicated machinery. `AssetImporterRegistry::Register` is inline for exactly this reason — an
  out-of-line definition would be an unresolved symbol in the module.
- **Its own ABI, versioned independently.** `VengCookModuleRegister(VengCookModuleHost*)` with a
  `VengCookModuleAbiVersion` handshake (`VENG_COOK_MODULE_ABI_VERSION`, currently **2** — see
  [the ABI bump](#cook-module-abi-1--2)), so a change
  to the importer surface never invalidates every runtime module. `ModuleLoader::Load` is
  parameterized on the version symbol and expected value, so both contracts share one platform
  loader.
- **It registers importers only.** The asset type's *identity* — id, manifest name, display
  metadata — registers through the runtime module's `VengModuleRegister`, which every host
  reaches. Registering from both seams would deliver the same id twice in the editor, where both
  images load, and duplicate ids abort. The cooker merges the runtime module's asset types into
  its own registry (`MergeAssetTypes`) so a manifest entry naming a game type resolves.
- **Lifetime.** `LoadedCookModule` declares its `LoadedModule` handle before the importers it
  collected, so the handle destructs last; anything the importers move into (a `Cooker`) must
  likewise be declared after the handle. The **bootstrap cooker never loads cook modules** — the
  veng-free edge is untouched.
- The editor loads the same sibling per cook request (`CookSession`), so a game-typed source
  recooks in-editor and hot-reloads.

## Build wiring

`veng_add_asset_pack(... MODULE <lib>)` grows a `lib → cook → bundle` build-order edge so a
pack containing prefabs cooks after its game module is built; packs without prefabs
stay module-independent. `veng_add_game` wires the example's prefab pack to depend on
`libhello_triangle`. An optional `COOK_MODULE <lib>` adds the same ordering edge for the sibling
cook module (`veng_add_project` takes the same keyword): the cooker finds it by path and needs no
flag, but without the edge a cook can run before the cook module exists and fail as
`no importer registered for type 'X'` — the wrong cause, varying by build order.

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
