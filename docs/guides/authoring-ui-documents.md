# Authoring a UI document

This guide covers **`Veng::Gui`**: how a game authors a HUD or menu as **content** —
a `*.vui.xml` markup document plus a `*.vuss` stylesheet, cooked into assets — binds it
to live game state, attaches it to a viewport, and makes it interactive. It is the game
UI layer, distinct from `Veng::UI` (the immediate-mode ImGui vocabulary the editor and
debug panels use): a game HUD is a persistent tree of widgets with focus, transitions,
and bindings, authored by a designer, not re-issued in C++ every frame.

The live reference is the [template](../../examples/template/) module: its HUD markup is
in [`assets/ui/hud.vui.xml`](../../examples/template/assets/ui/hud.vui.xml), its
stylesheet in [`assets/ui/hud.vuss`](../../examples/template/assets/ui/hud.vuss), its
font in [`assets/fonts/ui.font.json`](../../examples/template/assets/fonts/ui.font.json),
and the ~15-line `Application` subclass that instantiates, binds, and attaches it is in
[`main.cpp`](../../examples/template/main.cpp). Open them beside this guide.

---

## The shape of it

```
*.vui.xml  ─┐                                    ┌─► Gui::Document::Instantiate ─► attach to a Viewport
*.vuss     ─┼─► vengc cook ─► UIDocument asset ──┤   (a live tree, engine-driven each frame)
*.font.json─┘   (markup+CSS parsed offline)          └─► BindContext + UpdateBindings (game state → widgets)
```

The keystone: **the cooker parses the markup and stylesheet; the runtime never does.**
`*.vui.xml` becomes a binary element tree, `*.vuss` becomes flattened resolved style
tables — the runtime loads the cooked blob and only lays it out, resolves styles, and
draws. So the heavy parsing (XML + CSS) is cooker-side, never linked into your game.

Four things you author, three things the engine does:

1. **A font** — a `*.font.json` naming a TTF/OTF, cooked into an MSDF glyph atlas.
2. **A stylesheet** — a `*.vuss` of type/class/id selectors and their properties.
3. **A document** — a `*.vui.xml` of elements, referencing the stylesheet and font by id.
4. **A tiny `Application` subclass** — instantiates the cooked document, binds a
   view-model, and attaches it to the managed viewport.

The engine loads the assets, drives every attached document's layout + draw each frame,
and composites the layers over the scene.

---

## 1. Author the font

A `*.font.json` names a TTF/OTF (relative to itself), a charset, a glyph size, and an
SDF pixel range. The cooker generates a multi-channel SDF (MSDF) atlas + a metrics table,
so text is crisp at any scale from one small atlas:

```json
{
  "font": "Roboto.ttf",
  "charset": "ascii",
  "glyphSize": 40,
  "pixelRange": 4
}
```

Add the font to your pack manifest (`*.vengpack.json`) as an ordinary asset entry:

```json
{ "id": "0x9A6A7CD94A6DDBD7", "type": "Font", "source": "fonts/ui.font.json" }
```

Mint the id with `vengc generate-id` — never hand-invent one.

## 2. Author the stylesheet

A `*.vuss` is a USS-like subset: **type**, **class** (`.name`), and **id** (`#name`)
selectors, plus pseudo-states (`:hover`/`:active`/`:focus`/`:disabled`/`:checked`). No
full-CSS specificity cascade, no floats — the cooker matches selectors offline and emits
each element's resolved style plus its state variants, so the runtime never runs a
selector engine.

```css
.hud {
    flex-direction: column;
    background: #12141acc;
    corner-radius: 6px;
    padding: 12px;
}

.title {
    font-size: 22px;
    color: #f2f4f8ff;
}

.bar {
    height: 8px;
    background: #2a6df4;
    corner-radius: 3px;
}
```

Layout is **flexbox** (Yoga): `flex-direction`, `justify-content`, `align-items`,
`flex-grow`/`flex-shrink`/`flex-basis`, `width`/`height` (`px` or `%`), `margin`,
`padding`, `position`/`inset`. Paint is `background`, `background-gradient`, `color`
(text / widget fill), `corner-radius`, `border-width`/`border-color`, `opacity`. Colors
are hex `#rrggbb` or `#rrggbbaa`, resolved sRGB→linear at cook time. Register the
stylesheet in the pack as type `StyleSheet`.

A `background-gradient` fills the element with a multi-stop gradient instead of a flat
color (it wins over `background` when both are set, and composes with `corner-radius`
and a border). The multi-stop color is baked into a ramp at cook time; the shape is one
of three, each spanning the element's box:

