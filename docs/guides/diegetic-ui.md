# Diegetic and glowing UI

This guide covers **world-space, glowing UI**: putting a `Veng::Gui` document *in the
world* — on a monitor, a hologram, a floating readout — so its bright content **blooms**
through the scene's own bloom. It builds on [authoring a UI
document](authoring-ui-documents.md) (the markup, stylesheet, and font are authored the
same way); the new pieces are the **`GuiSurface`** component, a **translucent or emissive
panel material**, and the **`rgb()` linear-float** color syntax that lets a color exceed
1.0.

The live reference is the [template](../../examples/template/) module: its glowing panel
is authored as prefab data — a `GuiSurface` beside a `MeshRenderer` in
[`assets/prefabs/scene.prefab.json`](../../examples/template/assets/prefabs/scene.prefab.json),
its panel material in
[`assets/materials/panel.vmat.json`](../../examples/template/assets/materials/panel.vmat.json)
+ [`assets/shaders/panel.frag.slang`](../../examples/template/assets/shaders/panel.frag.slang),
and its document + stylesheet in
[`assets/ui/panel.vui.xml`](../../examples/template/assets/ui/panel.vui.xml) /
[`assets/ui/panel.vuss`](../../examples/template/assets/ui/panel.vuss). Open them beside
this guide.

---

## Why world UI glows and screen UI does not

A `Veng::Gui` document is drawn to a texture; **where that texture is composited decides
whether it can glow.**

- A **screen-space overlay** (a document attached to a viewport, the HUD path) composites
  **after** the scene's tone mapping, in LDR. Tone mapping has already mapped the scene's
  HDR range down to displayable values, so an overlay color is clamped to white at 1.0 —
  it cannot be brighter than white, and there is nothing left to bloom. Screen UI is
  honest, un-glowing UI painted over the finished frame.
- A **world-space surface** (a `GuiSurface`, this guide) composites its document **into
  the lit HDR scene color, before bloom and tone mapping** — exactly where scene geometry
  lives. A document pixel brighter than 1.0 is now a genuine HDR emitter that the scene's
  existing bloom catches, and tone mapping maps it down alongside the rest of the scene.

So glow is a property of GUI **in the world**, the same way a real monitor emits its own
light. There is **no dedicated GUI bloom pass** — a glowing panel is ordinary scene
geometry, and the scene's mip-pyramid bloom blooms it for free.

## The three pieces

### 1. A `GuiSurface` component on a mesh entity

`GuiSurface` (`Veng/Gui/Surface.h`) is a reflected scene component that owns a live
`Gui::Document`, renders it into a **persistent HDR (`RGBA16Sfloat`) render target** each
frame, and binds that target's bindless handle onto the material of the mesh it shares an
entity with. It is authored as prefab data beside a `MeshRenderer`:

```json
{
  "::Veng::MeshRenderer": {
    "Source": { "type": "::Veng::PlaneShape",
                "value": { "Size": [1.7, 0.96], "Material": "0x…panel-instance" } }
  },
  "::Veng::GuiSurface": {
    "Document": "0x…panel-document",
    "Resolution": [512, 288],
    "Domain": "Translucent"
  }
}
```

- `Document` is the cooked `UIDocument` recipe; the surface instantiates its own live tree
  from it on first drive (or a document built imperatively in C++ is injected through
  `SetDocument`).
- `Resolution` is the HDR target size in pixels **and** the extent the document lays out
  against — pick it for the panel's aspect and text sharpness.
- `Domain` selects how the document texture becomes scene light (below).

The engine drives every `GuiSurface` in a viewport's bound scene into its target **ahead
of** the scene render, so a data-authored panel needs **no per-frame game code**. Its
document data-binds like any other (`{obj.field}`), dirty-gated so a static panel repaints
nothing. A plane mesh works because the document maps across the mesh's UVs; any mesh
does.

### 2. A panel material — translucent (default) or opaque-emissive

The material domain is the placement decision. A `GuiSurface` binds the document target
onto the sibling `MeshRenderer`'s first material each frame:

