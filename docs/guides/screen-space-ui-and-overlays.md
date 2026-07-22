# Screen-space UI, level overlays, and scene captures

This guide covers three engine-driven scene facilities that a game reaches by **authoring
data on entities** rather than hand-wiring per-frame code:

- **Presenting screen-space UI** with a **`GuiOverlay`** component — the screen-space
  sibling of the world-space [`GuiSurface`](diegetic-ui.md).
- **Opening a whole level as a secondary, simulated overlay** with **`LevelOverlay`** — a
  menu, a picture-in-picture view, or a full-screen modal world running over the game.
- **Authoring a scene capture** with a **`CaptureSurface`** component — the
  render-to-texture member of the same family (a mirror, a monitor, an environment probe).

The live reference is the [template](../../examples/template/) module: its primary HUD is a
`GuiOverlay` on an entity in
[`assets/prefabs/scene.prefab.json`](../../examples/template/assets/prefabs/scene.prefab.json),
its mirror is a `CaptureSurface` on the same prefab, and a Tab key opens a **secondary
overlay level** ([`assets/levels/overlay.level.json`](../../examples/template/assets/levels/overlay.level.json)
+ [`assets/prefabs/overlay.prefab.json`](../../examples/template/assets/prefabs/overlay.prefab.json))
through `LevelOverlay`. Open [`main.cpp`](../../examples/template/main.cpp) beside this guide.

This guide builds on [authoring a UI document](authoring-ui-documents.md) (the markup,
stylesheet, and font are authored the same way) and is the screen-space companion to
[diegetic and glowing UI](diegetic-ui.md).

---

## The family: one discovery-and-drive pattern, three targets

planset-52's `GuiSurface` established the pattern: a **reflected scene component** the
**Viewport** discovers in the scene it renders (`world.View<GuiSurface>()`) and **drives**
each frame. Two more components join it, all discovered the same way and differing only in
where the engine drives them:

| Component | Target | Space |
|-----------|--------|-------|
| `GuiSurface` | a `Gui::Document` onto a **world mesh** | HDR, pre-bloom — it **glows** |
| `GuiOverlay` | a `Gui::Document` onto the **viewport layer stack** | LDR, post-tonemap — it does **not** glow |
| `CaptureSurface` | the **scene rendered into a texture** | sampled by the entity's material |

The engine owns the boilerplate every consumer used to hand-roll — load → instantiate →
attach for a document, build → register → rebind for a capture. The game owns only what the
engine cannot know: the **data binding** (for the UI members) and the **refresh policy** (for
the capture). None of the three is a new subsystem or a cooked-format change; each is an
additive reflected component authored in a prefab like any other.

---

## 1. Presenting screen-space UI — the `GuiOverlay` component

A HUD, a menu, a notification stack — screen-space UI painted over the finished 3D frame — is
authored as a **`GuiOverlay`** component (`Veng/Gui/Overlay.h`) on an entity:

```json
{
  "::Veng::Name": { "Value": "HUD" },
  "::Veng::GuiOverlay": {
    "Document": "0x…hud-document",
    "Layer": 0,
    "Interactive": false
  }
}
```

- `Document` is the cooked `UIDocument` recipe the overlay presents.
- `Layer` is the document's z-order in the viewport's layer stack — higher composites over
  lower (HUD under menu under notifications).
- `Interactive` gates input: `false` (the default) is display-only (it data-binds and draws
  but hit-tests nothing and takes no focus); `true` routes the claiming viewport's seat input
  into the document. A system may flip it at runtime.
- `TargetSeat` (an entity reference, omitted above) resolves *which* viewport claims the
  overlay under multi-viewport presentation (below).

The **Viewport discovers every `GuiOverlay` in the scene it renders and drives the ones it
claims** — lazy-loading the document, instantiating it, attaching it to the layer stack at
`Layer`, and re-attaching across a viewport recreation. So the load / instantiate / attach the
old `OnWorldLoaded` code hand-rolled is gone; the overlay is authored data.

### Why it does not glow (and `GuiSurface` does)

A `GuiOverlay` composites **after** tonemap, in LDR — the honest overlay: a color brighter
than white clamps at white, and there is nothing left to bloom. A `GuiSurface` composites into
the **lit HDR scene color before bloom**, so its bright content glows. Same document, same
authoring; the placement decides. If you want a glowing readout in the world, that is a
`GuiSurface` — see [diegetic and glowing UI](diegetic-ui.md).

### The C++ interface — a state component and a binding system

The engine never knows the game's model. The game binds the overlay through the **canonical
C++ interface**: a reflected **state component** holding the display data, and a small
**binding system** that hands it to the overlay and marks it dirty. There is **no HUD
controller object**.

