# Plan 10 — Example asset pack + project-wide GLSL removal

**Goal:** the deliverable, demonstrated — and GLSL gone from the repo. hello-triangle
ships a **hand-written JSON asset pack** — a pure `{ id, type, source }` manifest —
naming a texture, a mesh, a material, the material's two shaders, and the composite
pass's two shaders, each pointing at its own per-asset source file (`*.tex.json` /
`*.mesh.json` / `*.vmat.json` / `*.shader.json`). A new `add_asset_pack(...)` CMake
function cooks the pack with `vengc` at build time into a `.vengpack`; the sample
mounts it and renders the mesh + material + texture entirely **by `AssetId`**, and
builds its composite pipeline from shader assets loaded by id too.

The same conversion is applied **project-wide**: every remaining hand-compiled GLSL
shader — the sample's composite pass and the GPU tests' shaders
(`tests/shaders/*.{comp,vert,frag}`) — becomes a Slang source cooked through the
asset pack process and loaded by `AssetId`. Once nothing compiles GLSL, the
`add_shaders` / `glslc` build step is deleted outright: `cmake/Shaders.cmake`, its
`include()`, the top-level `add_shaders` block, and the package `install` of it all
go away. The engine library itself ships no GLSL today (its core pack is vertex
layouts; ImGui's shaders are vendored), so "project-wide" means the sample plus the
test shaders. The end state: **Slang is the only shader language in the tree, and
every shader reaches the GPU through a cooked pack.** This doubles as the worked
example for the docs.

## Why this is its own plan

The earlier type plans each wired a minimal ad-hoc cook step into the sample, and
plan 09b put every asset type behind its own source file. This plan consolidates
the cook into the real authoring workflow — a single hand-written pack, a reusable
build integration (`add_asset_pack`) replacing the inline `add_custom_command`, and
a clean load-by-id render loop — so the sample reflects how a veng app *actually*
ships assets, not a series of one-offs. With the authoring workflow real, it is also
the right moment to retire the *parallel* shader path that predates the asset system:
the hand-compiled GLSL still used by the sample's composite pass and the GPU tests.
Eradicating it here means there is exactly one way a shader reaches the GPU — Slang →
cooked pack → `LoadSync` — with no `glslc` build step to keep alive.

## The authored files (all under `examples/hello-triangle/assets/`)

```jsonc
// sample.vengpack.json   — pure { id, type, source } manifest
{
  "version": 1,
  "assets": [
    { "id": 1001, "type": "texture",  "source": "textures/brick.tex.json" },
    { "id": 1002, "type": "mesh",     "source": "meshes/cube.mesh.json" },
    { "id": 1003, "type": "material", "source": "materials/brick.vmat.json" },
    { "id": 1004, "type": "shader",   "source": "shaders/brick.vert.shader.json" },
    { "id": 1005, "type": "shader",   "source": "shaders/brick.frag.shader.json" },
    { "id": 1006, "type": "shader",   "source": "shaders/composite.vert.shader.json" },
    { "id": 1007, "type": "shader",   "source": "shaders/composite.frag.shader.json" }
  ]
}
```
```jsonc
// materials/brick.vmat.json   — explicit, typed, ordered fields (plan 09b)
{
  "shaders": { "vertex": 1004, "fragment": 1005 },
  "fields": [
    { "name": "Albedo",        "type": "texture", "id": 1001 },
    { "name": "AlbedoSampler", "type": "sampler", "texture": "Albedo" },
    { "name": "Factors",       "type": "vec4",    "value": [1.0, 1.0, 1.0, 1.0] }
  ]
}
```
```jsonc
// shaders/brick.vert.shader.json   — source-only (plan 09b)
{ "source": "brick.vert.slang", "entry": "vsMain", "vertex_layout": 5603155022528551788 }
```
```jsonc
// shaders/composite.vert.shader.json   — fullscreen-triangle vertex stage, no inputs
{ "source": "composite.vert.slang", "entry": "vsMain" }
```

Shaders are always compiled from `source` by the cooker; there is no precompiled
inline path. The brick shaders and per-asset source files (and the manifest form of
the pack) are established by plan 09b — this plan consumes them. The **composite
shaders are authored here**, ported from the sample's old GLSL: `composite.vert`
emits the fullscreen triangle from `SV_VertexID` (no vertex inputs, so its
`*.shader.json` omits `vertex_layout`), and `composite.frag` declares the bindless
set-0 arrays (`Texture2D g_Textures[]` / `SamplerState g_Samplers[]` at
`binding(0,0)`/`(1,0)`, excluded from the reflected interface) and a push-constant
block `{ SceneTexture, ImGuiTexture, Sampler }`. Ids 1006/1007 above are
placeholders minted with `vengc generate-id` during implementation.

## The CMake integration

```cmake
# cmake/AssetPack.cmake — mirrors add_shaders' shape
add_asset_pack(hello_triangle_assets
    PACK    assets/sample.vengpack.json
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/assets/sample.vengpack)
add_dependencies(hello_triangle hello_triangle_assets)
```

- A custom command that runs the built `vengc` on the pack JSON, with the cooked
  archive landing in the build tree; the sample bakes its path in via a compile
  definition (`HT_ASSET_DIR`) so it runs from any working directory.
- Depends on the `vengc` target so the tool builds first; re-cooks when the pack or
  its sources change.
- With every sample shader now an asset in this pack, the example's
  `add_shaders(hello_triangle_shaders …)` call and its `HT_SHADER_DIR` compile
  definition are deleted, and the `examples/hello-triangle/shaders/` directory
  (composite + dead triangle GLSL) is removed.
- Once the test shaders are ported too (see **Project-wide GLSL removal** below),
  `add_shaders` has no callers: delete `cmake/Shaders.cmake`, its
  `include(... Shaders.cmake)` (top-level `CMakeLists.txt:17`), the top-level
  `add_shaders(veng_compute_dispatch_shaders …)` block (~`:327`), and the package
  `install(FILES cmake/Shaders.cmake …)` (~`:443`) — and drop it from the generated
  `veng-config.cmake` include set. `find_package(Vulkan REQUIRED)` stays (the engine
  links Vulkan), but `glslc` is no longer invoked anywhere.

## The sample render loop (by id)

```cpp
void OnInitialize() override {
    m_Assets->Mount(HT_ASSET_PACK);                          // the cooked .vengpack
    m_Material = m_Assets->LoadSync<Material>(AssetId{1003}).value();  // pulls 1001 + shader
    m_Mesh     = m_Assets->LoadSync<Mesh>(AssetId{1002}).value();
}
void OnRender() override {
    m_Material->Bind(cmd);                                    // material is the draw interface
    cmd.BindVertexBuffer(m_Mesh->GetVertexBuffer());
    cmd.BindIndexBuffer(m_Mesh->GetIndexBuffer());
    for (auto& s : m_Mesh->GetSubMeshes()) cmd.DrawIndexed(s.IndexCount, s.IndexOffset);
}
void OnDispose() override { m_Material = {}; m_Mesh = {}; }   // release handles before teardown
```

## Project-wide GLSL removal

The sample is the headline, but it is not the last GLSL in the tree. Two GPU-test
executables compile shaders with `add_shaders` into a shared `shaders/` build dir
and load them with `Shader::Create({.Path = …spv})`:

- `veng_compute_dispatch` (`CD_SHADER_DIR`): `invert.comp`, `fullscreen.vert`,
  `sample.frag`.
- `veng_gpu` (`GPU_SHADER_DIR`): `bindless_sample.frag` + the shared
  `fullscreen.vert` (its bindless-registry case).

These are ported the same way: each `tests/shaders/*.{comp,vert,frag}` becomes a
`*.slang` + `*.shader.json`, cooked into a small **test `.vengpack`** by
`add_asset_pack`, and loaded via `LoadSync<Renderer::ShaderAsset>`. The engine has
**no runtime Slang compiler** — cooking is offline-only — so the tests must mount a
cooked pack to get a shader, exactly like an app; there is no direct-from-source
shortcut. Notes:

- None of the test shaders declare vertex inputs (`fullscreen.vert` pulls from
  `SV_VertexID`; the rest are fragment/compute), so no `vertex_layout` and no
  `--reference` is needed for the test pack.
- `invert.comp` is the first **compute** shader through the cooker
  (`ShaderImporter` already maps `SLANG_STAGE_COMPUTE`); its asset's `.Module`
  feeds `ComputePipeline::Create` unchanged.
- The tests keep their hand-built `DescriptorSetLayout` / `PipelineLayout` (set 1
  storage images, push constants) and use only the asset's `.Module` — the
  reflected `ShaderInterface` is not consumed here, matching the composite pass.
- `tests/shaders/*.{comp,vert,frag}` and both `*_SHADER_DIR` compile definitions are
  deleted; the GPU-test harness (`tests/test_support` / `GpuFixture`) gains whatever
  small mount-a-pack helper the ported tests need.

With both the sample and the tests off GLSL, the `add_shaders` machinery is deleted
globally (see the last bullet under **The CMake integration**).

## Work

1. `cmake/AssetPack.cmake` with `add_asset_pack` (mirroring `add_shaders`'
   shape: a custom command running `vengc cook` on the pack JSON, depending on the
   `vengc` target and the pack's sources, with `--reference` for the engine core
   pack so the shader's `vertex_layout` id resolves); wire it into the sample's
   `CMakeLists.txt`, replacing the inline `add_custom_command` cook step.
2. Confirm the manifest pack + per-asset source files (`brick.tex.json`,
   `cube.mesh.json`, `brick.vmat.json`, `brick.{vert,frag}.shader.json`) and the
   `brick.png` / `cube.obj` / `brick.{vert,frag}.slang` binaries they reference are
   in place — authored by the earlier type plans and converted to source-file form
   by plan 09b; this plan does not re-author them.
3. Author the composite shaders as assets: port `composite.{vert,frag}` from GLSL to
   `composite.{vert,frag}.slang` + `composite.{vert,frag}.shader.json`, add their two
   `shader` entries to the manifest, and add the new sources to the cook's `DEPENDS`.
4. Swap `CreateCompositePipeline()` off `Shader::Create({.Path = …spv})` to
   `LoadSync<Renderer::ShaderAsset>(AssetId{…})`, feeding `.Module` into the
   pipeline's `ShaderStages` (mirroring `MaterialLoader`); the hand-built composite
   `PipelineLayout` (push-constant range + bindless set 0) is unchanged.
5. Remove the example's GLSL build step: delete the `add_shaders(hello_triangle_shaders …)`
   call + its `add_dependencies`, drop the `HT_SHADER_DIR` compile definition, and
   `git rm -r examples/hello-triangle/shaders/` (composite + dead triangle GLSL).
6. The sample mounts the pack and renders purely by `AssetId`, releasing all handles
   in `OnDispose` (in place from the type plans — verify it still holds).
7. Port the GPU-test shaders (see **Project-wide GLSL removal**): author
   `tests/shaders/*.{comp,vert,frag}` as `*.slang` + `*.shader.json`, cook a test
   `.vengpack` via `add_asset_pack`, and rework `veng_compute_dispatch` and
   `veng_gpu` to mount it and `LoadSync<ShaderAsset>` instead of `Shader::Create`
   from `CD_SHADER_DIR` / `GPU_SHADER_DIR`.
8. Delete the GLSL build step globally: `cmake/Shaders.cmake`, its `include()`, the
   top-level `add_shaders(veng_compute_dispatch_shaders …)` block, the
   `install(FILES cmake/Shaders.cmake …)`, and the `Shaders.cmake` line in the
   generated `veng-config`. Remove the now-deleted `tests/shaders/` GLSL and both
   `*_SHADER_DIR` definitions.
9. Verify the headless smoke path mounts + loads + renders the pack (the `Headless`
   smoke run is the CI gate for this), and that `ctest` is green — including the
   `gpu`-labelled `veng_compute_dispatch` / `veng_gpu` on their new cooked packs and
   the `validation` gate under `VE_DEBUG`.

## Dependencies

Plans 06 (texture), 07 (mesh), 09 (material), 09b (per-asset source files +
manifest pack) — every type must exist in its source-file form. The capstone
demonstration.

## Acceptance

- Clean build cooks `sample.vengpack` via `vengc` as a build step.
- Smoke binary writes a correct-sized PPM rendering the cube with the brick
  material + texture, loaded entirely by id from the cooked pack.
- **No GLSL remains in the repo.** `examples/hello-triangle/shaders/` and
  `tests/shaders/` are gone; no target invokes `add_shaders` / `glslc`;
  `cmake/Shaders.cmake` is deleted and absent from the installed package. A
  tree-wide search for `*.{vert,frag,comp,glsl}` returns nothing.
- The `gpu`-labelled tests (`veng_compute_dispatch`, `veng_gpu`) pass loading their
  shaders from a cooked test pack by `AssetId`.
- **Validation-clean** under `VE_DEBUG` for the full sample.
- Hand-editing an id in the pack JSON and rebuilding re-cooks and still loads —
  proving ids are author-owned and the workflow is repeatable.

## Notes

- This is the moment the README's end-to-end target is literally true: a
  hand-written JSON pack → `vengc` → archive → `LoadSync` by id → on screen.
- The sample's old hard-coded GLSL triangle path is retired and deleted here, so the
  diff that converts the composite pass and removes `add_shaders` is the legible end
  of that arc; the smoke PPM stays non-deterministic (rotation), so verify by size +
  exit 0, not golden pixels (CLAUDE.md).
- `add_asset_pack` is the durable artifact here — other examples/tests reuse it,
  and the ported GPU tests are its first non-sample consumers.
- This plan is large because it is two things at once: the example deliverable and
  the project-wide GLSL eradication. They share the same `add_asset_pack` artifact
  and the same Slang-asset load path, so doing them together avoids standing up the
  test-pack plumbing twice; if the diff gets unwieldy, the test-shader port (Work 7)
  is the natural seam to land as a second commit within the plan.
