# The flipbook-atlas manifest

A flipbook-atlas manifest is the JSON sidecar a sprite-sheet tool writes beside its images. It
describes a grid of frames and the images that share it. veng's `FlipbookImporter` cooks it as it
stands: a pack entry of type `Flipbook` names the manifest (`*.atlas.json`) as its `source`.

There are two schema versions, and both cook:

- **Version 1** describes one finished picture per frame: a single `atlas` image with a declared
  alpha convention.
- **Version 2** names several images sharing one grid. It is either one **unlit** premultiplied
  colour sheet, or two **six-way lightmaps** and a **temperature** that the renderer lights and
  colours at runtime. Either kind may add a **motion-vector** sheet. A six-way manifest may also
  carry a default **emission ramp**.

## The grid

Frame `f` (0-based) is the tile at column `f % columns`, row `floor(f / columns)`, counted from the
image's top-left. Its pixel origin is `(column · frame.width, row · frame.height)`. The populated
frames run row-major. Every image a manifest names is `columns · frame.width` pixels wide and
`rows · frame.height` pixels tall.

## Fields both versions share

| Field | Type | Meaning |
|---|---|---|
| `version` | int | `1` or `2`. Any other value is refused. |
| `type` | string | Always `"flipbook-atlas"`. |
| `frame.width`, `frame.height` | int > 0 | One tile's size in pixels. |
| `columns`, `rows` | int > 0 | Tiles across and down; `columns · rows ≥ frames`. |
| `frames` | int > 0 | Populated frames. |
| `fps` | number > 0 | The intended playback rate. |
| `loop` | bool | Whether the sequence is intended to loop. |
| `blend` | string | Recommended compositing: `"alpha"` (over) or `"additive"` (glow). |
| `worldExtent` | `[x, y, z]`, optional | The world-space size one frame represents. |
| `pivot` | `[x, y, z]`, optional | The effect's anchor inside `worldExtent`, in world units from its minimum corner. |

Every other top-level key the cook does not name here is free-form and ignored: a tool may write
`source`, `technique`, `name`, `provenance`, or anything else it wants to keep beside its export.

## Version 1

| Field | Type | Meaning |
|---|---|---|
| `atlas.file` | string | The atlas image, relative to the manifest. A manifest without one is a frame-sequence export and is refused. |
| `atlas.width`, `atlas.height` | int > 0 | The atlas's size, which must be the grid's. |
| `alpha` | string | How alpha relates to RGB: `"premultiplied"` (RGB already multiplied by coverage), `"coverage"` (straight coverage), `"luminance"` (alpha derived from luminance; the stored alpha is ignored), or `"opaque"` (alpha is 1; the stored alpha is ignored). |
| `colorSpace` | string | `"srgb"` or `"linear"`: how the stored 8-bit values are read. |

`blend` and `alpha` are orthogonal: `"alpha"` with `"premultiplied"` composites premultiplied-over
(`ONE, ONE_MINUS_SRC_ALPHA`), and `"alpha"` with `"coverage"` composites straight-over
(`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`).

A version-1 manifest cooks as an **unlit** flipbook that **steps** from frame to frame.
`frameScale` is a version-2 field and is refused here.

## Version 2

### `shading` and `textures`

`"shading"` is required, and is `"unlit"` or `"sixway"`.

`"textures"` is an object of named entries. Each entry is
`{ "file", "width", "height", "colorSpace" }`, with `file` relative to the manifest, the size
equal to the grid's, and `colorSpace` `"srgb"` or `"linear"`. Which names a manifest may use
depends on its shading, and a name outside the set is refused, so a misspelt `lighta` is caught:

| shading | required | optional |
|---|---|---|
| `unlit` | `color` | `motion` |
| `sixway` | `lightA`, `lightB` | `motion` |

**There is no `alpha` key in version 2**, and no top-level `atlas` or `colorSpace`. Each texture's
alpha has one fixed meaning:

| texture | RGB | A |
|---|---|---|
| `color` | colour, premultiplied by coverage | coverage |
| `lightA` | light received from **+X, +Y, −Z** | coverage |
| `lightB` | light received from **−X, −Y, +Z** | temperature |
| `motion` | RG: motion; B unused | unused |