The state is an ordinary reflected struct — its fields are what the markup's `{obj.field}`
bindings resolve against:

```cpp
struct TemplateHud
{
    string Caption = "warming up";
    f32 Level = 0.0f;
};
VE_REFLECT(::TemplateHud, 0x…)
VE_FIELD(Caption)
VE_FIELD(Level)
VE_REFLECT_END();
```

Binding is a `Gui::BindingContext` — the data object plus a table of named `onClick`/`onChange`
handlers. The overlay's `SetContext` / `SetOnInstantiate` are **callable before the first
render**: they are stored and applied when the Viewport instantiates the document, so a
binding pass that runs ahead of the first drive has no ordering hole. Bind once (in
`OnWorldLoaded`, or a system's first tick), then `Invalidate` each frame you mutate a field:

```cpp
void OnWorldLoaded(Scene& world, ResidencyBatch&) override
{
    m_Context.SetData(m_Model);                       // the reflected view-model
    for (auto [entity, overlay] : world.View<GuiOverlay>())
    {
        overlay.SetContext(&m_Context);               // deferred bind — no ordering hole
    }
}

void OnUpdate(f32 delta) override
{
    m_Model.Caption = fmt::format("{:.0f} fps", 1.0f / delta);
    m_Model.Level   = std::clamp((1.0f / delta) / 120.0f, 0.0f, 1.0f);
    m_Context.Invalidate();  // the Viewport's overlay drive re-resolves the dirtied bindings
}
```

An **`onClick`** handler is registered by name; markup `onClick="Dismiss"` fires the entry
named `"Dismiss"`, receiving the element that raised it:

```cpp
m_Context.SetHandler("Dismiss", [this](Gui::Element&) { m_CloseRequested = true; });
```

The engine drives layout and draw and re-reads only the dirtied bindings; the game supplies the
model and the handlers. That is the whole division: **the engine owns load → instantiate →
attach; the game owns bind → update.**

### The ergonomic path — a per-instance `GuiDriver`

The `SetContext` pattern above is fully supported, but it forces the game to find the overlay,
guard the one-time bind, and keep per-instance state in system members — which becomes an
entity-keyed map the moment two viewports claim two instances of one overlay. The **ergonomic
path is a driver**: a named, registered, **per-instance** presentation binding the engine
instantiates *with the document* and destroys *with it*. A `GuiOverlay` names one in a reflected
`Driver` field, and the game writes no find-and-bind system at all.

A `GuiDriver` (`Veng/Gui/Driver.h`) has two hooks — `OnInstantiate` (resolve elements and bind
the driver's own `Gui::BindingContext`, re-run on any re-instantiate) and `OnUpdate` (once per
frame while attached, handed a `GuiDriverFrame { Document, Scene, Seat, Delta, View }` with the
claiming viewport's real view). The template's `TemplateOverlayDriver` in
[`main.cpp`](../../examples/template/main.cpp) is the live reference — it seeds its model from the
populate-hook snapshot, binds the dismiss handler, and publishes the button press to a drained
channel:

```cpp
class TemplateOverlayDriver final : public GuiDriver
{
public:
    void OnInstantiate(Gui::Document& document, Scene& scene, Entity) override
    {
        if (const OverlaySnapshot* snapshot = scene.TryGetFirst<OverlaySnapshot>()) { m_Model = *snapshot; }
        m_Context.SetData(m_Model);
        m_Context.SetHandler("Dismiss", [this](Gui::Element&) { m_DismissRequested = true; });
        document.BindContext(&m_Context);          // the document supplies its own registry
    }
    void OnUpdate(const GuiDriverFrame& frame) override { /* publish state into a drained channel */ }
    // ... per-instance view-model in members ...
};

VE_GUI_DRIVER(TemplateOverlayDriver, 0x…ULL, "Template Overlay");   // mint the id with `vengc generate-id`
```

Mint the `GuiDriverId` with `vengc generate-id` (it is a leaf in the `SystemId`/`ActionId` id
family), register the driver in `VengModuleRegister` — `host->Drivers->Register<TemplateOverlayDriver>()`
(guard the null `Drivers` pointer; the launcher passes it, a bare host may not) — and name it on
the overlay, in the prefab or in C++:

```json
"::Veng::GuiOverlay": { "Document": "0x…", "Driver": "0x…", "Interactive": true }
```

Two viewports claiming one overlay (split-screen) become **two driver instances with independent
view-models**, so the per-instance state a binding system would key by entity dissolves. **The
boundary is concrete and checkable:** a driver reads scene state and *stamps* request/command and
`VE_VIEW_OUTPUT`-tagged components — but never writes a `VE_REPLICATED` or a Sim-input component
and never advances authoritative simulation. It is a presentation binding, not a controller;
gameplay stays components + systems (see
[Writing gameplay systems](writing-gameplay-systems.md)). Registering the driver requires the
module ABI at **version 6** — the `GuiDriverRegistry` is the host member whose addition bumped it.

### Multi-viewport: which viewport claims an overlay is decided by seat

A scene presented by more than one viewport (split-screen) resolves which viewport drives an
overlay by **seat**: a viewport claims the overlays whose target seat is its own bound seat.
The target seat is the entity's own seat when the `GuiOverlay` sits on a `Viewer` entity, else
its `TargetSeat` reference, else **unbound** — in which case the sole (or primary) presenting
viewport claims it (the single-viewport HUD case). So per-player HUDs fall out of authored data
(one overlay per seat), and a single overlay never thrashes between viewports.

### Layering *within* one document: the popup layer

The layer stack orders whole **documents** (HUD under menu under notifications). Ordering *inside*
one document — a dropdown that must cover its own siblings and escape a scrolling ancestor's clip —
is the **popup layer**, not a second overlay: `Document::OpenPopup(anchor, options)` pushes a
subtree that lays out against the document extent, paints after the whole main tree with no
inherited scissor, and hit-tests ahead of it, dismissed on an outside press or `Cancel` and closed
with its anchor. Reach for a second attached document when the content is genuinely a separate
screen; reach for a popup when it is a menu belonging to the document that opened it. Authoring one
is [Authoring a UI document](authoring-ui-documents.md), step 6.

---

## 2. Opening a level as an overlay — `LevelOverlay`

The `WorldRunner` ticks every open world, and `LevelOverlay` is the preset that opens one. To run
a **second, live scene over the running one** — a menu that is a real 3D scene, a picture-in-picture
camera, a full-screen modal — open a `Level` through the **`LevelOverlay`** RAII handle
(`Veng/LevelOverlay.h`). It is a thin preset over `WorldRunner::OpenWorld`, composing the owned-world
open with the overlay policy (register a viewport on top, hand off focus + the cursor seat, pause the
covered world) into one call.

### The overlay is a first-class level

Because the overlay is an ordinary `Level`, it composes its own systems, seat, and UI — nothing
is special-cased. The template's overlay level authors, in its prefab:

- an **input seat** (`Viewer` / `InputContextStack` / `PlayerInput` / `SeatInput`), so its
  interaction reads input from its **own** scene;
- an **`Interactive` `GuiOverlay` HUD** with an `onClick` button (its HUD comes free — the
  overlay's own viewport drives it, exactly as any scene's overlay);
- ordinary content (a spinning cube on a `ConstantMotion`, a camera, a light);

and names, in its `systems`, the builtin **`DeviceAssignmentSystem`** + **`InputMappingSystem`**
plus its own driving system. "Create a viewport, load a level with its systems, its seat, and
its HUD, and simulate it" is one `LevelOverlay::Open`.

### Opening and driving it

```cpp
m_Overlay = LevelOverlay::Open(*this, LevelOverlayInfo{
    .Source          = m_OverlayLevel,        // a resident AssetHandle<Level>
    .CoveredWorld    = GetManagedWorldId(),   // pause this world for the overlay's lifetime (invalid pauses none)
    .WaitForResidency = true,                 // block Open until the spawn is resident
    .Populate = [this](Scene& scene) {        // fill the overlay scene from host state, before it starts
        const Entity e = scene.CreateEntity();
        scene.Add<OverlaySnapshot>(e, OverlaySnapshot{ .Caption = m_Model.Caption });
    },
});
```

There is **no per-frame drive** — the `WorldRunner` ticks the overlay's world and the engine pushes
its camera each frame, exactly like any world, so the opener writes no `Update` call (there is no
`LevelOverlay::Update`). It only decides when to close.

Dropping the handle (`m_Overlay.reset()`, or `Close`) tears the overlay down and restores every
router / cursor-seat / world-pause value to the state captured at open, then closes the world.
Overlays **stack** — a second opened over the first (a dialog over a modal) nests through the
cursor-seat handoff and the focus stack — and the handles must drop in reverse open order (LIFO).

### The populate hook: contract versus guidance

The one seam through which host state enters the overlay is the **populate hook** — a
`std::function<void(Scene&)>` the engine runs **once**, after `LoadInto` and **before**
`StartSimulation`. That is the whole enforceable **contract**: the engine calls it with the
fresh scene and does not inspect what it attaches.

The **guidance** (not enforced): attach a thin **source component** — ideally an existing
component the covered world's systems already read, not an invented bridge type — and let a
**system named in the overlay's `systems`** build the rest by reading it **by component type**.
The template's hook copies a small `OverlaySnapshot` in; the overlay's own system reads it and
drives the HUD. What crosses the boundary, in either direction, is the game's decision — **no
overlay system reaches into the primary scene**.

**Freshness follows what the source holds.** A **copy** gives a frozen overlay (the value at
open, held for the modal's life). A **live** view is safe only when the shared thing is
immutable and shared by reference (a component holding a `Ref`/`shared_ptr`) or is refreshed
each frame by the opener's own code. A retained raw pointer into the primary scene's component
storage is **not** safe — a structural change there dangles it.

### Input suspend versus simulation pause — two separate knobs

Taking the overlay's seat **always suspends the covered world's input** (its seat's contexts
swap to an empty context beneath the focus scope). Freezing the covered world's **simulation**
is a **separate, opt-in** knob — name it as `CoveredWorld`, and the overlay holds a refcounted
`WorldRunner::PauseScope` on it for its lifetime. So a live picture-in-picture view keeps its world
ticking beneath it (leave `CoveredWorld` invalid), while a modal that should stop the world names
it. The template's modal names `GetManagedWorldId()`; a picture-in-picture overview whose scene must
keep resolving beneath the overlay leaves it invalid. Because the pause is a refcount, stacked
overlays over one world nest correctly and an explicit game pause is not clobbered.

### Reading results back

Results flow back through an **explicit game-owned channel**, never an overlay system reaching
across. The template's dismiss button raises a flag in an `OverlayControl` component the overlay
system writes and the **opener drains** each frame:

```cpp
const OverlayControl* control = m_Overlay->GetScene().TryGetFirst<OverlayControl>();
if (control != nullptr && control->Requested)
{
    m_Overlay.reset();   // the button closed the overlay
}
```

A callback, or the opener's glue writing the host directly, are equally valid channels — pick
one; the engine enforces none.

---

## 3. Authoring a scene capture — `CaptureSurface`

A reflective surface, a mirror, a monitor showing a security camera, an environment probe — an
effect that needs the scene **rendered into a texture** — is authored as a **`CaptureSurface`**
component (`Veng/Renderer/CaptureSurface.h`) on the entity whose material samples it, beside a
`MeshRenderer`:

```json
{
  "::Veng::MeshRenderer": {
    "Source": { "type": "::Veng::PlaneShape",
                "value": { "Size": [1.7, 0.96], "Material": "0x…mirror-instance" } }
  },
  "::Veng::Renderer::CaptureSurface": {
    "Shape": "PlanarReflection",
    "Resolution": 256,
    "Refresh": "EveryFrame"
  }
}
```

The engine **discovers** the component in the driven scene and **drives** it: it builds the
owned `SceneCapture` from the authored config on first sight, feeds it to the capture drive-list
(`RegisterCapture`) against the component's lifetime, and drops it — **self-unregistering** —
when the component, its entity, or its scene goes away. Each frame it renders the scene from the
entity's world position and **rebinds the capture's output handle onto the same-entity
material** — the `GuiSurface` locality rule: the handle is a runtime bindless slot, not a cooked
id, so it is rebound every frame. The material authors the named texture slot; the component
fills it. So a mirror is **authored data**: no app-side `RegisterCapture`, no per-frame game
code.

- **`Shape`** records how the material samples the capture — a reflective/refractive object by
  direction (`EnvironmentProbe`), a flat mirror or monitor by its surface parameterization
  (`PlanarReflection`). It is authored data for the material, not a switch on the engine's
  capture path (the engine renders one environment capture from the entity regardless).
- **`Resolution`** is the edge length in pixels of each captured cube face.
- **`Refresh`** is the policy: **`EveryFrame`** (a live mirror or moving probe) or **`OnDemand`**
  (render one full refresh, then idle until a system calls `MarkDirty` — a probe for a scene
  that changes rarely). A settled `OnDemand` capture records nothing until re-dirtied.

The output is an octahedral map (pre-tonemap linear HDR). Point the sibling material's sampled
texture slot at it (authored with a placeholder texture the component overrides each frame), and
the surface reflects, refracts, or displays the scene-from-here.

## Verifying it

The engine drives all three components automatically, so there is nothing to call each frame for
the authored HUD, the mirror, or the overlay level's own HUD. Build and run the
[template](../../examples/template/): its primary HUD renders over the scene, its mirror reflects
the cube, and **Tab** opens the secondary overlay level — its interactive HUD dismissable by the
Tab key **or** by its `Resume` button. In the editor, each component is inspectable like any
other — its `Document` / `Layer` / `Interactive` / `TargetSeat` (or `Shape` / `Resolution` /
`Refresh`) show in the reflection inspector, the referenced document opens in the
`UIDocumentEditorPanel`, and entering **Play** renders the entity's `GuiOverlay` over the scene.