```css
/* linear: an angle (CSS convention — 0deg to the top, 90deg to the right) then stops */
.panel  { background-gradient: linear 135deg, #1a2b3c 0%, #4a5b6c 40%, #ff0080 100%; }
/* linear with explicit endpoints, as percentages of the box (0%,0% top-left → 100%,100% bottom-right) */
.streak { background-gradient: linear from 0% 0% to 100% 40%, #000 0%, #fff 100%; }
/* radial: an optional `at <x>% <y>%` center (default 50% 50%), farthest-corner fit */
.orb    { background-gradient: radial at 30% 30%, #ffffff 0%, #202080 100%; }
/* radial with an explicit radius — one value is circular, two make an ellipse (% of the half-box) */
.oval   { background-gradient: radial at 50% 50% radius 80% 40%, #fff 0%, #206 100%; }
/* conic: an optional `from <angle>` and `at <center>` */
.dial   { background-gradient: conic from 90deg at 50% 50%, #000000 0%, #ffffff 100%; }
```

Stop positions (`40%`) are optional — omitted stops distribute evenly. A gradient is
authorable **only in a stylesheet rule** (like `animation`), applied from the base
(non-pseudo-state) rules. It is not yet variant-swappable or transition-eased, but a
gradient **can be animated from C++**: `Document::SetBackgroundGradient(element, …)`
sets a resolved gradient whose `P0`/`P1`/`AngleOffset` you mutate per frame — moving a
linear axis, growing a radial, or spinning a conic — a paint-only write with no re-solve.

A state variant is the selector plus a pseudo-state — a `:hover` rule contributes a
variant the runtime folds over the base style when the element is hovered, easing any
transition-able property:

```css
.primary { background: #3b82f6; }
.primary:hover { background: #60a5fa; }
.primary:disabled { opacity: 0.5; }
```

## 3. Author the document

A `*.vui.xml` is a tree of elements. The **root** element carries a `stylesheets`
attribute — a space-separated list of `StyleSheet` asset ids — and any element references
a font through the `font:` inline-style property (an asset id). Element attributes are
class/id tags, inline `style`, `{obj.field}` **bindings**, and `on*` **handlers**:

```xml
<Panel class="hud" stylesheets="0x9199FE52C60D7DF8" style="font: 0x9A6A7CD94A6DDBD7; width: 240px;">
  <Text class="title">veng</Text>
  <Text class="caption" value="{Caption}"/>
  <ProgressBar class="bar" value="{Level}"/>
</Panel>
```

The widget set: `Panel` (flex box), `Text`, `Image`, `Button` (`onClick`), `Checkbox`
(`value`/`checked`/`onChange`), `Slider` (`min`/`max`/`step`/`value`/`onChange`),
`ProgressBar` (`value`), `TextInput` (`value`/`onChange`), `ScrollView`, and `List` (a
data-bound repeater whose single child is the item template). Register the document in the
pack as type `UIDocument`; its font and stylesheet resolve as ordinary cook/load
dependencies.

A `{obj.field}` value is a **binding** resolved against a bound view-model through
reflection; a literal (`min="0"`) is read once at instantiate. A binding path is a dotted
field path — `{player.health}` resolves `health` on the `player` field of the bound
object; a single segment (`{Level}`) resolves a field on the bound object directly.

### `Image` — a textured box

An `<Image>` draws a cooked texture. It takes a `src` — the texture's asset id — plus two
optional literals: a `tint` (a `#rrggbbaa` color multiplied into the texels, the style
opacity folding into its alpha) and a `uv` (four space-separated numbers `minX minY sizeX
sizeY` selecting a sub-rect of the texture, for an atlas; the whole texture by default):

```xml
<Image class="badge" src="0x502E61AE5D720E64" tint="#ffffffff" uv="0 0 1 1"/>
```

The `src` texture is a document dependency the loader keeps resident, resolved to a live
handle at instantiate time through the same `AssetManager` path a font uses. An image is
**sized by style** — `width`/`height`/flex, like any element — and it **composes with
`corner-radius` and a border**, so a rounded, framed thumbnail is `corner-radius` +
`border-width`/`border-color` on the `<Image>` with no extra markup. An image with no
resolved texture paints its styled box only (its background/border), so a missing texture
degrades rather than crashes.

> `Image` v1 is a plain textured box. **9-slice** (a border-stable stretch for panels and
> frames) and **texture-intrinsic sizing** (an image measuring to its texture's pixel
> dimensions the way `Text` measures its run) are natural follow-ons on the same element,
> not yet authored.

## 4. Instantiate, bind, and attach

The one thing the engine cannot do from data alone is wire the document to *this game's*
state. That is a tiny `Application` subclass. Define the **view-model** — a reflected
struct whose fields the bindings read — and register it with your module:

```cpp
struct TemplateHud
{
    string Caption = "warming up";
    f32 Level = 0.0f;
};

VE_REFLECT(::TemplateHud, 0x0D0C072CE1127CF4ULL)
VE_FIELD(Caption)
VE_FIELD(Level)
VE_REFLECT_END();
```