### Colour spaces

Everything version 2 stores is **scene-linear** before any transfer: never tonemapped, never
exposure-scaled. The lightmaps and `color` may be stored under the sRGB transfer for precision in
the darks; the transfer applies to RGB only, and alpha is always linear. `motion` must be
`"linear"`.

### The six-way lightmaps

**The axes are the sprite's own frame:** X right, Y up, Z toward the viewer. The baked camera looks
along −Z, so `lightB`'s +Z is the light falling on the face toward the camera, and `lightA`'s −Z
the light arriving from behind.

**Each channel is a unit-light response.** It is the light that reaches the eye from a white light
of unit irradiance arriving along that axis, self-shadowing and scattering included. It carries no
light colour and no exposure.

- **The normalisation:** a response of 1 is what a white Lambertian card facing that axis returns.
  The renderer's outgoing radiance is `response · E / π` for an irradiance `E`, the same diffuse
  normalisation its surfaces use.
- **Premultiplied:** a channel already counts how little of the texel the medium covers. That is
  the form a volume march produces (`∫ T σ L ds`), and it composites premultiplied-over with
  `lightA.a` as the coverage.

### Temperature

`lightB.a` stores the normalised temperature `t ∈ [0, 1]` of the emission the eye sees at the
texel, and 0 where nothing visible emits. The renderer emits `ramp(t) · coverage`, so a baker that
hides a hot core behind soot writes the lower temperature the eye actually sees there.

What is stored is the transfer, not `t` itself:

| `temperatureEncoding` | stored value |
|---|---|
| `"sqrt"` (the default) | `√t`, spending precision on the hot end |
| `"linear"` | `t` |

`temperatureEncoding` is a six-way field and is refused on an unlit manifest.

### The emission ramp

```json
"emission": {
  "ignition": 0.25,
  "ramp": [
    { "t": 0.25, "color": [0.8, 0.12, 0.0] },
    { "t": 0.6,  "color": [6.0, 2.2, 0.4] },
    { "t": 1.0,  "color": [24.0, 18.0, 10.0] }
  ]
}
```

- It is optional, and a six-way field: it is refused on an unlit manifest.
- `ramp` holds 1 to 8 keys. Each `t` is in [0, 1], strictly ascending.
- Each `color` is linear HDR in the renderer's exposure-scaled units, the units a surface's
  emissive is authored in; every channel is finite and non-negative.
- `ignition` is in [0, 1] (default 0). Emission is zero below it.
- Leaving `emission` out means the flipbook emits nothing by default. The temperature is still
  stored, so a renderer may supply a ramp of its own.

### Per-frame extent: `frameScale`

`"frameScale": [s₀, s₁, …]` is optional. It has one entry per frame, each in (0, 1]. Frame `i`
covers `worldExtent · sᵢ`, centred on the same point as the full frame. So a cloud that grows
twentyfold can fill every frame instead of a twentieth of frame 0. Leaving it out means every
`sᵢ = 1`.

### Motion

`motion`'s RG hold the offset that carries a texel of frame N to its position in frame N+1, stored
at frame N's texel.

- **Units:** extent-normalised. The offset is the world displacement divided by `worldExtent`, with
  `u` running right and `v` running down the frame like the image's. With no `frameScale` that is
  exactly a frame-UV offset; with one, the units stay the same across frames of different zoom, so
  two frames blend in one space.
- **Scale:** the entry requires `"scale": s > 0`, the largest offset representable.
- **Encoding:** `stored = 0.5 + m / (2s)`. An 8-bit source writes the centre as 128, so the decode
  is `m = (255 · stored − 128) / 127 · s`: 128 decodes to exactly no motion, and 255 to exactly `s`.

## How it cooks

Each image cooks through the ordinary texture path, mipped, with a linear clamp-to-edge sampler.
The build configuration maps each texture's role to a codec:

| texture | role (sRGB / linear) | alpha while filtering mips |
|---|---|---|
| version-1 `atlas` | `Color` / `Packed` | as declared: premultiplied, straight for `coverage`, ignored for `luminance` and `opaque` |
| `color`, `lightA` | `Color` / `Packed` | premultiplied |
| `lightB` | `Color` / `Packed` | discarded: written opaque |
| `motion` | `Vector` | data |
| temperature (`lightB.a`) | `Mask`, as a texture of its own | data |

