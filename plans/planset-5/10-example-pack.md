# Plan 10 — Example asset pack: hand-written JSON → build-time cook → load

**Goal:** the deliverable, demonstrated. hello-triangle ships a **hand-written JSON
asset pack** — a pure `{ id, type, source }` manifest — naming a texture, a mesh, a
material, and the material's two shaders, each pointing at its own per-asset source
file (`*.tex.json` / `*.mesh.json` / `*.vmat.json` / `*.shader.json`). A new
`add_asset_pack(...)` CMake function cooks the pack with `vengc` at build time into
a `.vengpack`; the sample mounts it and renders the mesh + material + texture
entirely **by `AssetId`**. This is the proof that the whole pipeline works end to
end, and it doubles as the worked example for the docs.

## Why this is its own plan

The earlier type plans each wired a minimal ad-hoc cook step into the sample, and
plan 09b put every asset type behind its own source file. This plan consolidates
the cook into the real authoring workflow — a single hand-written pack, a reusable
build integration (`add_asset_pack`) replacing the inline `add_custom_command`, and
a clean load-by-id render loop — so the sample reflects how a veng app *actually*
ships assets, not a series of one-offs.

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
    { "id": 1005, "type": "shader",   "source": "shaders/brick.frag.shader.json" }
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

Shaders are always compiled from `source` by the cooker; there is no precompiled
inline path. The per-asset source files (and the manifest form of the pack) are
established by plan 09b — this plan consumes them and supplies the build
integration and the by-id render loop.

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
  definition (like `HT_SHADER_DIR`) so it runs from any working directory.
- Depends on the `vengc` target so the tool builds first; re-cooks when the pack or
  its sources change.

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
3. The sample mounts the pack and renders purely by `AssetId`, releasing all handles
   in `OnDispose` (in place from the type plans — verify it still holds).
4. Verify the headless smoke path mounts + loads + renders the pack (the
   `Headless` smoke run is the CI gate for this).

## Dependencies

Plans 06 (texture), 07 (mesh), 09 (material), 09b (per-asset source files +
manifest pack) — every type must exist in its source-file form. The capstone
demonstration.

## Acceptance

- Clean build cooks `sample.vengpack` via `vengc` as a build step.
- Smoke binary writes a correct-sized PPM rendering the cube with the brick
  material + texture, loaded entirely by id from the cooked pack.
- **Validation-clean** under `VE_DEBUG` for the full sample.
- Hand-editing an id in the pack JSON and rebuilding re-cooks and still loads —
  proving ids are author-owned and the workflow is repeatable.

## Notes

- This is the moment the README's end-to-end target is literally true: a
  hand-written JSON pack → `vengc` → archive → `LoadSync` by id → on screen.
- Keep the sample's old hard-coded triangle path available (or clearly retired) so
  the diff is legible; the smoke PPM stays non-deterministic (rotation), so verify
  by size + exit 0, not golden pixels (CLAUDE.md).
- `add_asset_pack` is the durable artifact here — other examples/tests reuse it.
