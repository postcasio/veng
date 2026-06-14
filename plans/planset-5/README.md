# planset-5 — the synchronous asset system (cooker, packs, loader)

**Phase goal:** give veng a real asset pipeline, end to end and **synchronous**:
hand-write a JSON **asset pack** that names assets by id and points at sources
(or a JSON material that points at a Slang shader, inline or external), **cook**
it into a single binary **archive** with a standalone tool, and **load** assets
in the engine by `AssetId`. By the end, the hello-triangle sample renders a mesh +
material + texture entirely out of a cooked pack.

This is **future [area 1](../future/README.md) — the asset system**, taken up as
its own planset, scoped to the **synchronous slice**. The broader design vision
(async loading, the bindless material end-state) lives in
[asset-system.md](../future/asset-system.md); this planset delivers the
foundation that vision is built on, without the parts that depend on threading or
bindless.

## The four decisions that shape this planset

Resolved up front (they change the structure of every plan):

1. **Cooking is not part of the engine.** Importing/cooking lives in a **separate
   in-repo library + CLI** (`cooker/` → `libveng_cook` + the `vengc` tool). The
   engine (`libveng`) **never** links stb / assimp / Slang — it only *loads*
   cooked archives. A small shared library, `assetformat/` → `libveng_assetformat`,
   defines the archive container + cooked-blob layouts and is linked by **both**
   sides as the one source of format truth.

