# Veng::Gui — the retained, data-driven game UI

`Veng::Gui` (`engine/include/Veng/Gui/`, in `libveng`) is the game-facing retained UI subsystem —
the HUDs, menus, and panels a shipped game authors as **content** and drives from game state. This
doc covers the runtime: the cooked assets, the document/element tree, styling, drawing, viewport
hosting, binding, input, and the widget library. Project-wide conventions are in
[the root CLAUDE.md](../../../CLAUDE.md), the runtime overview in [engine/CLAUDE.md](../../CLAUDE.md),
the cook-side parsing in [cooker/CLAUDE.md](../../../cooker/CLAUDE.md), and the immediate-mode
sibling `Veng::UI` in [../UI/CLAUDE.md](../UI/CLAUDE.md).

## The two UI layers

`Veng::Gui` is **distinct from and orthogonal to `Veng::UI`**: `Veng::UI` is the immediate-mode
ImGui vocabulary for **debug panels and the editor**, authored in C++ at the call site and
re-issued every frame; `Veng::Gui` is a **retained tree** of persistent widgets with focus,
selection, transitions, and reflection bindings, authored by a designer as a `*.vui.xml` markup
asset plus a `*.vuss` stylesheet. The two coexist deliberately — a game HUD is content, not a
sequence of function calls. **The boundary:** `Veng::UI` never draws a game HUD and `Veng::Gui`
never draws an editor panel — a symbol from one layer does not appear in the other's call sites.

## Cook the markup; the runtime never parses it

The keystone split, the shader/material split applied a third time: the cooker parses `*.vui.xml` +
`*.vuss` (pugixml + a CSS tokenizer, both cooker-only) into a **binary element tree + flattened,
resolved style tables**; the runtime loads that cooked blob and does three things — **layout →
style resolution → draw**. XML and CSS never enter `libveng`, so the runtime stays small and the
editor's existing cook-on-demand hot-reload serves UI with no new machinery. The parsing side is in
[cooker/CLAUDE.md](../../../cooker/CLAUDE.md); this doc is the runtime.

## The cooked assets

`AssetTypes::Font` (`Veng/Asset/Font.h`) is an **MSDF glyph atlas** (a bindless atlas texture + a
CPU metrics table — advance, bearing, atlas rect, kerning) cooked from a `*.font.json` naming a
TTF/OTF; the runtime decodes nothing and shapes crisp text at any scale from the one small atlas.

`AssetTypes::StyleSheet` (`Veng/Gui/StyleSheet.h`) is a reusable cooked stylesheet — **resolved**
rules (type/class/id selectors matched at cook time) plus their
`:hover`/`:active`/`:focus`/`:disabled`/`:checked`/`:selected` state variants, colors resolved sRGB→linear, a
**transition table** (each `transition` declaration owning a contiguous run the resolve copies onto
the element as its ease list), and
a **gradient table** (each `background-gradient` baked at cook time to a shape + box-space geometry
— linear endpoints `P0`/`P1`, elliptical radial radii, conic center + turn — plus an N×1 ramp LUT
the instantiate resolve uploads through the borrowed AssetManager). At draw the gradient geometry
rides a per-frame **`GpuGradient` storage buffer** (`GuiScenePass` rings it and binds it bindless);
the vertex carries only the record index, so many gradients batch into one run and a game animates
a gradient by mutating its geometry per frame (`Document::SetBackgroundGradient`). A cooked sheet
also carries a **typed variable table** — the sheet's own file-scope `--name` tokens whose value
flattens to a color or a single number — queried at runtime by `StyleSheet::FindVariableColor` /
`FindVariableScalar` (names stored without the leading `--`), so imperative code reads the same
palette the rules were flattened from rather than restating it. Token substitution itself
(`var(--name)`, `@use "sheet.vuss"` variable import, last-wins redefinition, define-before-use) is
a pure cook-time transform ahead of the flatten — the runtime sees no `var()`; see
[cooker/CLAUDE.md](../../../cooker/CLAUDE.md).

`AssetTypes::UIDocument` (`Veng/Gui/UIDocument.h`) is the cooked markup: a **pre-order recipe
element tree** (each element carrying its kind, id, classes, text, inline style, unresolved
`{obj.field}` bindings, and named handlers) plus the `StyleSheet` handles it references and its
font/texture dependencies (kept resident). All three load through the ordinary
`AssetManager::Load`/`LoadSync` path ([../Asset/CLAUDE.md](../Asset/CLAUDE.md)); a `UIDocument`
eager-loads its stylesheet + font dependencies exactly as a `Material` eager-loads its textures.

## Document instances — the `Prefab` model

A `UIDocument` is a recipe; a `Gui::Document` is an instance.
`Gui::Document::Instantiate(const UIDocument&, AssetManager&)` materializes an **independent** live
tree from the cooked recipe (cascading the referenced stylesheets onto each element, inline style
winning), so instantiating one recipe twice yields two trees that mutate separately — two
split-screen HUDs are two instances over one cooked blob. The document resolves its asset
declarations through the borrowed `AssetManager` **directly** — a font declaration's `AssetId`
loads to an atlas via `LoadSync<Font>`, and an `Image`'s `src` `AssetId` to its texture, each a
cache lookup on the already-resident dependency; the resolve re-runs on later style resolves, so
the manager must outlive the document. (The id→handle resolution is a plain `AssetManager*`
threaded through; gradients resolve internally at `Instantiate`.) A `Document` can also be built
and mutated **imperatively** in C++ (`Add`/`Remove`/`SetText`/`SetStyle`/…) — the same retained
tree, two authoring modes.

## The retained tree, Yoga, and the per-frame pipeline

A `Gui::Document` single-owns a tree of `Gui::Element`s (`Veng/Gui/Element.h` — a kind, a resolved
`Style`, a computed `Layout` rect, interaction state, children) mirrored into a **Yoga** flexbox
node tree. Yoga (vendored MIT, `libveng`) is the **one** runtime layout step — the deliberate
exception to "the runtime does no layout math," because layout depends on the resolved size each
frame (window size, dynamic list contents, text reflow) and so cannot be baked; the cooker emits
flex *properties*, the runtime *solves* them. **Yoga's C headers are confined to `.cpp`** (the
Native idiom — no third-party type in a public `Veng/Gui/` header), guarded by `include_hygiene`
exactly as the Slang/assimp gate is. The per-frame pipeline is `Update(delta)` (re-select active
style variants over the base style, advance property transitions) → `Solve(available)` (push each
element's layout inputs into the Yoga mirror, run the flex solve at the available extent, read each
computed rect back into `Element::Layout`) → `Build(DrawList&)` (walk the laid-out tree, emit
background/border/text/image/widget primitives, clip-pushed where an element clips). A clean
`Solve` at an unchanged extent is a no-op.

## One box model: the element's rect is its border box

**`Element::Layout` is the element's border box.** Margin lies outside it; **border and padding lie
inside it and are both reserved by the layout solve** (`ApplyStyle` sets the Yoga border edge beside
the padding one, from the single uniform `Border::Width`); content — and a measured leaf's own
shaped or intrinsic size — lives in the content box, which is `Layout` deflated by border +
padding. There is no `box-sizing` choice and no per-edge border width.

Three consequences, and they are what the paint path was always written against:

- **An authored `width`/`height` is the outer extent.** A `64×48` box with a `4px` border still
  solves to `64×48`; its frame comes out of the inside and a stretched child fills the remaining
  `56×40`.
- **An auto-sized element includes its own frame.** A text leaf measuring `20×20` inside `6px` of
  padding and a `3px` border solves to `38×38`.
- **A measure function is handed the box its content is drawn in** — when it is handed one at all.
  A `100px`-wide **wrapping** leaf with `6px` padding and a `3px` border wraps its text at `82px`,
  so the last glyphs do not clip. A non-wrapping leaf is offered no width, because there is
  nothing for it to wrap within.

**Wrapping is opt-in (`text-wrap`), and `nowrap` is the default — because the measure and the paint
must shape the same run.** `Document::MeasureElementText` and `DrawList::Text` both take a wrap
width, and both read it from `Style::Wrapping`: a `wrap` element hands its content width to each, a
`nowrap` element hands neither one. When the two disagreed the failure was silent and looked like
nothing to do with text — a label wider than its column measured two lines tall, solved a box twice
the height it needed, and then painted **one** unwrapped line at that box's top. Under
`align-items: center` the over-tall box is centred while the single line sits at its top edge, so
the text drew outside the row that owned it and no longer lined up with that row's own background.
The box was wrong, not the glyphs. Defaulting to `nowrap` makes the measure agree with what the
paint has always done, so an over-long run overflows horizontally — where `overflow: hidden` can
catch it — instead of mis-sizing its box.