**The temperature cooks as a single-channel texture of its own**, not in `lightB`'s alpha. BC7 and
ASTC alpha were measured holding a √t field with a steep front far worse than BC4 or
single-channel ASTC hold it, so the cook moves the channel out; the schema a baker writes is the
same either way.

The cook records which way a player should blend between frames, from the source: a version-1
sheet **steps**, a version-2 sheet without `motion` **crossfades**, and one with `motion` **warps
along the motion vectors**.

## Examples

### Unlit, with motion

```json
{
  "version": 2,
  "type": "flipbook-atlas",
  "name": "sparks",
  "frame": { "width": 128, "height": 128 },
  "columns": 8,
  "rows": 8,
  "frames": 60,
  "fps": 30,
  "loop": true,
  "blend": "additive",
  "shading": "unlit",
  "textures": {
    "color":  { "file": "sparks.color.png",  "width": 1024, "height": 1024, "colorSpace": "srgb" },
    "motion": { "file": "sparks.motion.png", "width": 1024, "height": 1024, "colorSpace": "linear",
                "scale": 0.1 }
  }
}
```

### Six-way, with motion, a ramp and a growing extent

```json
{
  "version": 2,
  "type": "flipbook-atlas",
  "name": "fireball",
  "frame": { "width": 256, "height": 256 },
  "columns": 4,
  "rows": 4,
  "frames": 16,
  "fps": 24,
  "loop": false,
  "blend": "alpha",
  "worldExtent": [30.0, 30.0, 30.0],
  "pivot": [15.0, 15.0, 15.0],
  "shading": "sixway",
  "textures": {
    "lightA": { "file": "fireball.lightA.png", "width": 1024, "height": 1024, "colorSpace": "srgb" },
    "lightB": { "file": "fireball.lightB.png", "width": 1024, "height": 1024, "colorSpace": "srgb" },
    "motion": { "file": "fireball.motion.png", "width": 1024, "height": 1024, "colorSpace": "linear",
                "scale": 0.05 }
  },
  "temperatureEncoding": "sqrt",
  "emission": {
    "ignition": 0.2,
    "ramp": [
      { "t": 0.2, "color": [0.6, 0.08, 0.0] },
      { "t": 0.6, "color": [5.0, 1.8, 0.3] },
      { "t": 1.0, "color": [20.0, 15.0, 9.0] }
    ]
  },
  "frameScale": [0.06, 0.12, 0.2, 0.28, 0.36, 0.44, 0.52, 0.6,
                 0.67, 0.74, 0.8, 0.85, 0.9, 0.94, 0.97, 1.0]
}
```

## Refusals

Every refusal is a cook error naming the manifest and the field.

**Either version:** a `version` other than 1 or 2; a `type` other than `"flipbook-atlas"`; a missing
or non-positive grid field; more `frames` than the grid holds; a non-positive `fps`; a non-boolean
`loop`; an unknown `blend`; a `worldExtent` or `pivot` that is not three numbers; an image whose
decoded size is not the grid's.

**Version 1:** no `atlas` object; no `atlas.file` (a frame-sequence export); an atlas size that is
not the grid's; an unknown `alpha` or `colorSpace`; `frameScale`.

**Version 2:**

- an unknown `shading`;
- `alpha`, `atlas` or a top-level `colorSpace`;
- a texture missing for its shading, or present outside it;
- an entry missing `file`, `width`, `height` or `colorSpace`, or with an unknown `colorSpace`;
- an entry whose size is not the grid's;
- `motion` without a positive `scale`, or with a `colorSpace` other than `"linear"`;
- `frameScale` with a length other than `frames`, or with an entry outside (0, 1];
- `emission` or `temperatureEncoding` on an unlit manifest;
- an unknown `temperatureEncoding`;
- an emission ramp with no keys or more than 8, a key's `t` outside [0, 1] or not above the key
  before it, or a colour channel that is negative or not finite;
- an `ignition` outside [0, 1].