2. **No cooking on demand.** Cooking is an explicit offline step (`vengc cook …`,
   wired into the example's build). The runtime has **no importer code, no source
   parsers, no re-cook path** — if an `AssetId` isn't in a mounted archive, that's
   a `NotFound`, full stop. (This is the deliberate departure from the original
   sketch's `CookOnDemand`.)

3. **Loading is by `AssetId`, through asset packs.** An **asset pack** is a
   registry of `AssetId (u64) → asset` mappings. `AssetId` is an **opaque `u64`** —
   typically a random number minted by the tooling, but freely hand-assignable in a
   hand-written pack. No content-hashing, no name-hashing; ids are just numbers the
   pack author owns. The cooker turns a JSON pack into a binary **archive**
   (`.vengpack`); the runtime mounts archives and resolves `AssetId`s against them.

4. **Materials author shaders in Slang.** The shader toolchain for materials is
   **Slang** (`slangc` in the cooker), compiled to SPIR-V and **reflected offline**
   into a serializable `ShaderInterface`. The engine still loads plain SPIR-V — it
   gains no Slang dependency. A material references its shader by **external path**
   (compiled by the cooker) **or inline base64** (a precompiled SPIR-V blob, for
   editor-produced materials).

> **Synchronous-only, by decision.** `AssetManager::LoadSync` blocks; uploads use
> today's `Image/Buffer::UploadSync` (`WaitIdle`) path. Async `Load`, the transfer
> queue, and the task system are **out of scope** — they are the next planset
> (future area 2, [threading-task-system.md](../future/threading-task-system.md)),
> which turns these synchronous loads non-blocking. The async-default naming the
> threading doc describes is designed so this planset's `LoadSync` keeps its name
> when async lands. **Bindless is also out of scope** — `Material` binds through
> today's per-set `DescriptorSet`; the bindless backing
> ([bindless-descriptors.md](../future/bindless-descriptors.md)) replaces it later.

## New project layout (plan 01)

Each library gets its own root subdirectory. Behaviour-preserving move first, new
libraries scaffolded into the new shape:

```
/CMakeLists.txt            top-level: shared deps + add_subdirectory(...) per lib
/engine/                   libveng — the runtime (loader only; no importer deps)
    include/Veng/...          (was /include/Veng — #include <Veng/…> unchanged)
    src/...                   (was /src)
/assetformat/              libveng_assetformat — shared archive + cooked-blob format
    include/Veng/Asset/...    AssetId, AssetType, archive reader/writer, blob structs
    src/...
/cooker/                   libveng_cook + the vengc CLI — stb, assimp, Slang, json
    include/ src/ tool/
/examples/hello-triangle/  the sample (+ its asset pack, plan 09)
/tests/                    include_hygiene, headless_smoke, compute_dispatch, gpu, +new
/cmake/  /docs/  /plans/
```

`assetformat` is the hinge: **Vulkan-free, importer-free, engine-independent**. It
must not include any `Renderer/` header (that would couple the format to the
engine and break the standalone-cooker boundary). Cooked-blob structs store engine
enums (e.g. `Renderer::Format`) as their **underlying integer type**; the engine
loader bridges with a `static_cast` guarded by a `static_assert`/`VE_ASSERT`
(planset house style — a loud one-line fix, not silent UB). `engine` links
`assetformat` PUBLIC (its types appear in the public `AssetManager` API);
`cooker` links it too. `include_hygiene` is extended to cover `assetformat`'s
public headers.

## The end-to-end target (plan 09 acceptance)

```jsonc
// examples/hello-triangle/assets/sample.vengpack.json  — hand-written
{
  "version": 1,
  "assets": [
    { "id": 1001, "type": "texture",  "source": "textures/brick.png" },
    { "id": 1002, "type": "mesh",     "source": "meshes/cube.obj" },
    { "id": 1003, "type": "material", "source": "materials/brick.vmat.json" }
  ]
}
```
```jsonc
// materials/brick.vmat.json  — a material referring to an external Slang shader
{
  "shader": { "path": "shaders/brick.slang" },     // OR { "spirv_b64": "…" }
  "textures": { "albedo": 1001 },                    // by AssetId
  "params":   { "tint": [1.0, 0.9, 0.8, 1.0] }
}
```
```sh
vengc cook examples/hello-triangle/assets/sample.vengpack.json -o sample.vengpack
```
```cpp
// in the sample, at runtime — by AssetId, no paths:
auto mat  = m_Assets->LoadSync<Material>(AssetId{1003}).value();  // pulls 1001 too
auto mesh = m_Assets->LoadSync<Mesh>(AssetId{1002}).value();
// … render mesh with mat …
```

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Project reorg into per-lib subdirectories](01-project-reorg.md) | Move engine to `engine/`; scaffold `assetformat/`, `cooker/`; top-level CMake. Behaviour-preserving. | proposed |
| 02 | [`assetformat`: AssetId, archive container, blob layouts](02-assetformat-lib.md) | The shared format lib + reader/writer + round-trip tests. | proposed |
| 03 | [`cooker` lib + `vengc` CLI + JSON pack parsing](03-cooker-cli-json-pack.md) | Stand up the tool; parse the pack JSON; importer-registry skeleton; emit a valid archive. | proposed |
| 04 | [Runtime: `AssetManager`, `AssetHandle`, pack mount, `LoadSync`](04-asset-manager-runtime.md) | Engine-side registry, structured `AssetLoadError`, deferred eviction. | proposed |
| 05 | [Texture vertical slice (stb cook → engine load)](05-texture-slice.md) | First full type, end to end; sample samples a cooked texture. | proposed |
| 06 | [Mesh (assimp cook → engine load)](06-mesh-assimp.md) | Cooked vertex/index buffers; sample draws a cooked mesh. | proposed |
| 07 | [Shader via Slang + offline reflection → `ShaderInterface`](07-shader-slang-reflection.md) | Absorbs deferred shader-reflection work; layouts from reflection. | proposed |
| 08 | [Material: JSON asset, inline/external shader, engine `Material`](08-material.md) | The headline; per-set binding (pre-bindless), validated against the interface. | proposed |
| 09 | [Example asset pack: hand-written JSON → build-time cook → load](09-example-pack.md) | `add_asset_pack` CMake fn; the full deliverable demonstrated. | proposed |
| 10 | [Docs + roadmap re-cut](10-docs-roadmap.md) | `ownership.md`, `CLAUDE.md`, `future/README`, `asset-system.md`, `plans/README`. | proposed |

## Dependency graph

```
01 reorg ──► 02 assetformat ──► 03 cooker/CLI ──┐
                   │                             ├─► 05 texture ──► 06 mesh ──┐
                   └──────────► 04 AssetManager ─┘                            │
                                                  07 shader (Slang) ──────────┼─► 08 material ──► 09 example pack ──► 10 docs
```

- **01** is the foundation; nothing else can land until the tree is reshaped.
- **02** (format) and **04** (runtime) are the two contracts; **03** is the tool
  shell. **02 → {03, 04}** because both the cooker and the loader speak the format.
- **05 texture** is the first vertical slice and the template every later type
  copies (cook side in 03's registry, load side in 04's loader table).
- **07 shader** is independent of 05/06 and can proceed in parallel; **08 material**
  needs 07 (its shader) and 05 (its textures). **06 mesh** is needed by 09 (the
  scene) and references materials by id (a forward ref, so it doesn't block on 08).

## New dependencies (all cooker-only)

The engine gains **only** `libveng_assetformat` (clean, in-repo). The cooker pulls,
via `FetchContent` with pinned tags like the rest:

- **nlohmann/json** — pack + material JSON parsing (cooker only; the runtime reads
  the *binary* archive, never JSON).
- **assimp** — mesh import (plan 06). The one heavy dependency; cooker-only, so it
  never reaches the engine or its consumers.
- **Slang** — `slangc` for material/shader compilation (plan 07). Prefer the
  prebuilt release binary; document the toolchain requirement.
- **SPIRV-Reflect** — uniform offline reflection of the final SPIR-V (plan 07), so
  both Slang-compiled and inline-precompiled shaders reflect through one path.
- **stb_image** — already vendored (`src/Vendor`); reused by the cooker for texture
  decode (plan 05).

## Out of scope (named, so it isn't half-built)

- **Async / threading** — the whole of future area 2. `LoadSync` only.
- **Bindless** — `Material` uses per-set `DescriptorSet`; bindless backing later.
- **Hot-reload / file watching** — needs a watcher and the async swap path; future.
  `Reload(id)` is *not* implemented this planset.
- **KTX2 / Basis / GPU-compressed textures, mip generation** — v1 textures are
  stb-decoded RGBA8, single mip. Cooked-format room is left for mips (plan 02).
- **A scene/level asset type** — meshes + materials + textures only.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in
the same pass → verify (clean build, `ctest` green, smoke binary writes a
correct-sized PPM) → update this table → one commit per plan,
`Plan NN: <summary>` with a `Co-Authored-By` trailer (`planset-5:` for
roadmap-only edits).

- **Validation discipline.** Plans that create GPU resources from cooked data
  (05/06/08) must pass the `VE_DEBUG` validation check (run the relevant binary
  from `build-debug/`, grep stderr for `Vulkan validation` ERROR) and must not
  widen the known descriptor-pool gap (CLAUDE.md).
- **Delegation.** The mechanical sweeps — the reorg's path/CMake churn (01), the
  cooker importer bodies (05/06 cook side) — are good `model: sonnet` subagent
  work; keep format design (02), the runtime/loader contract (04), and the
  reflection/material design (07/08) on the main thread.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified.

## On completion

Closes **future area 1's synchronous slice.** Update
[future/README.md](../future/README.md): mark area 1 taken up by planset-5 (sync),
leaving area 2 (threading → async loads) and the bindless rework as the named
follow-ons; re-cut the ordering so the remaining chain is
`5 (sync assets, done) → 2 threading (async) + bindless`. Update
[plans/README.md](../README.md) and trim [asset-system.md](../future/asset-system.md)
to the enduring async/bindless end-state vision.
