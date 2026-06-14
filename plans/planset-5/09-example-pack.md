# Plan 09 — Example asset pack: hand-written JSON → build-time cook → load

**Goal:** the deliverable, demonstrated. hello-triangle gets a **hand-written JSON
asset pack** referring to a JSON material (which refers to an external Slang shader,
with an inline-base64 variant shown too), a texture, and a mesh. A new
`add_asset_pack(...)` CMake function cooks the pack with `vengc` at build time into
a `.vengpack`; the sample mounts it and renders the mesh + material + texture
entirely **by `AssetId`**. This is the proof that the whole pipeline works end to
end, and it doubles as the worked example for the docs.

## Why this is its own plan

The earlier type plans each wired a minimal ad-hoc cook step into the sample. This
plan consolidates them into the real authoring workflow — a single hand-written
pack, a reusable build integration (`add_asset_pack`), and a clean
load-by-id render loop — so the sample reflects how a veng app *actually* ships
assets, not a series of one-offs.

## The authored files (all under `examples/hello-triangle/assets/`)

```jsonc
// sample.vengpack.json
{
  "version": 1,
  "assets": [
    { "id": 1001, "type": "texture",  "source": "textures/brick.png", "srgb": true },
    { "id": 1002, "type": "mesh",     "source": "meshes/cube.obj" },
    { "id": 1003, "type": "material", "source": "materials/brick.vmat.json" }
  ]
}
```
```jsonc
// materials/brick.vmat.json   (external-shader form)
{ "shader": { "path": "shaders/brick.slang" },
  "textures": { "albedo": 1001 },
  "params": { "tint": [1,1,1,1] } }
```

A second material file demonstrates the **inline** form
(`{ "shader": { "spirv_b64": "…" } }`) cooked into the same or a sibling pack, so
both shader-reference paths are exercised by the shipped sample.

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

1. `cmake/AssetPack.cmake` with `add_asset_pack`; wire it into the sample's
   `CMakeLists.txt`, replacing the ad-hoc cook steps from plans 05/06.
2. Author the pack JSON, the material JSON (both shader forms), and bring in the
   `brick.png` / `cube.obj` / `brick.slang` source assets.
3. Rewrite the sample to mount the pack and render purely by `AssetId`; release all
   handles in `OnDispose`.
4. Verify the headless smoke path mounts + loads + renders the pack (the
   `Headless` smoke run is the CI gate for this).

## Dependencies

Plans 05 (texture), 06 (mesh), 08 (material) — every type must exist. The capstone
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
