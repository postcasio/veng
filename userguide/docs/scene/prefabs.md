# Prefabs & spawning

A **prefab** is a reusable recipe for a subtree of entities and their components,
authored as JSON, cooked into an asset, and spawned into a `Scene` at runtime.

## Authoring

A `*.prefab.json` lists entities, their components, and field values. The cooker
validates every component against your module's actual types (see
[The vengc cooker](../tools/vengc.md)), so an unknown component, a wrong field
type, or a bad value is caught at cook time with a clear error rather than at
runtime.

## Loading & spawning

A prefab loads like any other asset — see [Loading at runtime](../assets/loading.md)
— and the assets its components reference (a mesh, a material) come along as
dependencies. You spawn it into a scene:

```cpp
auto prefab = GetAssetManager().LoadSync<Prefab>(AssetId{0x…ULL});
vector<Entity> roots = prefab.value()->SpawnInto(scene, GetAssetManager());
```

`SpawnInto` creates the entities, fills in their components, fixes up any
references between them, and returns the root entities it created.

Spawning the same prefab twice gives you two independent copies — a prefab is a
reusable template, not a shared instance.

## Recipe components

A component can store a *recipe* and build what it describes when it spawns, rather
than storing a finished resource. The built-in `Primitive` is the example: it holds
a shape and its parameters — a cube, plane, sphere, or icosphere, plus a material —
so a prefab saves *"icosphere, radius 0.8, 4 subdivisions, brick material"* instead
of a baked mesh.

When the prefab spawns, `Primitive` builds the mesh from that recipe and assigns it
to the entity's `MeshRenderer`. Since the mesh streams in asynchronously, the
primitive appears a frame or two later, exactly as a loaded mesh would.

Each recipe builds its own mesh — there's no deduplication. If you want many
entities sharing one generated mesh, call `BuildPrimitiveMesh` once and assign the
result to each.

If you add or change a recipe component outside of spawning (in your own tools, for
example), call `ResolveComponents(scene, entity, assetManager)` to rebuild its
resource. The [editor](../tools/editor.md) does this automatically when you edit
one.

## Runtime meshes without the cooker

You can also build a mesh directly, with no cooker involved. `Primitives::Cube`,
`Plane`, and `Sphere` generate mesh data with proper normals, tangents, and UVs;
`Mesh::BuildSync` uploads it and hands you a `Ref<Mesh>`, while
`AssetManager::Build<Mesh>` does the same off the render thread.

A mesh built this way isn't an addressable asset and never touches a pack, but it's
otherwise interchangeable with a cooked one. Wrap it with `AssetManager::Adopt` to
get an `AssetHandle<Mesh>` you can drop into a `MeshRenderer` like any other.