- **`Translucent` (default).** A `Translucent`-domain material (`Veng/translucent.slang`)
  whose fragment samples the document target through a `Texture` / `Sampler` handle pair
  and **returns it as the surface's radiance**. Transparent document regions show the
  scene behind (the surface writes no depth), and bright regions glow. Author `"cull":
  "None"` so a single quad is visible from either side:

  ```json
  {
    "defaultInstance": "0x…panel-instance",
    "domain": "Translucent",
    "cull": "None",
    "shaders": { "vertex": "0x69BE03796E97148D", "fragment": "0x…panel-frag" },
    "fields": [
      { "name": "Texture", "type": "texture", "id": "0x…any-placeholder-texture" },
      { "name": "Sampler", "type": "sampler", "texture": "Texture" }
    ]
  }
  ```

  The `Texture` default is a placeholder — `GuiSurface` overrides it with the document
  handle every frame. The fragment un-premultiplies the sampled texel (`rgb / a`) because
  the Gui target is premultiplied alpha while the Translucent domain blends straight alpha
  (see the template's `panel.frag.slang`). The vertex shader is the core
  `surface.vert` (`0x69BE03796E97148D`).

- **`OpaqueEmissive`.** For a solid monitor that **occludes** what is behind it, bind the
  same document handle onto an opaque `Surface`-domain material's `EmissiveTexture` instead
  (`GuiSurface` seeds `EmissiveColor` to white so the document value passes through
  unmodulated). The bezel is a lit g-buffer surface; the document drives the additive
  emissive term. The panel writes depth and blocks the scene behind it.

Both composite pre-bloom, so both glow.

### 3. `rgb()` colors above 1.0

Screen UI never needed colors past 1.0 (they would clamp), so hex `#rrggbb` — sRGB,
`0`–`255` — was enough. A glowing panel needs HDR, so the cooker accepts an
**`rgb()` / `rgba()`** syntax whose components are **linear floats** and may exceed 1.0:

```css
.value {
    font-size: 96px;
    color: rgb(0.4, 6, 7);          /* glowing cyan — a genuine HDR emitter */
}

.panel {
    background: rgba(0.01, 0.03, 0.05, 0.82);   /* dark, translucent backing */
}
```

`rgb()` is **linear**, unlike sRGB hex — `rgb(0.5, 0.5, 0.5)` is **not** `#808080`. The
two coexist: hex for familiar LDR hues, `rgb()` for linear and HDR values. The gradient
ramp is HDR too, so a gradient stop can glow. Magnitude *is* the glow: a pixel's
brightness is its emitted radiance directly, and a black or transparent background emits
nothing.

## The one gotcha: hot cores desaturate

A bright, saturated emissive color does **not** keep its hue at its center. The scene tone
mapper compresses the HDR range, and a value far above 1.0 tone-maps toward white — so a
saturated hot color like `rgb(0, 8, 8)` reads **white-hot at the core** with a **colored
bloom halo** spreading around it. This is not a bug; it is the physically-expected look of
a bright emitter (a neon tube, an overdriven display) — the core saturates the sensor while
the bloom carries the hue. If you want a panel that stays its authored hue, keep its
brightest color nearer 1.0; push it well past 1.0 only when you want the hot-emitter look.

## Verifying it

The engine drives the surface automatically, so there is nothing to call each frame. Build
and run the world; the panel renders its document onto the mesh and, with the scene's
`Bloom` enabled (a level render setting), its bright text blooms. In the editor, a
`GuiSurface` is inspectable like any component — its `Document`, `Resolution`, `Domain`,
and `Seat` show in the reflection inspector, and the referenced document opens in the
`UIDocumentEditorPanel`.

## Making a world panel interactive

A `GuiSurface` is display-only by default: it data-binds and draws but does not hit-test.
To make an in-world panel *pressable*, give it a **`Seat`** (the `Viewer` entity whose
devices drive it — a world panel has no host viewport to inherit one from) and route a
world ray into it through `Veng/Gui/SurfaceInput.h` (`RouteSurfacePointer` /
`SurfaceInputConsumer`), which intersects the panel, maps the hit UV to a document
coordinate, and dispatches a pointer event through the same capture→bubble pipeline a
screen-space pointer uses — under a `SeatFocusScope`, exactly as a screen menu opens
interactivity. The ray→coordinate mapping is independent of the material domain: a
translucent hologram and an opaque monitor are pressed the same way.
