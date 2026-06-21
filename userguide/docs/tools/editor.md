# The editor

The editor is a separate executable, not part of the runtime. It's a
reflection-driven framework (`libveng_editor`) built on the engine — `Veng::UI`,
the scene renderer, the asset manager, and the reflection layer — plus the cooker,
for editing assets live.

`veng_add_editor(<name> SOURCES …)` builds an editor executable that loads your game
module the same way the launcher does, with the editor's hooks switched on so your
module can contribute panels and custom widgets.

## Host and panels

`EditorHost` is an `Application` subclass that sets up a docked, multi-panel window
and loads your module into it. A panel is an `EditorPanel` — a title and an
`OnImGui()` method, plus an optional `OnRender()` for a panel that draws into a
render target (a 3D viewport, for instance). The host ships with an asset browser
and a console out of the box.

## The prefab editor

The prefab editor is where you edit a scene. It spawns a prefab into a live scene
and gives you three panels over it:

- a **viewport** that renders the scene from an orbit camera, with a debug-view
  dropdown for inspecting g-buffer channels and the like;
- a **scene tree** for selecting, renaming, reparenting, reordering, and
  creating or deleting entities;
- an **inspector** for editing the selected entity's components.

## The inspector

The inspector is driven entirely by reflection. It walks the selected entity's
components and draws a widget for each field based on its
[field class](../scene/reflection.md#field-classes) — sliders for numbers, vector
editors, enum dropdowns, asset pickers, and so on — honoring a field's editor
metadata like read-only, hidden, and tooltips. Nested structs expand inline, and a
variant field lets you pick its type and edit the chosen one — this is what gives a
`Primitive` its shape picker and per-shape parameters.

You can replace the default widget for any type with
`EditorRegistry::RegisterFieldWidget`. Editing a recipe component — a primitive's
shape or size — rebuilds its mesh on the spot.

## Editing assets live

The editor can cook an edited asset on the fly and reload it without a restart. It
cooks off the render thread, mounts the result
[from memory](../assets/loading.md#mounting-an-archive-from-memory), and reloads
behind the existing handle, so your change appears immediately. The texture editor
is the simplest example: tweak its sRGB or filter settings and the preview updates
as you go.

## The node graph

The editor includes a reusable node-graph editor, with graphs saved as JSON. Node
types are described as data rather than written as subclasses: a node is a set of
typed pins plus a property struct, edited with the same widgets as a component.

The material editor is built on it: you wire up a graph that compiles to a
material's parameters, embedded in the `.vmat.json`, with a live preview rendering
the material on a sphere.
