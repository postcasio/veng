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
selectors, plus pseudo-states (`:hover`/`:active`/`:focus`/`:disabled`/`:checked`/`:selected`). No
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
(text / widget fill), `corner-radius`, `border-width`/`border-color`, `opacity`, and
`text-align` (`left`/`center`/`right` — a Text element's glyph alignment inside its
solved box, meaningful when the box is wider than the run, e.g. a Table cell). Colors
are hex `#rrggbb` or `#rrggbbaa`, resolved sRGB→linear at cook time. Register the
stylesheet in the pack as type `StyleSheet`.

**Edge order.** `margin`, `padding`, and `inset` take one to four lengths and follow the
CSS shorthand rules exactly. Four values run clockwise from the top — **top, right,
bottom, left**:

```css
.card {
    margin: 1px 2px 3px 4px;  /* top 1, right 2, bottom 3, left 4 */
    padding: 8px;             /* all four edges 8 */
    padding: 8px 16px;        /* top/bottom 8, left/right 16 */
    padding: 8px 16px 4px;    /* top 8, left/right 16, bottom 4 */
}
```

One value applies to all four edges; two set top/bottom then left/right; three set top,
then left/right, then bottom. `corner-radius` takes the same one-to-four form but names
corners rather than edges, clockwise from the top-left — **top-left, top-right,
bottom-right, bottom-left** — matching CSS `border-radius`. The per-edge longhands
`inset-left`/`inset-top`/`inset-right`/`inset-bottom` each take a single length and are
unambiguous.

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

### Variables and `@use`

A palette or a shared metric drifts when it is a hex literal restated in every rule (and,
worse, restated again as a C++ `vec4`). A **file-scope variable** hoists it to one token: a
`--name: value;` declaration at the top level of the sheet (never inside a rule — these are
theme tokens, not per-element custom properties, so there is no inheritance and no runtime
cost), and `var(--name)` substitutes its token sequence anywhere in a declaration value —
including a gradient stop list:

```css
--panel:  #12141acc;
--accent: #2a6df4;

.hud { background: var(--panel); }
.bar { background: var(--accent); }
.spinner { background-gradient: conic from 0deg at 50% 50%, var(--accent) 0%, #12141a00 100%; }
```

Substitution runs at cook time, so the runtime never sees a `var()`. The rules:

- **Define before use.** A `var(--x)` with no prior `--x: …;` is a located cook error.
- **Redefinition is last-wins** in processing order — define a token, cook rules against it,
  then redefine it lower in the file to override a theme for the rules that follow.
- **Inline `style="…"` does not see variables** — the document cook has no sheet in scope, so
  a token-driven property belongs in a class, not an inline style.

Two sheets share **variables** through `@use`, and share **rules** through the document's
`stylesheets` list — one mechanism per axis, no overlap. `@use "theme.vuss";` at the top of a
sheet imports **only** that sheet's top-level `--` variables (its rules are ignored by the
read; it may still cook as a sheet in its own right). The path is resolved relative to the
using sheet and recorded as a cook dependency, so editing the theme re-cooks every sheet that
`@use`s it:

```css
@use "theme.vuss";           /* pulls in theme.vuss's --tokens, not its rules */
--accent-strong: var(--accent);   /* a variable may build on a used one */
```

Imperative code reads the same palette the rules were flattened from, so the C++ side needs no
duplicated `vec4` table: a loaded `StyleSheet` answers `FindVariableColor("accent")` and
`FindVariableScalar("gap")` (the name **without** the leading `--`). Only the sheet's **own**
top-level variables whose value parses as a single color or a single number are queryable — a
multi-token variable and an `@use`d one are cook-time-only (an `@use`d variable is queried on
the theme sheet that owns it). The `template` HUD hoists its palette this way; `hello-triangle`
reads its `--accent` back through `FindVariableColor` to tint a HUD element from the one source.

### Rotation

`rotation` is a scalar style property — degrees, clockwise in the y-down document space, `0` by
default — that turns an element's whole subtree rigidly at paint time. Like any scalar property
it eases through a transition and animates through a keyframe clip, so a continuously-spinning
element is a stylesheet `@keyframes` on `rotation` with no per-frame C++:

```css
@keyframes spin { from { rotation: 0; } to { rotation: 360; } }

.spinner {
    origin: 0.5 0.5;                 /* pivot at the element's center */
    animation: spin 1s loop;         /* clip, duration, and loop|ping-pong|once */
}
```

