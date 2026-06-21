# libveng_cook + vengc — the offline cook pipeline

`cooker/` is `libveng_cook` plus the `vengc` CLI: the offline toolchain that turns
hand-written JSON asset sources into the binary `.vengpack` archive the runtime
mounts. It is **never linked by the engine**. The on-disk archive format it emits is
documented in [assetpack/CLAUDE.md](../assetpack/CLAUDE.md); runtime loading of the
result (the `AssetManager`, `AssetHandle`, async/sync `Load`) and the full
shader/material model are in [engine/CLAUDE.md](../engine/CLAUDE.md). Project-wide
conventions live in the [root CLAUDE.md](../CLAUDE.md).

## Toolchain isolation

The cooker's heavy/toolchain deps — **stb, assimp, Slang** (shader compile +
reflection), and **nlohmann/json** — are **cooker-only**: gated behind
`VENG_BUILD_TOOLS` and never linked into `libveng` or its consumers, which load the
*binary* archive and never parse a source asset. The split is the whole point — the
runtime gains no importer, no source parser, no Slang, and no re-cook path.

The **content-hash function lives only here** (and in `vengc verify`): the cooker
writes each blob's xxh3-128 hash and the TOC digest into a `.vengpack` (format v2),
so `assetpack` stores the raw 16 bytes and computes nothing, and `libveng` gains no
hash dependency. The loader never verifies — hashing is tooling, not the hot path.

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
- **Materials** (`*.vmat.json`) are validated against the fragment shader's reflected
  parameters — the declared, explicitly-typed field list must match — and the
  fragment outputs are validated against the material domain's contract (Surface →
  g-buffer MRT `SV_Target0`+`SV_Target1`; PostProcess → a single `SV_Target0`).

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

A **`FieldClass::Variant`** field is authored as `{ "type": <registered name>, "value":
{…fields…} }`; the importer matches `"type"` against the registered `TypeInfo.Name` of
the variant's alternatives (a name not among them is a located error), selects that
alternative, and recurses `BindField` into `"value"`, emitting the same `TypeId`
tag-plus-record bytes the engine reader expects. An absent or empty-`"type"` variant
stays empty.

This rests on the **GPU-free type-registration contract** (`RegisterBuiltinTypes`,
`Register<T>()`, a module's `VengModuleRegister` touch no `Context`/device): the
headless cooker reflects a module's types with no ICD present, and a no-device cooker
test pins the contract.

## `vengc` subcommands

- **`cook`** — build a `.vengpack` from a manifest (`--module <lib>` to reflect a
  game module for prefab validation).
- **`verify`** — re-hash a `.vengpack`'s blobs + TOC digest and exit nonzero on any
  mismatch.
- **`generate-id`** — mint a collision-free `AssetId` (prints hex for C++ literals and
  decimal for JSON packs; `--reference <pack.json>` to avoid existing ids).
- **`generate-type-id`** — the `TypeId` analogue of `generate-id`.
- The tool can also emit a **type manifest**.

## Build wiring

`add_asset_pack(... MODULE <lib>)` grows a `lib → cook → bundle` build-order edge so a
pack containing prefabs cooks after its game module is built; packs without prefabs
stay module-independent. `veng_add_game` wires the example's prefab pack to depend on
`libhello_triangle`.
