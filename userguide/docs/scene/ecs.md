# Entities & components

A `Scene` is a runtime ECS world. It's single-owner — you create one and spawn
into it, and a scene is never loaded from disk.

```cpp
auto scene = Scene::Create(GetTypeRegistry());
```

The `TypeRegistry` you pass must outlive the scene and must already have every
component type registered — see [Reflection](reflection.md).

## Entities

An `Entity` is a generational handle — an index plus a generation, with
`Entity::Null` for the empty one. The generation is what catches use-after-free:
accessing an entity whose slot has been recycled is a fatal assert, not silent
corruption.

## Components

A component is just a reflected struct the scene stores in a pool, created the
first time you add that type. The per-entity API is templated:

```cpp
Entity e = scene->CreateEntity();
scene->Add<Transform>(e, Transform{ .Position = {0, 1, 0} });
scene->Add<MeshRenderer>(e, MeshRenderer{ .Mesh = meshHandle });

if (scene->Has<Transform>(e)) {
    Transform& t = scene->Get<Transform>(e);
    t.Position.y += 1.0f;
}
```

The per-entity methods are `Add`, `Remove`, `Get`, `TryGet`, and `Has`.

## Queries

Multi-component queries iterate the entities that have *all* the named
components, driving off the smallest participating pool:

```cpp
for (auto [e, transform, renderer] : scene->View<Transform, MeshRenderer>()) {
    // transform and renderer are references into their pools.
}
```

`View<Ts...>` is range-for and supports `break`; `Each<Ts...>` is the callback
form.

!!! danger "No structural changes during iteration"
    Adding or removing components, or destroying entities, **mid-`View`/`Each`**
    is API misuse — the single-threaded model offers no re-entrancy guard.
    Collect what you intend to change, then change it after the loop.

## The hierarchy

Parent/child relationships live in a `Hierarchy` component. You change the tree
only through scene operations, which keep the links consistent:

- `SetParent(child, parent)` — reparent (a null parent moves the child to the root);
- `Detach(child)`;
- `MoveBefore(child, sibling)` — reorder, as the editor does on drag.

Read it back with `GetParent(entity)` and `ForEachChild(entity, fn)` (in order).
Creating a cycle is a fatal assert. `DestroyEntity` removes the entity's whole
subtree, unlinking it from its parent cleanly first.

World transforms are computed on demand by walking up the parent chain; there's no
cached world matrix to keep in sync.

## The spatial version

A scene tracks a version number (`GetSpatialVersion()`) that moves whenever the
spatial state — transforms, hierarchy, or mesh renderers — might have changed. The
[scene renderer](../rendering/scene-renderer.md) uses it to skip rebuilding its
culling tree for a scene that hasn't moved.

One consequence:

!!! warning "Write transforms through the scene each frame"
    Holding onto a `Transform&` across frames and mutating it directly skips the
    version bump, which can leave culling stale. Fetch the transform through the
    scene each frame — as all the engine and sample code does — rather than caching
    the reference.

## The builtin components

The builtins are plain reflected components, pre-registered exactly like a game's
own:

| Component | Carries |
| --- | --- |
| `Name` | A display label. |
| `Transform` | Local TRS — `Position` / `Rotation` / `Scale`. Never a world matrix. |
| `Hierarchy` | The scene-graph link (parent + ordered children). |
| `Camera` | `FovY` / `Near` / `Far`; builds a `CameraView` from its world transform. |
| `MeshRenderer` | The `AssetHandle<Mesh>` a draw queries. The mesh owns its materials. |
| `Light` | A directional light — `Direction` / `Color` / `Intensity`. |
| `Primitive` | A procedural-mesh *recipe* (cube/plane/sphere/icosphere) that resolves to a mesh at spawn. |