The rotation pivots the element's **`Origin`** anchor (`origin: <x> <y>`, normalized 0..1 of the
box, `0 0` — the top-left corner — by default, so `origin: 0.5 0.5` spins about the center) and
turns the element's own primitives **and** its children together —
rotating a container spins its text, borders, gradients, and child images with it, composing
with `corner-radius`, borders, and MSDF glyphs by construction. It is **paint only**: the
element keeps its unrotated flex box, hit-testing stays axis-aligned against the unrotated layout
rect, and content clips stay axis-aligned scissors. For a per-frame angle from C++ (a needle
tracking a value), `Document::SetRotation(element, degrees)` writes it paint-only, with no layout
re-solve. `hello-triangle`'s HUD spins a conic-gradient loading arc this way.

## 3. Author the document

A `*.vui.xml` is a tree of elements. The **root** element carries a `stylesheets`
attribute — a space-separated list of `StyleSheet` asset ids — and any element references
a font through the `font:` inline-style property (an asset id). **A font inherits**: declaring it
once on the root serves every text-bearing element beneath it, and an element that declares its own
overrides it for its subtree. Element attributes are
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
`ProgressBar` (`value`), `TextInput` (`value`/`onChange`), `ScrollView`, `List` (a
data-bound repeater whose single child is the item template, and with `selection` a
selectable list view — see below), and `Table` (a
column-aligning row container — see below). Register the document in the pack as type
`UIDocument`; its font and stylesheet resolve as ordinary cook/load dependencies.

A `{obj.field}` value is a **binding** resolved against a bound view-model through
reflection; a literal (`min="0"`) is read once at instantiate. A binding path is a dotted
field path — `{player.health}` resolves `health` on the `player` field of the bound
object; a single segment (`{Level}`) resolves a field on the bound object directly.

### Repetition: `count` and `${}`

A fixed pool of identical elements — a strip of ticks, a row of ability slots, a set of
waypoint markers — is authored once and replicated at cook time with a `count` attribute.
`count="N"` (1–1024) on any non-root element unrolls that element **and its whole subtree** N
times, and `${…}` forms substitute the replica index into every attribute value and text of the
repeated subtree:

```xml
<Panel class="ticks">
  <Text class="tick" count="8">${n:02}</Text>   <!-- eight Texts: 01, 02, … 08 -->
</Panel>
```

- `${i}` is the **0-based** index, `${n}` the **1-based** index (`n = i + 1`).
- `${i:0W}` / `${n:0W}` **zero-pad** to width `W` (`${n:02}` → `01`, `02`, …).
- `$${` escapes a literal `${` where the text genuinely needs one.

Unrolling happens after the XML parse and before attribute interpretation, so by the time the
runtime loads the document the pool is just N ordinary sibling elements — the outline, the
layout, and the inspector see siblings, not a repeater. Nesting one `count` inside another, or a
`count` outside 1–1024, is a located cook error.

**`count` is for a fixed, imperatively-driven pool; `List` is for a runtime-varying array.**
Draw the line by where the length comes from: if the number of items is a bound array whose size
changes at runtime, use `List` (its single child is the item template, cloned per array
element, bound per row). If the number is fixed at author time and the game drives each element
by hand — lighting up the active tick, filling the charged slots — use `count`. The pool needs no
ids: `Document::FindAllByClass("tick")` returns every element carrying the class in tree order,
so the game resolves the whole pool once and drives it by index. `hello-triangle`'s HUD authors
its numbered tick strip this way.

### Scrolling: `overflow` and styleable scrollbars

Scrolling is a **style property**, not an element type — any element can scroll:

```css
.track-list {
    overflow-x: hidden;    /* visible | hidden | scroll */
    overflow-y: scroll;
    scrollbar: gutter;     /* overlay (default) | gutter */
}
```

`overflow` is the CSS two-value shorthand (`overflow: hidden scroll`, or one keyword for both).
Set the axes independently so a vertical list can't drift sideways when one row runs long.

`<ScrollView>` still exists as the **preset** — a `Panel` whose overflow defaults to `scroll` on
both axes — so existing markup is unchanged, and a `ScrollView` styled `overflow-x: hidden` still
takes the authored value. But a `<List>` no longer needs wrapping in one; style it and it scrolls.

**Scrollable content must hold its size.** A flex child shrinks to fit by default, so nothing ever
overflows to scroll. Give the content `flex-shrink: 0` (or an explicit size), exactly as you would
in CSS.

**The scrollbar is made of real elements, so you style it with ordinary selectors:**

```css
ScrollBar             { background: #00000040; }
ScrollBar.vertical    { width: 8px; }
ScrollBarThumb        { background: #ffffff60; corner-radius: 4px; transition: background 0.15s; }
ScrollBarThumb:hover  { background: #ffffffa0; }
List > ScrollBar      { width: 6px; }
```

A scrollable axis gets a `ScrollBar` carrying a `ScrollBarThumb`, tagged `horizontal` or
`vertical`. They take backgrounds, borders, corner radii, gradients, pseudo-state variants, and
transitions like any other element. You **cannot author them** — a `<ScrollBar>` tag is a cook
error — they exist only as the widget layer's own children.

