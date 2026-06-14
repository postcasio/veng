# Plan 05 — Texture vertical slice (stb cook → engine load)

**Goal:** the first asset type, end to end. The cooker decodes an image with
`stb_image` into a `CookedTextureHeader` + pixel blob; the engine registers a
`Texture` loader that turns that blob into an `Image` + `ImageView` via
`UploadSync`; the sample mounts a cooked pack and samples the loaded texture. This
is the template every later type copies — cook importer on one side, runtime loader
on the other, meeting at the `assetformat` blob.

## Why this is its own plan

Texture is the simplest type that exercises the *whole* pipeline (JSON entry →
importer → archive → mount → loader → GPU resource → drawn), so it's where the
two tables (cooker importer, engine loader) are first proven against real GPU data.
Mesh and material then copy this shape.

## Cook side (`libveng_cook`)

```jsonc
{ "id": 1001, "type": "texture", "source": "textures/brick.png",
  "srgb": true, "flip_y": false }
```

- A `TextureImporter : AssetImporter` (registered in the cooker). `Cook`:
  `stbi_load` the `source` (relative to the pack dir) as RGBA8 → fill
  `CookedTextureHeader { Format, Width, Height, MipCount=1 }` + append the pixel
  bytes → return the blob. `Format` is the **underlying integer** of
  `Renderer::Format::R8G8B8A8_Unorm` / `_Srgb` (per the cycle-avoidance rule,
  plan 02), chosen from the `srgb` flag.
- stb is already vendored; the cooker compiles the stb TU (no new dep).
- Errors → `Result` ("texture id 1001: cannot decode brick.png").

## Load side (`libveng`)

- A `Texture` public type:
  ```cpp
  class Texture
  {
  public:
      [[nodiscard]] Ref<Image>     GetImage() const;
      [[nodiscard]] Ref<ImageView> GetView() const;
      [[nodiscard]] Renderer::Format GetFormat() const;
      [[nodiscard]] uvec2          GetExtent() const;
  };
  ```
- A `TextureLoader : AssetLoader` registered in `AssetManager`'s constructor.
  `Load`: read `CookedTextureHeader`, bridge `Format` (the guarded
  `static_cast<Renderer::Format>` + `static_assert`), `Image::Create` (sampled,
  the right extent/format) + `image->UploadSync(pixels)` + an `ImageView`. Returns
  the `Ref<Texture>`. **`UploadSync` only** — the `WaitIdle` path (async is the
  threading planset).

## Sample migration

hello-triangle gains a tiny cooked pack (a single texture) cooked at build time
(the full `add_asset_pack` machinery is plan 09; here a minimal `vengc cook`
custom command suffices) and samples the loaded texture in the composite/triangle
pass instead of (or in addition to) its current path — proving a cooked texture
reaches the screen.

## Work

1. Cooker: `TextureImporter` + register it; a fixture pack + image under
   `tests/`/`cooker`.
2. Engine: `Texture` type, `TextureLoader`, register in `AssetManager`; the
   `Format` bridge with its `static_assert`.
3. Sample: cook a one-texture pack, mount it in `OnInitialize`,
   `LoadSync<Texture>(id)`, sample it; release the handle in `OnDispose`.
4. Tests: extend the GPU suite — cook (or fixture) a texture archive, mount,
   `LoadSync<Texture>`, assert format/extent and that the image uploaded (a
   readback or a clear+sample check consistent with the existing `image_*` GPU
   cases).

## Dependencies

Plans 03 (importer table) and 04 (loader table + `AssetManager`). First consumer of
both. Blocks 06/08 only by precedent (they copy the shape), not by code.

## Acceptance

- Clean build, `ctest` green incl. the new texture cook + load tests.
- Smoke binary writes a correct-sized PPM with the cooked texture in the render.
- **Validation-clean** under `VE_DEBUG`: run the sample/GPU binary from
  `build-debug/`, grep stderr — no new `Vulkan validation` ERROR (the sampled-image
  path is the clean one per CLAUDE.md; do not widen the storage-image gap).

## Notes

- **RGBA8 single-mip v1.** Mip generation, KTX2/Basis, and GPU-compressed formats
  are out of scope; the `MipCount` field and blob layout already leave room
  (plan 02) so adding them later is additive.
- This plan deliberately uses a hand-rolled `vengc` build step for the sample;
  plan 09 generalizes it to `add_asset_pack`. Don't over-build the CMake here.
- Keep the loader the only Context-touching code; the importer stays GPU-free so it
  could run on a worker once threading lands.
