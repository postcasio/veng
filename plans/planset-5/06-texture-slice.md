# Plan 06 — Texture (JSON source + sampler → stb cook → bindless load)

**Goal:** the first asset type, end to end, and the first consumer of the bindless
registry. A texture has its **own JSON source file** (like a material) that names
the image binary and declares its **sampler settings** and import options; the
cooker decodes the image with `stb_image` into a cooked blob carrying the pixels +
sampler state; the engine loader creates an `Image`/`ImageView`/`Sampler`,
**registers them into the `BindlessRegistry`**, and the `Texture` carries its
`TextureHandle` + `SamplerHandle`. This is the template every later type copies.

## Why this is its own plan

Texture is the simplest type that exercises the *whole* pipeline (JSON authoring →
importer → archive → mount → loader → GPU resource → bindless slot → drawn), so
it's where the two tables (cooker importer, engine loader) and the registry first
meet real GPU data. Mesh and material then copy this shape.

## The texture JSON source

The pack entry points at a per-asset JSON file (symmetric with materials), which in
turn references the image binary:

```jsonc
// pack entry
{ "id": 1001, "type": "texture", "source": "textures/brick.tex.json" }
```
```jsonc
// textures/brick.tex.json
{
  "image":  "brick.png",        // binary, relative to this file
  "srgb":   true,
  "generate_mips": false,        // v1: false only (mip-gen is future)
  "sampler": {
    "min": "linear", "mag": "linear", "mipmap": "linear",
    "wrap_u": "repeat", "wrap_v": "repeat",
    "anisotropy": 8.0
  }
}
```

Sampler enum strings map to veng's `Renderer::` vocabulary (`Filter`,
`SamplerAddressMode`, …) at cook time, stored as underlying integers in the blob
(the cycle-avoidance rule, plan 02) and bridged back on load.

## Cook side (`libveng_cook`)

- A `TextureImporter : AssetImporter`. `Cook`: parse the texture JSON,
  `stbi_load` the referenced image (relative to the JSON's dir) as RGBA8, fill
  `CookedTextureHeader { Format, Width, Height, MipCount=1, Sampler… }` + append
  pixels. `Format` is the underlying int of `R8G8B8A8_Srgb`/`_Unorm` from `srgb`.
  Sampler settings are packed into the header (plan 02's reserved fields extend to
  carry them).
- stb is already vendored — no new dep.

## Load side (`libveng`)

```cpp
class Texture
{
public:
    [[nodiscard]] Ref<Image>     GetImage() const;
    [[nodiscard]] Ref<ImageView> GetView() const;
    [[nodiscard]] Ref<Sampler>   GetSampler() const;
    [[nodiscard]] TextureHandle  GetHandle() const;        // bindless sampled-image slot
    [[nodiscard]] SamplerHandle  GetSamplerHandle() const; // bindless sampler slot
    [[nodiscard]] Renderer::Format GetFormat() const;
    [[nodiscard]] uvec2          GetExtent() const;
};
```

- A `TextureLoader : AssetLoader`. `Load`: read the header (bridge `Format` +
  sampler enums with guarded `static_cast`), `Image::Create` + `UploadSync`
  (the `WaitIdle` path — async is the threading planset), `ImageView::Create`,
  `Sampler::Create` from the cooked sampler state, then **register the view and
  sampler into `Context`'s `BindlessRegistry`** and store the returned handles.
  The `Texture` *is* a `u32` to the renderer (its `TextureHandle`).
- On eviction, the registry slots are released through the deferred path (plan 05).

## Sample migration

hello-triangle authors a `brick.tex.json`, cooks a one-texture pack, mounts it,
`LoadSync<Texture>(id)`, and samples it through **set 0** using the texture's
handle (building on plan 05's bindless sampling path) — proving a cooked texture
reaches the screen via the registry. (The full `add_asset_pack` machinery is
plan 10; a minimal `vengc` custom command suffices here.)

## Work

1. Cooker: `TextureImporter` (JSON source + sampler) + register; a fixture
   `.tex.json` + image under `tests/`.
2. Engine: `Texture` type, `TextureLoader` (creates + registers into bindless),
   register in `AssetManager`; the `Format`/sampler enum bridges.
3. `assetformat`: extend `CookedTextureHeader` to carry sampler settings.
4. Sample: cook + mount + `LoadSync<Texture>` + sample via the handle; release in
   `OnDispose`.
5. Tests: cook (or fixture) a texture archive, mount, `LoadSync<Texture>`, assert
   format/extent/sampler and that it registered a valid `TextureHandle`; sample it
   bindlessly.

## Dependencies

Plans 03 (importer table), 04 (loader table), and **05 (bindless registry)** — the
texture registers into it. First consumer of all three. Precedent for 07/09.

## Acceptance

- Clean build, `ctest` green incl. the texture cook + load tests.
- Smoke binary writes a correct-sized PPM with the cooked texture sampled via set 0.
- **Validation-clean** under `VE_DEBUG` (the sampled-image bindless path; do not
  widen the storage-image gap).

## Notes

- **RGBA8 single-mip v1.** Mip-gen, KTX2/Basis, GPU-compressed formats are out of
  scope; `MipCount` + the blob layout leave room (plan 02) so it's additive.
- **Sampler dedup** is a later optimization — v1 registers a sampler per texture;
  the registry's sampler array is small, and dedup (a sampler-state → handle cache)
  can come when profiling asks.
- Importer stays GPU-free (could run on a worker once threading lands); the loader
  is the only Context/registry-touching code.