A bar exists while its axis is styled `scroll` and hides itself when there's nothing to scroll, so
content growing past the box reveals it with no layout change. Drag the thumb, or click the track
to page.

`scrollbar: gutter` reserves the bar's width out of the content box instead of letting it overlay,
and holds that space whether or not the content currently overflows — which is what stops the
content jumping the moment it grows past the box. The reserved width **is** the bar's own styled
width, so `ScrollBar { width: 6px }` narrows both from one value.

### A selectable list view: `selection` and `:selected`

Give a `<List>` (or a `<Table>` repeating an array) a `selection` attribute and it becomes a
**list view** — a container the player picks items out of:

```xml
<List id="tracks" items="{Playlist.Tracks}" selection="extended"
      onSelectionChanged="TracksPicked">
  <Panel class="row">                            <!-- the item: a whole subtree -->
    <Image class="art" src="0x…"/>
    <Text class="title" text="{Title}"/>
    <Button class="remove" onClick="RemoveTrack">Remove</Button>
  </Panel>
</List>
```

```css
.row          { flex-direction: row; height: 32px; background: #202020; }
.row:hover    { background: #303030; }
.row:selected { background: #2266cc; color: #ffffff; }
```

**The unit of selection is the item, not a line of text.** An item is the whole authored
template subtree, so the row above — art, title, and a per-row button — is one selectable
thing, and `:selected` scopes its style exactly the way `:hover` scopes any other element's.
Clicking anywhere inside the row selects the row.

Pick the mode by the input device the screen is for:

| `selection` | A plain activation | Chords |
| --- | --- | --- |
| absent | nothing — a display repeater (the default) | — |
| `single` | replaces the one selected item | — |
| `multiple` | **toggles** that item | — (built for gamepad/touch, where there is no chord) |
| `extended` | replaces the selection | `Control`/`Meta` toggles one, `Shift` extends a range from the anchor |

A selectable list's items are **focus stops**, so arrow keys and a d-pad walk them, a focused
item inside a `<ScrollView>` scrolls itself into view, and confirm applies the mode's meaning.
Under `single` and an unmodified `extended` move the selection travels with focus; `Control` +
arrow moves focus alone so the player can reach an item and then toggle it.

Read the result back by **array index**, which is what the game already indexes its model with:

```cpp
for (const u32 index : document.GetSelectedItems(*document.FindById("tracks")))
{
    playlist.Tracks[index].Queued = true;
}
```

`GetSelectedItems` survives a re-sync — grow or shrink the bound array and the selection still
names the same rows, minus any the shrink removed. `SetSelectedItems` / `SelectItem` /
`ClearSelection` drive it from the model and deliberately fire **no** `onSelectionChanged`, so
pushing the model's selection into the document each frame never re-enters your own handler.

For the per-row button above, the handler receives the `Button` — ask the document which row it
came from:

```cpp
context.SetHandler("RemoveTrack", [&](Gui::Element& button)
{
    if (const optional<u32> row = document.GetItemIndex(button))
    {
        playlist.Tracks.erase(playlist.Tracks.begin() + *row);
        context.Invalidate();
    }
});
```

`GetItemIndex` works on any descendant of a `List`/`Table`, selectable or not — it is how a
control inside an item template says which item it belongs to.

### `Table` — column-aligned rows

A `<Table>` lays out and paints as a Panel (typically `flex-direction: column`), but each
direct child is a **row** (a `flex-direction: row` container) and the k-th in-flow cell of
every row is widened to that column's widest cell across the table — so the rows read as a
table without hand-pinning widths. A cell is any element; its margins count toward the
column but stay its own, so per-class cell spacing composes. A hidden or
absolutely-positioned child (a rule, an overlay) neither contributes to nor receives a
column width. A cell with a positive `flex-grow` is an **elastic filler**, not a column: it
absorbs its row's slack (give it a `flex-basis` for its minimum gap), so the fixed columns
after it right-anchor to the rows' shared right edge — the way a name column on the left
and numeric columns pinned to the panel's right edge coexist.

With an `items` binding a Table **repeats its authored child as the row template**, exactly
as a `List` does — one row per bound array element, each row's `{field}` bindings resolved
against its element. Without `items` its children are static, hand-authored rows.

Numeric columns pair the table with **`text-align`** (a `Text`-element style property:
`left`/`center`/`right`): a right-aligned cell's glyphs end at the cell box's right edge, so
a widened numeric column aligns its values' right edges. Alignment is paint-only — a
content-sized text box (no slack) draws identically under all three values.

```xml
<Table class="scores" items="{Standings}">
  <Panel class="score-row">
    <Text class="score-name" value="{Name}"/>
    <Text class="score-points" value="{Points}"/>   <!-- .score-points { text-align: right; } -->
  </Panel>
</Table>
```

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
    GetManagedViewports().Get(0)->AttachDocument(*m_Hud);
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

