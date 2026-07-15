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

`AssetType::Font` (`Veng/Asset/Font.h`) is an **MSDF glyph atlas** (a bindless atlas texture + a
CPU metrics table — advance, bearing, atlas rect, kerning) cooked from a `*.font.json` naming a
TTF/OTF; the runtime decodes nothing and shapes crisp text at any scale from the one small atlas.

`AssetType::StyleSheet` (`Veng/Gui/StyleSheet.h`) is a reusable cooked stylesheet — **resolved**
rules (type/class/id selectors matched at cook time) plus their
`:hover`/`:active`/`:focus`/`:disabled`/`:checked` state variants, colors resolved sRGB→linear, and
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

`AssetType::UIDocument` (`Veng/Gui/UIDocument.h`) is the cooked markup: a **pre-order recipe
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

## Styling

Styling flattens at cook time; the runtime interpolates among variants. The cooker matches the
USS-like selectors (type / class / id / pseudo-state, no full-CSS specificity cascade) and emits
each element's resolved base style plus a handful of state variants. The runtime never runs a
selector engine — `Update` selects the variants whose state bit is set in the element's live
interaction mask, folds them over the base, and **eases** any transition-able property (colors,
opacity, scalar sizes) over its authored duration through a small tween clock. A style change that
moves a layout input re-dirties the Yoga box; a pure paint change (color/opacity) does not.

## The draw floor: a device-free draw list + a `GuiScenePass`

`Gui::DrawList` (`Veng/Gui/DrawList.h`) is a device-free builder of **batched, clipped, textured
quads** — rounded-rect / border SDF, 9-slice, tint/opacity, and MSDF text runs — that
`Document::Build` appends into. A `GuiScenePass` records the draw list into an offscreen image
blended over the viewport's scene output (its Slang shaders, `gui.slang` + the MSDF text shader,
are core-pack shaders — a game reuses them, never authors a UI shader). The image goldens
(`gui_overlay`, `gui_rotated`) are the render floor every later change holds pixel-stable against.
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

## Widgets

The built-in, markup-authorable, styleable, focusable controls on the primitives: `Panel` (a styled
flex box), `Text` (a shaped MSDF leaf), `Image` (a textured box — a `src` texture with an optional
`tint`/`uv`, sized by style and composing with `corner-radius`/border on the `DrawList::Texture`
path; the `Image` widget has no 9-slice or texture-intrinsic sizing), `Button` (`onClick`),
`Checkbox` (`value`/`checked`/`onChange`, driving the `:checked` variant), `Slider`
(`min`/`max`/`step`/`value`/`onChange`), `ProgressBar` (a `[0,1]` fill), `TextInput`
(`value`/`onChange`), `ScrollView` (a clipped, scrollable region), `List` (a data-bound repeater —
its authored children are an item template cloned once per element of a bound array), and `Table`
(a column-aligning row container: each direct child is a row, and the k-th in-flow cell of every
row widens to the column's widest cell via a measured min-width between the Solve's two layout
passes; a flex-grow cell is an elastic filler that absorbs row slack instead of becoming a column,
right-anchoring the columns after it; with an `items` binding it repeats its row template exactly
as a List does). A numeric Table column pairs with the `text-align` Text style property
(`left`/`center`/`right`, a paint-only glyph alignment inside the solved box). Each is an
`ElementKind` the cooker recognizes and the widget layer gives behavior; a control's literal config
attributes (`min`/`max`/`step`/`value`/`checked`) are read at `Instantiate` and its `{value}`
binding is one-way (the model drives the widget without firing `onChange`).

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
through the frame-rate-independent `Math::ExpApproach` (`Veng/Math/Ease.h`), reporting a hidden
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
other (`{obj.field}`). Because a bright emissive core desaturates through the scene tonemapper, a
saturated hot value (e.g. `rgb(0, 8, 8)`) reads white-hot at its center with a colored bloom halo —
the physically-expected hot-emitter look. Authoring a glowing panel end to end is
[docs/guides/diegetic-ui.md](../../../docs/guides/diegetic-ui.md).

## The engine-driven scene component family

The screen-space overlay is a reflected component too — the engine-driven scene component family
has three members (scene/ECS material is [../Scene/CLAUDE.md](../Scene/CLAUDE.md)). A
**`GuiOverlay`** (`Veng/Gui/Overlay.h`) is the screen-space sibling of `GuiSurface`: a reflected
scene component `{ AssetHandle<Gui::UIDocument> Document; i32 Layer; bool Interactive; Reference
TargetSeat; }` the **Viewport** discovers the same way (`View<GuiOverlay>()`) and drives onto its
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

The **third** family member is **`CaptureSurface`** (`Veng/Renderer/CaptureSurface.h`), the
render-to-texture sibling: a reflected component that puts a `SceneCapture` on an entity,
discovered and driven by the engine (built on first sight, fed to the `RegisterCapture` drive-list
against its lifetime, self-unregistering when the component/entity/scene goes), rebinding the
capture's output onto the **same-entity** material each frame (the locality rule) so a mirror /
probe / monitor is authored data. Its `Refresh` is `EveryFrame` or `OnDemand` (render once, then
idle until `MarkDirty`). So the family is one interface across three targets — `GuiSurface` (a
document on a world mesh, HDR, glowing), `GuiOverlay` (a document on the viewport layer stack, LDR,
screen-space), and `CaptureSurface` (the scene rendered into a texture, sampled by the entity's
material) — each discovered by `View<…>()` and driven by the engine. Authoring the screen-space and
RTT members, and opening a level as an overlay, is
[docs/guides/screen-space-ui-and-overlays.md](../../../docs/guides/screen-space-ui-and-overlays.md).

## Authoring surfaces

The editor's `UIDocumentEditorPanel` authors a `*.vui.xml` through the cook-on-demand loop (a
WYSIWYG canvas over an `Offscreen` viewport hosting the live document, an element-tree outline, and
a resolved-style inspector); see [editor/CLAUDE.md](../../../editor/CLAUDE.md). A task-oriented
authoring tutorial lives in
[docs/guides/authoring-ui-documents.md](../../../docs/guides/authoring-ui-documents.md).