**So every paint-side deduction is correct, not a double count.** `ToPaddingBox` deflates `Layout`
by the border; `ToContentBox` by border + padding; the text origin, the text-alignment slack, and
the `TextInput` line box inset by the same amounts. All of them read the width through one clamp
(`BorderWidth`), so a **negative** authored `border-width` reserves nothing, paints no ring, and
deflates nothing — layout and paint agree on a malformed value rather than diverging.

## Styling

**Typography inherits; nothing else does.** An element shapes and paints its text through the
`font` its own style declares, and otherwise through the nearest ancestor that declares one — so a
document names its font once on the root and every text-bearing descendant (a `Text`, a `Button`
label, a `TextInput`'s value and caret) resolves it, at measure and at paint alike. A font declared
lower overrides it for that subtree. Every other property resolves per element with no inheritance.

Styling flattens at cook time; the runtime interpolates among variants. The cooker matches the
USS-like selectors (type / class / id / pseudo-state, no full-CSS specificity cascade) and emits
each element's resolved base style plus a handful of state variants. The runtime never runs a
selector engine — `Update` selects the variants whose state bit is set in the element's live
interaction mask, folds them over the base, and **eases** any transition-able property (colors,
opacity, scalar sizes) over its authored duration through a small tween clock. A style change that
moves a layout input re-dirties the Yoga box; a pure paint change (color/opacity) does not.

**The ease durations are one per-element list, reached two ways.** A rule's
`transition: <property> <duration>[, …]` cooks into the sheet's transition table and the
instantiate-time resolve copies its slice onto the element; `Document::SetTransitions` writes the
same `vector<StyleTransition>` imperatively, for a duration a game computes at runtime. So the
declarative path is a default, not a replacement, and both drive one tween clock. The cook rejects
an unknown property name and one that does not interpolate (`Gui::IsAnimatableProperty` is the
shared predicate the tween clock and the cooker both read), and a later rule's `transition`
replaces an earlier one's **whole** list — the ordinary cascade, not a per-entry merge.

**A background is one fill source, never a stack.** `background-material` > `background-gradient` >
`background-image` > `background`, **exclusive**: the winning source *is* the fill and they do not
layer (CSS-style image-over-color compositing is out). The rule costs nothing to enforce because it
is what the code already shapes itself as at both ends — `Document::Build`'s single `else if` chain
picks one source per element, and within the shape path `gui_shape.frag.slang` is
`if (selector > 0) … else if (textureIndex >= 0) …`. So a single rule authoring two sources is a
**cook error** (`CheckExclusiveFillSources`) rather than a silently-ignored declaration. An
*unresolved* source is not a conflict: it falls through to the next one, so a texture that failed to
load leaves the flat color painting rather than a hole.

Each source rides the identical silhouette — the rounded-rect SDF with its corner radius, border
ring, clip, rotation, and composited opacity — which is the whole reason the set could grow without
touching the shape path. The three that follow are the same idea at three distances from the engine:
a **texture** (plain, tiled, or nine-sliced), the `Image` widget's content fill in that same
vocabulary, and an authored **material** whose fragment computes the RGBA.

**`background-image` is a texture fill on the same shape path.** It names a `Texture` `AssetId`
transported exactly as `font`'s is (`CookedStyleProperty::Handle`), resolved at instantiate to a
resident `AssetHandle<Texture>` held for the `Style`'s lifetime. Because a background image may be
authored in a **stylesheet rule** *or* an **inline style**, the texture is collected as a load-time
dependency in **both** loaders (`StyleSheetLoader`'s `TextureIds` and `UIDocumentLoader`'s, beside
the `Image` `src` ids it already gathered) — the instantiate-time resolve is then a cache hit.
The fill sizes against the **padding box**, so it sits behind the border and the content, and its
corner radii are the element's reduced by the border width (the CSS inner-radius rule). Three
shapes, driven by `background-slice` / `background-fit` / `background-repeat`:

- **Sliced** (`background-slice` non-zero, in source-texture pixels) → `DrawList::NineSlice`: the
  corners keep their source size while edges and center stretch — or **repeat**, when
  `background-repeat: tile` is authored beside the slice (see below). The sliced path is
  **unrounded** — the primitive takes no radii, which matches nine-slice art carrying its own
  corners.
- **Tiled** (`background-repeat: tile`, unsliced) → **one** `DrawList::Texture` quad with the UV
  rect scaled by box ÷ texture size, repeated by the texture's own wrapping sampler. Never a quad
  per tile: the GUI geometry ring is a hard cap behind an unconditional `VE_ASSERT`, so an unbounded
  tile count would abort, and the clip such quads would need would break batching. A texture whose
  `*.tex.json` authors a clamp address mode therefore clamps rather than tiles.
- **Sliced *and* tiled** → `DrawList::NineSlice` again, with each stretchable cell repeating its own
  source sub-rect. A wrapping sampler cannot express this: it wraps at the **whole texture's**
  bounds, so a UV run past a cell samples the neighbouring cell. The cell's sub-rect therefore rides
  the vertex — `GuiVertex::UvWrap` (`xy` the cell's UV min, `zw` its size, **a zero size meaning the
  lane is inactive**) — and the fragment wraps arithmetically,
  `uvMin + frac((uv - uvMin) / uvSize) * uvSize`, sampling with `SampleGrad` off the *unwrapped*
  UV's derivatives so `frac()`'s seam does not collapse a mipped texture to its smallest level along
  a one-pixel line. Which cells repeat follows from the slice: the four **corners never do** (they
  are fixed-size by definition), each edge repeats **along its growing axis only**, and the centre on
  both; the count is `cell destination ÷ cell source`, derived exactly as the unsliced path derives
  its own. **The asymmetry is deliberate:** the texture's address mode stays load-bearing unsliced
  and stops being so sliced, since the sliced fragment no longer asks the sampler to wrap.

  **Tiling a sliced fill only pays when the stretchable cells have something to repeat**, and two
  common frames have nothing. A frame whose slice insets sum to the whole texture on an axis leaves
  its edge and centre cells with **zero source extent** — there are no pixels to repeat, so the
  engine keeps stretching that axis rather than dividing by zero. And a frame whose stretchable
  middle is a **smooth ramp or a flat region** carries no motif: tiling it introduces a hard
  discontinuity at every copy, which is strictly worse than the stretch that ramp was drawn for. The
  feature is for edges and centres that carry a **repeating motif at non-zero source extent** — a
  hatch, a rivet run, a scanline. Reach for it on that evidence, not because the fill is sliced.
- **Fitted** (the default) → `DrawList::Texture` with the UV sub-rect and destination `ImageFit`
  computes: `fill` stretches, `contain`/`cover` letterbox/crop preserving aspect, `none` is
  intrinsic pixels. `ImageFit`/`ImageRepeat` (`Veng/Gui/Style.h`) are the shared fill vocabulary.

### `box-shadow` — an effect on the same silhouette

**A shadow is the shape's own SDF read with a wide coverage ramp.** `box-shadow`
(`<offset-x> <offset-y> [blur] [spread] [color] [inset]`, or `none`) emits **one extra quad** on
the unchanged `Shape` pipeline: the blur is the half-width of the coverage ramp in pixels, and
spread grows the silhouette (radius included, so a shadow follows `corner-radius`). Offset and
spread are applied **in the fragment**, against the element's own `RectHalf`/`RectCoord`, rather
than baked into the quad — which is what keeps an inset shadow exact inside a rounded corner,
where a scissor could only bound it to the square box.

- **Transport is a `GuiVertex` field of its own** (`vec4 Shadow`: signed blur, spread, offset),
  beside `Params` the way `GradientSelector` is — `Params`'s four lanes are fully occupied. A
  **zero blur lane means the quad is not a shadow**, a positive one a drop shadow, a negative one
  an inset shadow: the sign is the inset flag's only transport, which is why a hard-edged shadow
  still carries a tiny non-zero blur the fragment widens to the anti-aliasing width.
- **Geometry differs, the pipeline does not.** A drop shadow's quad is the box grown by spread +
  blur and slid by the offset (the SDF's outside-positive region has no fragments to shade
  otherwise); an inset shadow's quad is the box itself. Both are untextured, so they batch into
  the runs around them.
- **A drop shadow paints before the fill and an inset shadow after it**, both inside the
  element's own draw — so a shadow multiplies the inherited opacity, rotates with the transform
  stack, and is clipped by the enclosing scissor like every other primitive. The consequence to
  author around: **an outer shadow is clipped by an ancestor's `overflow: hidden`**.
- **One shadow per element** (no comma-separated list), and no separate blur pass — the
  smoothstep-over-SDF approximation is the whole mechanism. Text shadows are absent; the MSDF
  path is untouched.

### Material fills: an authored fill source

**A material is a fill source, never a silhouette.** `background-material: <material id>` on any
container, and `material: <material id>` on an `Image`, name a resident **`MaterialDomain::GuiFill`**
material whose fragment emits the RGBA *inside* the shape; the engine's rounded-rect SDF coverage,
border ring, composited opacity, clip, and rotation all multiply into it exactly as they do a flat
color. So a shader-driven fill composes with `corner-radius`, `border-width`, `overflow: hidden`,
and `rotation` for free, and the material cannot widen, replace, or alpha-blur the silhouette. (A
"full custom shader" domain flag that skips the coverage multiply is deliberately not built.)

- **`background-material` is the top of the exclusive fill-source order** — above
  `background-gradient`, `background-image`, and `background`. When set, the material *is* the fill;
  a rule authoring it beside another fill source is the same **cook error** the rest of the order
  raises. It may still sample a texture, as a declared parameter of its own.
- **Authoring is three files and an id.** A `*.slang` fragment (or a `.graph.json` with
  `"domain": "GuiFill"`) `#include`s the core-pack `Veng/guifill.slang`, declares its own
  `MaterialParams`, and returns `GuiFillResolve(input, fill)`; a `*.vmat.json` carries
  `"domain": "GuiFill"`, names the **core gui vertex stage** (`0x23896E307C8108E6`) as its vertex
  shader, and declares a `defaultInstance` id; the style names **that instance id**. The three
  domain-only source nodes a graph gets — `GuiBoxCoord`, `GuiUV`, `GuiTime` — are the same three
  values a hand-written fragment reads off `GuiFillInputs` and `g_PC`.
- **An `Image`'s `material` shades its art rather than replacing it.** The element's `src` texture
  and sampler are written **once at resolve** into the two conventional runtime-bound handle fields
  — `Image` and `ImageSampler` (`Gui::ImageMaterialTextureField` / `ImageMaterialSamplerField`) —
  when the material declares them, so the shader samples exactly what the plain fill would have
  drawn. A material declaring neither shades procedurally. The fill covers the **content** box, like
  every other `Image` fill.
- **Animation rides the pass clock, not a parameter write.** `GuiScenePass::SetTime` is the domain's
  only per-frame channel (`float Time` at byte 16 of the GUI push block, read as `g_PC.Time`); the
  viewport and the `GuiSurface` texture path each accumulate the frame delta into it. There is no
  general per-frame material-parameter channel here, and a `GuiSurface` carrying a material fill
  drops out of the dirty-gate and re-records every drive. Because the clock is *supplied*, a capture
  that never advances it renders reproducibly — which is what makes `gui_material.png` a golden.
- **Residency is a load-time dependency in both loaders**, exactly as `background-image`'s texture
  is: `StyleSheetLoader`'s `MaterialIds` for a rule and `UIDocumentLoader`'s for an inline style, so
  the instantiate-time resolve is a cache hit.
- **A material fills the whole shape, and the border is drawn over it.** `GuiFillResolve`'s ring
  branch keys off the *quad's* border lane, which is the emission-side flag meaning "this quad is
  the border ring" — a bordered element is two quads, the fill and the ring, exactly as a flat or
  gradient background is. `Document::Build` passes an empty `Border` for both material fill sites,
  so a material always paints the full silhouette and the border quad then covers its outermost
  `border-width` pixels, as it covers any other fill. **A material *ring* is therefore shaped in the
  fragment's own alpha**, not by setting `border-width` — the border is a separate opaque quad, not
  a window onto the fill.
- **The cost is batching.** A material is part of the run key, so N distinct materials are ≥ N runs —
  the same trade a distinct texture already forces, and the reason the pass's rebind guard is keyed
  on `{kind, material instance}` rather than the run-kind enum (two adjacent material runs would
  otherwise draw the second's geometry with the first's pipeline). Adjacent fills sharing one
  material still merge. `GuiScenePass` caches one built pipeline **per parent material** (the
  pipeline depends on its layout and modules, not on an instance's parameter block), each entry
  pinning that material's shader modules resident — bounded by the resident material set of the
  documents the pass draws.

### The `Image` widget's fill

**One fill vocabulary, two hosts.** The same three shapes drive the `Image` widget's own content
through `object-fit` / `image-repeat` / `image-slice` — the widget-side spellings of
`background-fit` / `background-repeat` / `background-slice`, over the identical
`ImageFit`/`ImageRepeat`/`Insets` types. Two things differ from a background fill:

- **Which box.** An `Image` fills its **content** box (inside the border *and* the padding, the box
  a `Text` leaf's run draws in), where a `background-image` fills the padding box.
- **Intrinsic size.** An `Image` is a **measured leaf** like `Text`/`Button`/`TextInput`: its
  Yoga measure returns the resident texture's own pixels (`Element::ImageSize`, filled by the
  instantiate-time resolve), so an `Image` with no authored `width`/`height` lays out at natural
  scale and flexes like any other measured content instead of collapsing. An authored size still
  wins, and `object-fit` decides the mapping when the box differs. A **sliced** `Image` measures the
  sum of its corner insets instead — the smallest box at which the frame still reads. An unresolved
  texture measures zero. Taking a child turns an `Image` into a container, exactly as it does a
  `Button` (a measured Yoga node cannot hold children).
- **Slicing and tiling compose on either host.** `image-repeat: tile` (like `background-repeat:
  tile`) beside a slice repeats each cell within its own source sub-rect, through the per-quad
  `UvWrap` lane described above — the corners hold their size, the edges repeat along the axis they
  grow on, the centre on both. Nothing about it is widget-specific; the two hosts differ only in
  which box they fill.

**The measure reads the whole texture, never the `uv` sub-rect.** Reading `ImageUv` there would make
`Document::SetImageUv` a layout input, turning a per-frame atlas flipbook advance into a per-frame
layout re-solve; the setter keeps its paint-only, no-dirty contract. *Fit* and *slice*, by contrast,
are computed against the **sampled sub-rect**, so a flipbook frame fits and slices its own cell —
which is why `DrawList::NineSlice` takes the sub-rect its 3×3 split divides. *Unsliced* tiling
repeats the whole texture, since that is what the sampler's wrap addresses; sliced tiling repeats
each cell of the sub-rect, since the fragment wraps it arithmetically.

## The draw floor: a device-free draw list + a `GuiScenePass`

`Gui::DrawList` (`Veng/Gui/DrawList.h`) is a device-free builder of **batched, clipped, textured
quads** — rounded-rect / border SDF, 9-slice, tint/opacity, and MSDF text runs — that
`Document::Build` appends into. A `GuiScenePass` records the draw list into an offscreen image
blended over the viewport's scene output. Its two fixed pipelines (the rounded-rect shape path and
the MSDF text path) are built from **core-pack** Slang shaders a consumer reuses rather than
authors; the **third** run kind, `GuiPipeline::Material`, is the seam where a consumer *does*
author a fragment — an authored `GuiFill` material, drawn on the same vertex stage and multiplied
by the same silhouette (see [Material fills](#material-fills-an-authored-fill-source) above). The
image goldens are the render floor every later change holds pixel-stable against: one **per feature**
(`gui_overlay`, `gui_rotated`, `gui_image`, `gui_background`, `gui_sliced_tile`, `gui_shadow`,
`gui_material`, `gui_popup`), kept separate on purpose so a moved pixel names the feature that moved it, plus **two**
**composition** captures for what only shows when two of them meet: `gui_composition` — a nine-slice
frame around a tiled `Image`, a material fill inside a clipped scroller, and a shadowed card under an
open popup — and `gui_box_composition`, where tiled nine-slice frames wrap bordered boxes whose size
comes from a measure, so the cells' repeat counts are decided by the box model rather than by an
authored extent. **The vertex format is five files, not one**: the
`GuiVertex` struct, the cooked `gui.vlayout.json` the pass loads, and the `VSInput`/`VSOutput` of
`gui.vert.slang` — the shader importer hard-errors at cook time on a reflected-vs-declared
mismatch, so a new field lands in all of them at once or nothing cooks. **A fragment declares only
the interpolants it reads**, and the vertex stage may output more: semantics bind the two, not
member order or count, so `gui_msdf.frag.slang`'s `VSOutput` and the `GuiFillInputs` a material
reads both omit the **three** lanes they have no use for — the gradient selector, the shadow, and
the per-cell UV wrap. That is the established shape here, not an oversight — the validation gate
accepts the unread output (it logs the SPIR-V interface mismatch at `WARN`), and a fragment that
*does* read a lane must declare it at the matching semantic. The convention is stated on
`GuiFillInputs` itself, so a consumer authoring a fill meets it where it applies.
`DrawList` carries a composing **transform stack** (`PushTransform(pivot, angle)` / `PopTransform`)
applied to vertex positions at quad emission while `RectCoord`/`RectHalf`/UV stay in unrotated
local space, so a `rotation` style property (scalar degrees, clockwise in the y-down document
space, animatable by transitions and keyframes) turns an element's whole subtree rigidly about its
`Origin` anchor — the SDF, borders, gradients, textures, and MSDF glyphs rotate with it and
batching is unaffected. Rotation is **paint-only**: hit-testing stays axis-aligned against the
unrotated `Layout` rect and scissor clips stay axis-aligned; `Document::SetRotation` writes it per
frame with no layout re-solve.

## Viewport hosting & layers

A document is content a `Viewport` hosts, in layers, engine-driven ([../Renderer/CLAUDE.md](../Renderer/CLAUDE.md)
for the viewport and render tail). `Viewport::AttachDocument(doc, layer)` hosts an ordered,
**non-owning** layer stack of `Gui::Document` instances (bottom→top: HUD, menu, notifications — the
`Application` viewport-list idiom, so the owner keeps the `Unique` and dropping it self-detaches
through a stored back-reference). The engine drives every attached document's per-frame pipeline
(`Update` → `Solve` at the region extent → `Build`) inside `Viewport::Render` and composites the
layers through the viewport's `GuiScenePass` over the scene output, ahead of the managed gather +
composite tail. **A game resolves its `{obj.field}` bindings** each frame with
`Document::UpdateBindings()` — the engine drives layout and draw, but the binding re-read is the
game's per-frame call (it is a no-op when the bound context's version is unchanged). A document
attached to a **null-World** viewport does not composite — `Viewport::Render` short-circuits before
the document blend when there is no scene to render over — so a UI-only viewport pushes an empty
`Scene` (a cleared target the document composites onto).

## Binding & handlers

Binding and handlers reuse reflection ([../Reflection/CLAUDE.md](../Reflection/CLAUDE.md)). A
document binds one game-owned `Gui::BindingContext` (`Veng/Gui/BindingContext.h`) — a reflected
**data object** (a game struct by base pointer + its registered `TypeId`) plus a table of named
**handlers** — through `Document::BindContext(context, registry)`. A `{obj.field}` binding resolves
its dotted field path against the data object through the same `TypeRegistry` the inspector and
serializer use (one reflection path shared between editor and game UI); the context bumps a
**version** on a bound-field change (`Invalidate`) so `UpdateBindings` re-reads only the dirtied
bindings. A markup `onClick="Name"` names an entry in the handler table, fired by the event path
with the element that raised it.

## Input & focus

Input is per-seat, and documents are display-only by default. `Veng::Gui` registers as a consumer
in the input router's registry (`Veng/Gui/GuiConsumer.h`); it hit-tests the laid-out tree, routes
pointer events with **capture → target → bubble** propagation (enter/leave/down/up/click, text
input), and drives **keyboard and gamepad directional focus navigation** (plus confirm/cancel
activation, a focus ring drawn as the `:focus` variant) — all scoped to the **seat** the document
inherits from its host viewport (`Viewport::GetSeat`, the `Viewer`-entity seat identity — see
[../Scene/CLAUDE.md](../Scene/CLAUDE.md)), so seat A's menu leaves seat B playing. A document is
**display-only by default** (`IsInteractive() == false`): its bindings update and it draws, but it
hit-tests and takes no focus until the game opens interactivity on it. A game makes a menu
interactive by opening a **`SeatFocusScope`** (`Veng/Input/SeatFocusScope.h` — the RAII takeover: a
token UI focus entry on the seat's stack + the seat's `InputContextStack` swap + the viewport↔seat
association, restored in inverse order on destruction) and flipping `SetInteractive(true)`; the
input consumer then routes that seat's devices into the document. The takeover every game screen
otherwise hand-rolls is one engine seam.

**A focused text field claims the editing keys before focus navigation sees them.** Backspace,
Delete, the arrows and Home/End produce no character, so they reach a field only as key presses:
the consumer maps each to a `TextEditAction` and offers it to `Document::DispatchTextEdit` first.
Only a focused `TextInput` consumes one, so Left/Right move its caret while it holds focus and fall
through to directional focus navigation when any other element (or nothing) does — the precedence
is decided by what holds focus, not by the key. A caret move that is already clamped at either end
still consumes the key, so an arrow never leaks out of a field and moves focus instead. The caret
indexes **codepoints**, so a move steps one whole glyph and a delete removes one whole glyph of the
UTF-8 value. There is no selection anchor: every action addresses the caret or the codepoint
adjacent to it.

## Popups — the within-document layer that escapes every clip

**A popup is a subtree the document owns but does not parent under `Root()`.** An
absolute-positioned child leaves flow but stays inside its ancestors' scissor, so a menu opened
from a row inside a scrolling container is still clipped to that container and still painted under
its later siblings. The **popup stack** lifts both: a popup root lays out against the **document
extent** rather than a parent box, is built **after the whole main tree with the clip and
transform stacks reset**, and **hit-tests ahead of it**, top-down.

- **Opening.** `Document::OpenPopup(anchor, PopupOptions) -> PopupId` pushes an empty `Panel` root
  the caller fills through `GetPopupRoot(id)` with the ordinary `Add`/`SetStyle`/`InitWidget` calls
  — so a popup cascades, animates, scrolls, and takes any fill or shadow exactly as a parented
  subtree does. `ClosePopup(id)` / `CloseAllPopups()` dismiss; `IsPopupOpen` / `GetPopupCount` /
  `GetTopPopup` query. Because a popup root has no ancestor, the typography that would have
  inherited is **seeded from the anchor's resolved font** at open.
- **Placement is `Gui::Placement`, not a new vocabulary.** Each `Solve` runs after the main tree's
  layout read (the placement needs the anchor's solved rect), sizes the root **unconstrained** so
  it takes its content's extent — `max-height` + `overflow-y: scroll` is how an author caps a long
  menu — hangs it off the `PopupSide` edge of the anchor's box, adds `Offset`, and runs
  `AnchorBeside`/`ClampIntoBounds` against the extent with `Margin`.
- **The anchor is an `ElementHandle`, never an `Element*`.** Elements are not address-stable: a
  `List`/`Table` repeater destroys whole item subtrees whenever its bound array shrinks, and the
  allocator re-uses the storage. So an element carries a monotonic, never-re-issued
  `Element::Serial` (`Document::GetHandle` / `Resolve`), and `DestroySubtree` / `DetachTemplate`
  **close any popup anchored inside the subtree they free** — which makes "a popup closes with its
  anchor" a mechanism rather than a policy. The same hook drops the focus/hover/press targets
  naming a dying element.
- **Dismissal is LIFO and three-signalled.** Closing an entry closes everything above it (a menu
  takes its submenus), and focus returns to the element that held it before the chain opened. A
  **pointer press outside the top popup** light-dismisses it and is **consumed**, so a click-away
  never doubles as a click-through (`PopupOptions::LightDismiss` opts out). **`Cancel`** (Esc /
  gamepad B) closes the top popup before it reaches the focused element's `onCancel`. And focus
  navigation is **scoped to the top popup** while one is open, so the stops behind a menu are no
  more reachable by keyboard than they are by pointer.
- **Popups belong to interactive documents.** `SetInteractive(false)` closes them, so a
  display-only HUD holds none. Because the layer sits inside `Document::HitTest`,
  `Viewport::IsPointerOverDocument` counts an open menu covering the content beneath it with no
  extra wiring.

`gui_popup.png` is the render floor: a popup anchored inside an `overflow: hidden` panel, spilling
past that clip and over a banner the main tree painted after the panel.

## Widgets

The built-in, markup-authorable, styleable, focusable controls on the primitives: `Panel` (a styled
flex box), `Text` (a shaped MSDF leaf, sized by its own shaped run), `Image` (a textured box — a
`src` texture with an optional `tint`/`uv`, composing with `corner-radius`/border; it is the
**second measured leaf**, and it runs the background fill's vocabulary against its own content —
see [The `Image` widget's fill](#the-image-widgets-fill) below), `Button` (`onClick`),
`Checkbox` (`value`/`checked`/`onChange`, driving the `:checked` variant), `Slider`
(`min`/`max`/`step`/`value`/`onChange`), `ProgressBar` (a `[0,1]` fill), `TextInput`
(`value`/`onChange` — it **paints its own value**: the run draws vertically centred in its content
box in the style's `text-color`/`text-size`/font, clipped to the field's box, with a caret bar at
the edit position while it holds focus, so a bound `{value}` is visible with no companion `Text`
element. It is a **text-measured leaf** like `Text` and `Button`: it takes its intrinsic size from
the run it paints and holds **one line box open while empty**, so a field sizes itself with no
authored `min-height` and never clips the value it draws), `ScrollView` (a clipped, scrollable region), `List` (a data-bound repeater —
its authored children are an item template cloned once per element of a bound array, and with a
`selection` attribute a selectable list view; see below), and `Table`
(a column-aligning row container: each direct child is a row, and the k-th in-flow cell of every
row widens to the column's widest cell via a measured min-width between the Solve's two layout
passes; a flex-grow cell is an elastic filler that absorbs row slack instead of becoming a column,
right-anchoring the columns after it; with an `items` binding it repeats its row template exactly
as a List does). A numeric Table column pairs with the `text-align` Text style property
(`left`/`center`/`right`, a paint-only glyph alignment inside the solved box). Each is an
`ElementKind` the cooker recognizes and the widget layer gives behavior; a control's literal config
attributes (`min`/`max`/`step`/`value`/`checked`/`orientation`/`selection`) are read at `Instantiate` and its `{value}`
binding is one-way (the model drives the widget without firing `onChange`).

**`pointer-events` is three-valued, splitting "not me" from "not us".** `auto` hit-tests, `none`
makes the element *and its subtree* transparent, and `children` makes only the element itself
transparent while its descendants still hit-test. The third value is what makes a full-bleed HUD
root expressible: a backdrop that lays out its contents without claiming the pointer everywhere it
covers. The hit-test already walks children before self, so `children` only suppresses the self-hit;
`none` keeps its subtree prune, which is the cheaper form a decorative group wants. The enumerator is
**appended**, so `None` keeps its ordinal and no cooked blob version moves.

**A state reaches inside the element that carries it, and the selector grammar is why.** There is
no descendant combinator, so `.row:selected` cannot reach `.row .row-name`; a label with a `color`
of its own would keep it while the ground behind it inverted, and text colour does not inherit the
way the font does. So the *resolve* walks the chain instead — `EffectiveState` in `Document.cpp`
folds an ancestor's bits into the element's before variants are selected — and how far each bit
travels is the whole design:

- **`Selected` and `Disabled` are facts about a whole subtree.** An item host's selection covers the
  item, and a disabled container disables what it contains, so everything under the holder inherits,
  a nested control included.
- **`Hovered` and `Active` are facts about one path down it.** They reach only `pointer-events: none`
  content — a branch hit-testing prunes wholesale, so nothing in it could ever carry the bit on its
  own. A row spelled as a `Button` wrapping its own `Text` children is the case this exists for. A
  *reachable* sibling is left alone: it is hovered when the pointer is over it, and lighting it
  because the box around it is would be a different claim.
- **`Focused` and `Checked` do not travel.** A focus ring is a fact about the control, and a
  Checkbox's value is its own reading rather than a property of the words beside it.

`SetState` re-resolves the subtree on the spot rather than leaving it to the next `Update`, so a
ground and the text on it invert on the same frame and a transition eases from the right place.

**Hover is a property of a box, and every box the pointer is inside is hovered** — the element under
it and every ancestor containing it (CSS's own rule). Those are marked on `Element::State` by
`SetHovered`; the content reach above is the resolve's and is not marked a second time.

**The document reports the pointer state it already tracks.** `GetPointerPosition()` is where the
last routed event landed in document points, `IsPointerDown()` whether the primary button is held,
and `GetHoverTarget()` the element under it — the live hover the `:hover` variant follows, not a
fresh hit-test. Position and button follow the *transition*, not the target, so both track the
pointer across empty regions and a press that no element takes. That is what a consumer drawing its
own pointer reads: a game hiding the OS cursor to draw its own needs the pointer in the document's
coordinates, and the document is the one place that already has it.

**Gameplay asks whether the UI owns the pointer through `PointerRouting::OverUi`.** Consuming a
pointer *event* does not suppress a held-button *action* — a camera orbiting on a held mouse button
reads the action pipeline, which the Gui never sees — so the router publishes the answer instead:
`Viewport::IsPointerOverDocument` hit-tests the attached interactive documents and the routing
carries the result to `SystemContext::Pointer`. It is the retained-UI counterpart of the
immediate-mode `UI::WantCaptureMouse()`, and it is only meaningful because `pointer-events: children`
lets a backdrop decline the pointer it visually covers.

**Scrolling is a style property, and `ScrollView` is its preset.** `overflow-x` / `overflow-y`
(`visible` / `hidden` / `scroll`, with `overflow` as the CSS two-value shorthand) decide per axis
whether content is clipped and whether it scrolls — so a `List`, `Table`, or bare `Panel` styled
`overflow-y: scroll` scrolls with no wrapper element, and a vertical list styled `overflow-x: hidden`
cannot drift sideways when one row runs long. `ScrollView` remains as the named preset: it is a
`Panel` whose base style seeds `scroll` on both axes, so the cascade still lets an authored
`overflow-x: hidden` win. Every scroll behavior — the clip, the child origin shift, the drag capture,
the directional scroll, `ScrollIntoView` — reads the resolved style rather than the kind. The
property is mirrored onto the Yoga node (`YGOverflowScroll`), without which a flex child shrinks to
fit and nothing ever overflows to scroll.

**A scrollbar is real elements, so it styles through the ordinary cascade.** A scrollable axis owns a
widget-created `ScrollBar` carrying a `ScrollBarThumb`, tagged with a `horizontal`/`vertical` class.
They are `ElementKind`s rather than reserved class names, so `ScrollBar.vertical { width: 8px; }` and
`ScrollBarThumb:hover { … }` are plain type selectors with no new selector vocabulary — and the parts
carry their own background, corner radius, border, gradient, variants, and transitions. **A part
inherits its host's classes**, which is what makes it addressable per instance: the selector engine
matches one compound selector with **no descendant combinator**, so `ScrollBar.inventory` reaches a
particular list's bar where `.inventory ScrollBar` would not parse. They are
**not authorable**: the cooker's tag table does not accept them, so a `<ScrollBar>` tag is a cook
error.

**Because a part inherits its host's classes, a rule styling a scrollable element must name the
host's type.** `.inventory { overflow-y: scroll }` matches the bar that rule just asked for, not only
the list — a bare class selector cannot tell a host from its parts. `List.inventory { … }` can, and
is the form to write. The engine no longer lets the mistake hang the process (`SyncAllScrollBars`
skips scrollbar parts outright, since a bar is never itself a scroll container), but the part would
still take every other declaration in the rule, so the qualified selector is what expresses the
intent. Presence and visibility are separate: a bar *exists* while its axis is styled `scroll` and
*hides* when the axis has no travel, so content growing past the box reveals it with no structural
change. Dragging the thumb scales the pointer delta through the track's slack, and a press on the
track pages one viewport.

**A scrollable element captures its descendants' presses, and a click is still read off the element
the press landed on.** The capture is what lets a drag begun over a row pan the list rather than
work the row; taking the *click* from it as well would mean nothing inside a scroll container could
ever be clicked, since the release hit-tests the row while the capture holds the container and the
two never match. So the press records both — the claimant, which owns the drag, and its own hit
target, which owns the click — and each wears `:active`, so a row lights under the finger like any
other control. What the capture does legitimately consume is a press that *became* a scroll: a
gesture that actually moved the content completes no click, even when released over the row it
started on. The content moving is the test, rather than how far the pointer travelled, because it is
the gesture itself rather than a threshold standing in for one.

**The scrollable region is the content plus the container's far padding, measured off the layout the
children were last read at.** Both halves are load-bearing. A scrollable element's children are laid
out shifted by its scroll offset, so their solved boxes describe where the content *currently sits* —
the shift has to come back out (`Widget::LayoutScrollOffset`, the offset those boxes were actually
read with, which is not the live one between a scroll and the `Solve` after it), or the range shrinks
by as much as the content has already scrolled and its end is permanently unreachable. And the region
runs to the far padding edge, gutter included, so the last child clears the inside of the box the way
the first one does rather than ending flush against the frame.

**The wheel is `Document::DispatchScroll(point, delta)`**, and its delta is in **content space**: a
positive y scrolls the content down, which is the opposite sign from a wheel's own away-from-the-user
axis, so `GuiConsumer` flips it once at the input seam rather than every scrollable guessing. The
turn goes to the nearest scrollable ancestor of the element under the pointer **that can still move
that way**; a box already at that end declines and the turn passes outward to whatever contains it,
which is chaining with no chaining policy to author. Nothing left to move returns false, so an
unconsumed wheel is still available to whatever else was going to read it.

**`scrollbar: overlay | gutter`** decides whether the bars float over the content (the default,
reserving nothing) or the content box shrinks to reserve a stable gutter. The reserved width is the
bar's *own* styled thickness, so `ScrollBar { width: 6px }` narrows both the bar and its gutter from
one value; the gutter is held whether or not the axis currently overflows, since reserving only
while scrollable is what makes content jump as it grows.

**A `Slider`'s fill and thumb are widget parts on the same mechanism.** The `Slider` element is the
track (its own background, border, and radius); `SliderFill` and `SliderThumb` are parts placed from
the value fraction against the host's solved box. This retires the overload where a slider's fill was
its `color` and its thumb its `border-color` — one color per part, no hover state, and a border that
could not differ from the handle. A value change re-places the parts **directly** rather than
dirtying the tree, so dragging a slider does not re-run the flex solve per pointer move.

**The widget parts live in `Children`, as a trailing tail.** That buys the layout mirror, the
cascade, paint order, and hit-testing with no parallel paths — a bar or thumb is drawn and hit like
any element. The cost is that they are not *content*, so every content-shaped walk (item slots, the
`Selected` projection, focus order, template capture, table columns, the list grow/shrink, the
scroll extent) goes through the single **`ContentChildren`** accessor, which trims the tail, rather
than each testing the kind itself. `Document::Add` maintains the invariant by inserting content
*ahead* of the tail — a bar is created before the content it scrolls whenever `InitWidget` runs
before a `List`'s first item sync, and without that one funnel the parts interleave and a repeater
captures a scrollbar as part of its item template.

**An item host selects over item slots, not over elements.** A `List` — and a `Table` — takes a
`selection` attribute (`single` / `multiple` / `extended`, absent = unselectable, the default and the
status quo) that makes it a **list view**: a selectable container whose unit of selection is one
**item slot**, the whole authored item subtree an array element instantiates. So an item may contain
any elements at all — a row of `Text` + `Image` + `Button` is one selectable unit, exactly as a bare
`Text` item is — and nothing about selection is text-shaped. The three modes are distinct input
grammars, not degrees of one: **`single`** keeps exactly one item and replaces it on each activation;
**`multiple`** toggles one item per activation with **no chord**, which is what a gamepad or touch
surface has; **`extended`** is the desktop convention — a plain click replaces, `Control` (or `Meta`)
toggles, `Shift` extends a contiguous range from the **anchor**, which a range extend deliberately
leaves standing so a run of Shift-clicks re-extends from one origin rather than walking it forward.

Selection is **state, style, and geometry through the paths that already exist**. An item slot's
elements carry the `ElementState::Selected` bit, so `:selected` is a cooked style variant folded by
the same `Update` pass as `:hover` — there is no selection-specific paint path. A selectable host's
item roots become **focus stops**, so directional navigation walks the items and `ScrollIntoView`
reveals the focused one inside a `ScrollView` ancestor; `Single` and an unmodified `Extended` move
carry the selection with focus, `Control` detaches focus from it, and `Confirm` applies the chord
before the item's own activation, so an item template rooted at a `Button` both selects its row and
fires its `onClick`. The chord itself reaches the document as `Gui::InputModifiers` on
`PointerEvent` and as `Navigate`'s second argument, mapped once at the input seam.

**The selection is over the bound array, so it survives a re-sync.** `Document::GetSelectedItems`
returns **array indices**, not elements — the indices a game indexes its own model with — and a
`SyncList` that grows or shrinks the items re-clamps the set and re-projects the state bits rather
than losing it. The user-driven path fires the host's `onSelectionChanged`; the programmatic setters
(`SetSelectedItems` / `SelectItem` / `ClearSelection`) are **one-way** like a `{value}` binding, so a
game writing its model's selection back each frame never re-enters its own handler.
`Document::GetItemIndex(element)` closes the loop the composite item opens: a handler that receives
the `Button` from row 3 can ask which row raised it, which a repeater whose items are arbitrary
subtrees otherwise cannot answer. It resolves on any `List`/`Table` descendant, selectable or not.

**`List` is the runtime-varying repeater; `count` is the fixed authored pool.** `count="N"` on a
markup element is a **cook-time** unroll — it replicates the element's subtree N times with
`${i}`/`${n}`/`${n:0W}` substituted into every attribute and text (the runtime loads N ordinary
siblings, so the format is unchanged), for a fixed pool the game drives by hand. `List` stays the
data-bound repeater for arrays whose length varies at runtime. A `count` pool needs no ids:
`Document::FindAllByClass(name)` returns every element carrying the class in tree order, resolved
once and cached like `FindById`.

## Utilities: consumer-drive helpers, not a framework

A HUD's per-frame drive is boilerplate the engine owns as small **device-free value types** a game
composes: `DocumentHost` (`Veng/Gui/DocumentHost.h`) owns the lazy load → instantiate → bind →
attach lifecycle and re-attaches across a viewport recreation, with `SetOnInstantiate` running a
resolve-elements-once callback after every (re)instantiate (invoked immediately if the document is
already live). `Gui::Presence` (`Veng/Gui/Presence.h`) eases a boolean open/close goal to an alpha
through the frame-rate-independent `Veng::ExpApproach` (`Veng/Math/Ease.h`), reporting a hidden
threshold and a signed slide offset; `Gui::KeyedPresence<Key>` wraps it as the close-over-stale /
adopt-once-hidden swap a keyed panel needs. Neither touches a document — the caller applies the
alpha/slide (`SetOpacity`, `SetVisible`, `SetPlacement`), so placement stays caller-owned; a
**declarative enter/exit-transition system is deliberately not built** (an exit animation needs
keep-visible-until-settled sequencing the utility owns). `Viewport::WorldToDocument`
(`WorldToRegion` ÷ the UI scale — the logical-point space a document lays out in) and
`Viewport::GetDocumentExtent` bridge a projected world point into HUD space, and the device-free
`Gui::Placement` helpers (`ClampIntoBounds`, `AnchorBeside`, `Veng/Gui/Placement.h`) clamp a
card/label into bounds; the projection policy and rejection margins stay the game's. Three drive
paths support them: `SetText` early-outs on unchanged text, `SetImageUv` is a paint-only
atlas-flipbook setter, and `SetRotation` a paint-only per-frame angle.

**Pinning comes in two forms, and only one of them names a size.** `SetPlacement(element, topLeft,
size)` writes an absolute position *and* a fixed `Points` extent; `SetPinnedPosition(element,
topLeft)` writes the position and leaves `Width`/`Height` alone, so an element declaring neither is
sized from its content the way any in-flow element is, and one whose style authored a length keeps
it. Both re-dirty layout only on a real change, but the tests differ: the rect form compares
position *and* the size it wrote, while the position form compares **position and position type
only** — an auto-sized element has no written size to compare, so folding one in would re-dirty
every frame and cost a full re-solve on a caller that re-pins each frame. Their writes both land in
`BaseStyle`, which makes them **not interchangeable on one element**: an element pinned once by rect
carries that `Points` size forever, and switching it to content sizing takes a `SetStyle`. A caller
that needs the resulting extent reads `Element::Layout` after the next `Solve` — the measurement
already exists, so no measure-without-solving entry point does.

## Two placements: the screen-space overlay and the world-space surface

A document composited per-viewport over the scene is a **screen-space overlay** (above): it
composites **after** tonemap, stays **LDR**, and therefore does **not** glow — a bright overlay
color clamps at white. A document mapped onto a world mesh is a **`GuiSurface`**
(`Veng/Gui/Surface.h`) — a reflected scene component owning a live `Gui::Document`, a **persistent
HDR (`RGBA16Sfloat`) render target** the document records into each frame (`Gui::RenderTarget`, the
`Offscreen`-viewport `GetOutputHandle` contract), and a **material domain** (`GuiSurfaceDomain`)
that turns that target into scene light. Both domains composite into the lit HDR scene color
**before** bloom + tonemap, so a document color above 1.0 blooms through the scene's **own** bloom
with **no dedicated GUI bloom pass**:

- **`Translucent` (default):** the panel material returns the document texel as its radiance,
  transparent regions show the scene behind, and the surface writes no depth (a hologram / floating
  readout / see-through display).
- **`OpaqueEmissive`:** the document handle drives an opaque lit surface's emissive term, so the
  panel occludes what is behind it and can carry a lit bezel (a solid monitor / sign).

Glow is therefore a property of GUI **in the world**, exactly as a real display emits its own
pixels; a screen-space overlay is honest, un-glowing UI drawn after the scene's tone mapping. The
HDR color a `GuiSurface` needs is authored with the cooker's **`rgb()` / `rgba()` linear-float**
syntax (a component may exceed 1.0, distinct from sRGB hex) and the HDR gradient ramp. The engine
drives every `GuiSurface` in a viewport's bound scene into its target ahead of the scene render, so
a panel authored as prefab data needs no per-frame game code; its document data-binds like any
other (`{obj.field}`) and names its own `GuiDriver` in the reflected `Driver` field beside
`GuiOverlay`'s (see the driver section below). The surface **declares its sibling `MeshRenderer` required** (`VE_REQUIRES`),
the mesh being where the document lands, so `Scene::RemoveComponent` refuses to strip the renderer
while the surface is beside it — see [../Scene/CLAUDE.md](../Scene/CLAUDE.md), "The ECS world".
Because a bright emissive core desaturates through the scene tonemapper, a saturated hot value
(e.g. `rgb(0, 8, 8)`) reads white-hot at its center with a colored bloom halo — the
physically-expected hot-emitter look. Authoring a glowing panel end to end is
[docs/guides/diegetic-ui.md](../../../docs/guides/diegetic-ui.md).

**A surface's layout extent and its target's pixel count are two numbers, not one.** `Resolution`
is the extent, in **logical points**, the document lays out against; `PixelScale` (default `1.0`) is
target pixels per point, so the HDR target allocates `round(Resolution × PixelScale)` and the draw is
magnified into it through `GuiScenePass::SetUiScale` — the same magnification the overlay path takes
from its viewport. A hidpi panel therefore sets `2.0` rather than doubling every authored size, and
the default is byte-identical to one number doing both jobs. Three things make it work rather than
just allocate more memory: the scale reaches the pass (not only the target), it sits in
`DocumentTexture`'s dirty gate beside the extent (a scale change moves target pixels *and* the
magnification while leaving `available` alone, so `Document::Solve`'s own early-out would otherwise
skip the re-record), and `GuiSurface::Drive` clamps it so the derived extent is at least one pixel
and within the device's `maxImageDimension2D`.

**The shape a display-on-glass wants is a generator, not a mesh asset.** `Primitives::ProjectionShell`
(`Veng/Asset/Primitives.h`) is the engine's only **projection-derived** geometry: a grid over a
normalized screen rect, unprojected through a given perspective and placed at a fixed radius, so the
resulting spherical-cap section reproduces that screen rect exactly when viewed from its own eye
point and behaves like an ordinary pane from everywhere else. Its companion
`ProjectionShellReprojectionBound` is the closed-form between-vertex error in logical points, which is
what an alignment budget or a test threshold is stated in rather than a tuned constant. The pair is
covered by `tests/unit/projection_shell.cpp` (the reprojection property, checked through
`ProjectToScreen` and never the generator's own inverse) and `tests/gpu/projection_shell.cpp` (the
document-on-mesh geometry proof: overlay versus shell at the reference pose, plus a translated-eye
control). Authoring one is
[docs/guides/diegetic-ui.md](../../../docs/guides/diegetic-ui.md#perspective-true-shells-a-panel-that-agrees-with-a-screen-space-layout).

**A display that should look curved is the other generator, `Primitives::CurvedPanel`.** A shell's
centre of curvature *is* its eye point, so from that eye its curvature is invisible by construction —
a panel's curvature is instead decoupled from its viewing distance, at the cost of the exact
screen-space agreement. Its `CurvedPanelHit` companion is what a document on one uses to place a
world-anchored marker, since the canvas↔screen mapping is no longer a translation. The choice between
the two is written up beside the shell's section in the same guide.

## The engine-driven scene component family

The screen-space overlay is a reflected component too — the engine-driven scene component family
has three members (scene/ECS material is [../Scene/CLAUDE.md](../Scene/CLAUDE.md)). A
**`GuiOverlay`** (`Veng/Gui/Overlay.h`) is the screen-space sibling of `GuiSurface`: a reflected
scene component `{ AssetHandle<Gui::UIDocument> Document; i32 Layer; GuiDriverId Driver; bool
Interactive; Reference TargetSeat; }` the **Viewport** discovers the same way (`View<GuiOverlay>()`) and drives onto its
**screen-space layer stack** through a `Gui::DocumentHost` + `Gui::DocumentLayer` — the exact
`AttachDocument` path a HUD reaches by hand, owned by the engine (lazy load → instantiate → attach
at `Layer` → re-attach on recreation). It is **LDR, composited after tonemap, un-bloomed** — the
honest overlay `GuiSurface` is *not*. **Which viewport claims it is decided by seat:** a viewport
claims the overlays whose target seat is its own (the entity's own seat, or `TargetSeat`; unbound →
the sole/primary presenter), so split-screen per-player HUDs fall out and a single overlay never
thrashes. The game owns only the binding — a reflected state component plus a small system that
`SetContext`s a `Gui::BindingContext` (view-model + `onClick` handlers) and `Invalidate`s it — not
the load/instantiate/attach. `Interactive` gates whether it takes input. **`Detach(viewport)` is the
exact inverse of `Drive`** — it releases the driven document from a viewport's layer stack while the
runtime host survives for the next `Drive`, idempotent and touching only the document the engine
attached. `~GuiOverlay` detaches on component destruction (the right lifetime when the *component*
goes); `Detach` covers the other case — a viewport stops presenting a world that stays alive (a world
rebind), where the engine detaches the departed scene's overlays without waiting on component
teardown.

### The driver — per-instance presentation binding

The game owns only the data binding, and the **ergonomic path for it is a driver**, not a
find-and-bind system. A **`GuiDriver`** (`Veng/Gui/Driver.h`) is a named, registered, per-instance
presentation binding the engine instantiates from component data — from a `GuiOverlay` or a
`GuiSurface`, which carry the same reflected `Driver` field and resolve it the same way.
`VE_GUI_DRIVER(Type, 0x…ULL, "Name")` mints a `GuiDriverId` (a `u64` leaf in the `SystemId`/`ActionId` id family, minted with
`vengc generate-id`); a module registers the driver into the host-owned **`GuiDriverRegistry`**
(`Veng/Gui/DriverRegistry.h`, mirroring `SystemRegistry` — register/enumerate-without-instantiating/
duplicate-id-fatal, GPU-free/cooker-safe) reached through `VengModuleHost::Drivers` (the member whose
addition **bumped `VENG_MODULE_ABI_VERSION` 5 → 6**). A `GuiOverlay` names one in its reflected
`Driver` field (`GuiDriverId::Null` = undriven, the status quo). The component's `Drive` instantiates
the named driver on the first drive, owns it in the runtime (destroyed with it), re-runs
`OnInstantiate` whenever the document (re)instantiates — exactly like `SetOnInstantiate` — and calls
`OnUpdate` each drive with a `GuiDriverFrame { Document, Scene, Owner, Seat, Delta, Alpha, View }`
carrying the claiming viewport's real view. `Owner` is the entity the driven component sits on, so a
driver reads its own authored configuration and its siblings (its config component, its
`MeshRenderer`, its `Transform`) rather than searching the scene for itself; `Alpha` is the render
gather's own interpolation fraction, so a driver placing a marker against a moving body reads the
pose being drawn rather than the tick pose. Two claimed instances of one overlay (split-screen) are
**two driver instances with independent view-models**, so the per-instance state a per-world binding
system would key by entity dissolves. `OnInstantiate` resolves elements and binds the driver's
`Gui::BindingContext` through `Document::BindContext(context)` (the registry-free overload — the
document supplies its own).

**Where in the frame a driver runs differs between the two components, and it is the one asymmetry.**
An overlay composites *after* the scene, so `DriveOverlays` runs after the render gather and what its
driver stamps is read by the **next** frame's gather. A surface is sampled **by** the scene, so its
document has to be current before the gather — `RenderSurfaces` therefore drives it (and its driver)
*ahead* of `Execute`, and what a surface driver stamps is read by the **same** frame's gather. Neither
may add or remove the component it is driven from, which would disturb the `View<>` walk it is
running inside. The other difference is what the claim decides: an overlay is *presented* by the
viewport that claims it, while a surface's document is one document on one mesh in the world and is
rendered by **every** presenting viewport — `Viewport::ClaimsSurface` (the surface's own `Seat`, else
the sole/primary presenter) decides only which viewport owns the per-frame **driver** update, so a
scene shown twice does not update one view-model twice.

**The boundary is concrete and checkable.** A driver reads scene state, stamps request/command
components, and beyond those may write **only a component tagged `VE_VIEW_OUTPUT`**
(`Veng/Reflection/Reflect.h`) — derived, view-owned state gameplay may read but no simulation or wire
owns (`TypeInfo::ViewOutput` records the mark). A driver **never** writes a `VE_REPLICATED` or a
Sim-input component, and never advances authoritative simulation — that stays components + systems. The
bare `SetContext` / find-and-bind system pattern remains fully supported; the driver is the ergonomic
path, not the only one. Both examples show the shape: hello-triangle's HUD sweep and the template's
overlay HUD binding are drivers, their levels' `systems` arrays carrying no binding-only system.

The **third** family member is **`CaptureSurface`** (`Veng/Renderer/CaptureSurface.h`), the
render-to-texture sibling: a reflected component that puts a `SceneCapture` on an entity,
discovered and driven by the engine (built on first sight, fed to the `RegisterCapture` drive-list
against its lifetime, self-unregistering when the component/entity/scene goes), rebinding the
capture's output onto the sibling `MeshRenderer`'s material each frame so a mirror / probe /
monitor is authored data. Its `Refresh` is `EveryFrame` or `OnDemand` (render once, then
idle until `MarkDirty`).

**The locality is per mesh *asset*, not per entity.** The bound target is the first
`MaterialInstance` of the mesh the sibling `MeshRenderer` names — a **cooked, shared** asset, so two
entities drawing one mesh asset resolve to one instance and one texture slot: the last driven wins
and both sample that single probe. N independently captured surfaces therefore need N mesh assets (or
N material instances), and the engine **warns once per run** when one drive pass binds two captures
onto the same instance rather than resolving it silently. A per-entity material override is
deliberately *not* built — that is a change to the material model, not to the capture.

**A capture also publishes where it was rendered from.** `CenterSlot` (empty = off, beside
`TextureSlot`/`SamplerSlot`) names a `Param` field the drive fills with `vec4(probe position,
validity)` each frame. A **parallax-correcting** fragment needs the centre — the map is a view from
the probe while the sampling ray leaves the fragment, so the correction intersects that ray with a
proxy volume about the probe and re-takes the direction from there — and `SurfaceFragmentInput` gives
a fragment no route to its own draw's world matrix, so without the slot a consumer reimplements the
drive's own position lookup in a system of its own. The fourth component is a **validity flag**, 1
only once an output slot exists: without it a fragment cannot distinguish "no capture yet" from "a
probe at the world origin" and would index the bindless array with an unpopulated handle, so the flag
is what makes its fallback branch reachable. (A `nointerpolation float3 v_ObjectOrigin` on
`SurfaceFragmentInput` would serve *any* surface material, but it changes the shared vertex contract —
the five files the format spans plus every surface fragment — for a need one consuming domain has.)

**And which frame it was rendered in.** `OrientationSlot` (empty = off, beside the centre) names a
`Param` field the drive fills with the face basis as a **unit quaternion**, `xyz` imaginary and `w`
real — the capture-frame → world rotation, so a fragment expresses a world direction in the map's
frame by rotating by its conjugate. An **`Entity`-aligned** capture's map is a body-fixed environment,
which is exactly the case a direction sampled in world space is wrong for; `SurfaceFragmentInput`
gives a fragment no route to the carrier's frame, so without the slot a consumer reconstructs one out
of its interpolated normal and tangent — valid only for the single surface orientation it was derived
for, and silently wrong for a second panel on the same material at another angle. A **`World`-aligned
capture publishes the identity rotation** (0, 0, 0, 1) rather than nothing, so the consumer needs no
branch on the alignment and switching between the two changes only the published value. The slot
carries no validity flag of its own — the centre's `w` already reports the binding, and a material
correcting a sample declares both slots or neither. `PackCaptureOrientation` is the encoding, pure and
device-free beside the component.

**The capture is placed at the pose its entity is *drawn* at, not the one it was simulated at.** The
drive resolves the position through `Scene::GetInterpolatedWorldTransform` at the world's own
`LastAlpha` and hands that alpha to `Drive` as well, so `CaptureView::Position` (the face cameras and
the published centre) and `CaptureView::Alpha` (the content the face renders draw) sit on one pose —
the same pose the renderer draws the mesh the capture feeds at. This is the `CameraRigSystem` rule
applied to a probe, and for the same reason: a capture resolved against the un-interpolated pose sits
a partial tick from its own carrier, so everything rigidly attached to that carrier is sampled from
the wrong place by an offset that **reopens and collapses once per tick** as the alpha sweeps — read
as vibration rather than lag, growing with the carrier's speed and turn rate and with the mount
radius. See [../Scene/CLAUDE.md](../Scene/CLAUDE.md) for the camera-rig statement of it. Because the
drive walks every world and each ticks on its own clock, the alpha is read per world, not once per
frame.

**Teardown is the exact inverse of the bind.** `CaptureSurface::Unbind` — the `GuiOverlay::Detach`
counterpart — writes the unbound state back onto the material the last drive bound: an invalid handle
into the texture and sampler slots, a zero `vec4` into the centre (so the validity flag reads 0 and
the consumer's fallback is taken), and the **identity rotation** into the frame — not a zero `vec4`,
which normalizes to a NaN in a consumer reading it past the gate. The component's destructor calls it, so removing the component,
destroying the entity, or dropping the scene stops the material naming a bindless slot the capture's
release hands back to the free list — without it the sampled result does not revert, it freezes on
whatever registers into that slot next. The component holds the material it bound **resident** for
exactly that reason, which is why `Drive` takes an `AssetHandle<MaterialInstance>` rather than a raw
pointer. A handle slot returns to the unbound sentinel, not to a cooked default: these slots are
runtime-bound, and a fragment reading one without checking the validity flag indexes the array with it.

**The capture excludes the entity it feeds** (`CaptureView::Exclude`, set by
the component itself, in every domain the capture draws): a surface is not part of its own
environment, and a probe sitting on or inside that surface would otherwise both compound its own
sampled term into the next capture and occlude the environment behind it — see
[../Renderer/CLAUDE.md](../Renderer/CLAUDE.md). So the family is one interface across three targets — `GuiSurface` (a
document on a world mesh, HDR, glowing), `GuiOverlay` (a document on the viewport layer stack, LDR,
screen-space), and `CaptureSurface` (the scene rendered into a texture, sampled by the entity's
material) — each discovered by `View<…>()` and driven by the engine. Authoring the screen-space and
RTT members, and opening a level as an overlay, is
[docs/guides/screen-space-ui-and-overlays.md](../../../docs/guides/screen-space-ui-and-overlays.md).

## Authoring surfaces

The editor's `UIDocumentEditorPanel` authors a `*.vui.xml` through the cook-on-demand loop (a
WYSIWYG canvas over an `Offscreen` viewport hosting the live document, an element-tree outline, and
a resolved-style inspector), writing the source only on an explicit save; see
[editor/CLAUDE.md](../../../editor/CLAUDE.md). A task-oriented
authoring tutorial lives in
[docs/guides/authoring-ui-documents.md](../../../docs/guides/authoring-ui-documents.md).