Hold the document as a member (`m_Hud`); the app destructor frees it before the context
tears down.

> A UI-only viewport must render over *something*: `Viewport::Render` composites a
> document only when the viewport has a scene to render over, so a viewport with no game
> world pushes an empty `Scene` (a cleared target). The template's managed viewport
> already renders the game world, so its HUD composites over the cube with no extra wiring.

## Driving a HUD

The four sections above cover the static shape. A live HUD adds a per-frame drive loop, and a
handful of small utilities absorb the boilerplate every first consumer otherwise hand-rolls.
They are **utilities, not a framework** — device-free value types you compose, never a system the
engine runs behind your back.

**`DocumentHost` owns the lifecycle.** Rather than hand-wiring load → instantiate → bind →
attach and re-attach on a viewport recreation, hold a `Gui::DocumentHost` (`Veng/Gui/DocumentHost.h`):
construct it with the asset manager, the type registry, and the document id; `Attach(viewport)`
once per frame loads lazily on the first call, keeps the tree attached, and re-attaches
transparently if the viewport is recreated. `SetContext(&context)` binds the view-model,
`SetInteractive(true)` opens input.

**`SetOnInstantiate` resolves your element pointers once.** The one-time "find the elements I
drive" step belongs in a callback the host runs after every (re)instantiate, so cached pointers
stay correct even when the document is rebuilt:

```cpp
m_Host->SetOnInstantiate([this](Gui::Document& doc) {
    m_Ticks = doc.FindAllByClass("tick");   // resolve the count-pool once; cache the vector
    m_Needle = doc.FindById("needle");
});
```

`FindAllByClass` and `FindById` are unindexed tree walks — resolve here and cache, never per
frame. Then each frame, drive the cached elements with the paint-only setters — `SetText`
(a no-op on unchanged text, so a naive per-frame write costs nothing), `SetRotation`,
`SetTextColor`, `SetBackground`, `SetImageUv` (an atlas flipbook) — none of which re-run layout.

**`Presence` eases an open/close.** `Gui::Presence` (`Veng/Gui/Presence.h`) chases a boolean goal
with a frame-rate-independent ease (`Math::ExpApproach`): `Update(open, delta)`, then apply
`GetAlpha()` as an opacity and `GetSlide(travel)` as a slide-in offset. `IsHidden()` reports when
the alpha has decayed far enough to stop laying the element out. The presence never touches a
document — you apply the alpha and slide yourself (`SetOpacity`, `SetVisible`, a placement
offset), so placement stays yours. `Gui::KeyedPresence<Key>` wraps it for a panel that shows one
of several interchangeable subjects: set the desired key each frame, and it closes over the stale
content, adopts the new key only once fully hidden, then reopens — the open animation replays with
fresh content instead of cross-fading. Refresh the bound content only while `GetShown()` equals
the desired key.

**`WorldToDocument` bridges a world point into HUD space.** A marker pinned to a world position
projects through the viewport. `Viewport::WorldToDocument(worldPos)` returns the point in
**document logical points** — the space your HUD lays out in — or `nullopt` when it is behind the
camera or off-region; `Viewport::GetDocumentExtent()` is the HUD's size in the same space. The
conversion is region framebuffer pixels ÷ the UI scale, the one HiDPI trap this bridge removes
(project in pixels, lay out in points, and a marker drifts on a Retina display). Place the marker
with the device-free helpers in `Veng/Gui/Placement.h`: `ClampIntoBounds(pos, size, bounds, margin)`
slides a rect the minimum amount to keep it fully on screen, and `AnchorBeside(anchor, size, offset,
bounds, margin)` offsets a card beside a point and clamps it in one call:

```cpp
if (const auto p = GetManagedViewports().Get(0)->WorldToDocument(targetWorldPos)) {
    const vec2 pos = Gui::AnchorBeside(*p, cardSize, {12, -8},
                                       GetManagedViewports().Get(0)->GetDocumentExtent(), 8.0f);
    doc->SetPlacement(*card, pos, cardSize);
}
```

The engine owns the coordinate bridge and the clamp; the projection policy (which world point,
what rejection margin) stays yours.

## 5. Make it interactive

A document is **display-only by default** — its bindings update and it draws, but it
hit-tests and takes no focus, so a HUD never steals input from gameplay. A menu becomes
interactive when the game opens a **`SeatFocusScope`** on the document's seat and flips
`SetInteractive(true)`:

```cpp
// While the menu is open — RAII: the takeover restores on scope exit.
m_MenuFocus.emplace(GetInputRouter(), seat, GetManagedViewports().Get(0), m_MenuContext);
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