Then, in `OnWorldLoaded` (fired once the managed world is loaded), load the cooked
document, **instantiate** a live tree (resolving its font through the asset manager),
**bind** the view-model, and **attach** it to the managed viewport:

```cpp
void OnWorldLoaded(Scene&, ResidencyBatch&) override
{
    AssetManager& assets = GetAssetManager();
    const auto recipe = assets.LoadSync<Gui::UIDocument>(HudDocumentId);
    if (!recipe) { return; }

    m_Hud = Gui::Document::Instantiate(*recipe->Get(), assets);
    m_Context.SetData(m_Model);
    m_Hud->BindContext(&m_Context, &GetTypeRegistry());
    GetPrimaryViewport()->AttachDocument(*m_Hud);
}
```

`Instantiate` borrows the `AssetManager` and resolves the document's asset-backed
declarations through it directly — a font declaration's id loads to its atlas, and an
`<Image>`'s `src` to its texture, as ordinary resident dependencies (so the manager must
outlive the document). It copies the recipe into an **independent** tree (instantiate twice for two
split-screen HUDs over one cooked blob — the `Prefab` model). Attaching hands the viewport
a non-owning pointer; you keep the `Unique<Gui::Document>`, and dropping it self-detaches.

Each frame, feed the view-model and **re-resolve the bindings** — the engine drives the
document's layout and draw, but the binding re-read is your call (a no-op when nothing
changed):

```cpp
void OnUpdate(f32 delta) override
{
    m_Model.Caption = fmt::format("{:.0f} fps", 1.0f / delta);
    m_Model.Level = std::clamp((1.0f / delta) / 120.0f, 0.0f, 1.0f);
    m_Context.Invalidate();        // the model changed; re-read the dirtied bindings
    m_Hud->UpdateBindings();
}
```

Release the document in `OnDispose` (`m_Hud.reset()`) before the context tears down.

> A UI-only viewport must render over *something*: `Viewport::Render` composites a
> document only when the viewport has a scene to render over, so a viewport with no game
> world pushes an empty `Scene` (a cleared target). The template's managed viewport
> already renders the game world, so its HUD composites over the cube with no extra wiring.

## 5. Make it interactive

A document is **display-only by default** — its bindings update and it draws, but it
hit-tests and takes no focus, so a HUD never steals input from gameplay. A menu becomes
interactive when the game opens a **`SeatFocusScope`** on the document's seat and flips
`SetInteractive(true)`:

```cpp
// While the menu is open — RAII: the takeover restores on scope exit.
m_MenuFocus.emplace(GetInputRouter(), seat, GetPrimaryViewport(), m_MenuContext);
m_Menu->SetInteractive(true);
```

The `SeatFocusScope` is the whole transition: it pushes a UI focus entry on that seat's
stack, swaps the seat's input context, and associates the viewport for pointer routing —
all restored in inverse order when it destructs. The input consumer then routes that
seat's pointer, keyboard, and gamepad into the document: pointer events propagate
capture → target → bubble, directional focus navigation moves a focus ring, and a
`Button`'s `onClick="Name"` fires the handler registered on the binding context under
`"Name"`. Because focus is **per-seat**, seat A opening a menu leaves seat B playing —
split-screen-correct by construction.

## Editing in the editor

Opening a `UIDocument` in `veng-editor` (double-click it in the asset browser) opens the
**UI document editor**: a WYSIWYG canvas rendering the live document, an element-tree
outline, and a resolved-style inspector. Editing the `*.vui.xml`/`*.vuss` source recooks
off the render thread and hot-reloads the document behind its stable handle, so the canvas
reflects the change — the same cook-on-demand loop the texture and material editors ride.
The document is the source of truth.

The panel also authors an `<Image>` directly: **Add Image** appends one under the document
root, and selecting an image in the outline shows its `src` as a **texture asset-chip** in
the inspector — drop a texture from the asset browser onto it (or click to pick one) to
repoint the image, which rewrites the `src` in the markup and recooks behind the stable
handle.

---

## Recap

| You author | Cooked as | The engine does |
|---|---|---|
| `*.font.json` (+ TTF) | `Font` (MSDF atlas + metrics) | Loads the atlas; shapes crisp text |
| `*.vuss` | `StyleSheet` (resolved rules + variants) | Selects variants, eases transitions |
| `*.vui.xml` | `UIDocument` (recipe tree) | Instantiates a live tree; solves Yoga layout; draws |
| a ~15-line `Application` subclass | — | Drives layout + draw + composite each frame |

The runtime never parses XML or CSS — the cooker does, once, offline. Bindings and
handlers resolve against a reflected view-model through the same `TypeRegistry` the editor
inspector and the serializer use. A document is content a viewport hosts in ordered layers,
engine-driven, display-only until a `SeatFocusScope` opens it. The full architecture is in
[engine/CLAUDE.md](../../engine/CLAUDE.md), "Veng::Gui".
