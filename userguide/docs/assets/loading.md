# Loading at runtime

At runtime your app **mounts** a cooked archive and resolves assets against it by
opaque id. Loading is async by default and streams off the render thread; a
blocking sibling exists for tests, tools, and the smoke path.

## `AssetId` and the manager

An asset is identified by an `AssetId` — a 64-bit value, written in hex in C++. The
asset manager is owned by `Application` and reached through `GetAssetManager()`.

```cpp
constexpr AssetId BrickMaterial{0x…ULL};
```

## Async vs sync

```cpp
// Async (default): returns immediately, not yet resident.
AssetHandle<Material> mat = GetAssetManager().Load<Material>(BrickMaterial);
// ... later ...
if (mat.IsLoaded()) {
    use(mat);
}
```

`Load<T>` returns a handle immediately and does the decode and upload on a worker,
without stalling the frame. Check `IsLoaded()` before using the result. The
renderer skips a mesh that hasn't finished loading, so a streamed-in asset appears a
frame or two later.

```cpp
// Blocking: returns a resident handle or a structured error.
AssetResult<AssetHandle<Material>> r = GetAssetManager().LoadSync<Material>(BrickMaterial);
if (!r) {
    Log::Error("load failed: {}", /* branch on */ r.error().Kind);
    return;
}
AssetHandle<Material> mat = r.value();
```

`LoadSync<T>` runs everything inline and returns either a resident handle or a
structured error you can branch on.

## Asset handles

An asset stays loaded as long as you hold an `AssetHandle` to it. Once nothing
references it, `CollectGarbage()` frees it.

## Dependencies come along

An asset loads its dependencies with it. Loading a material brings in its textures
and shader; loading a mesh brings in its materials; loading a prefab brings in
everything its components reference. You load the top-level asset and get the whole
graph.

## Adopting a resource you built yourself

A resource you create at runtime — a generated mesh, say — isn't in a pack, but you
can still hand it to the asset manager with `Adopt` to get an `AssetHandle` for it,
usable anywhere a loaded one is. (`AssetManager::Build<T>` builds and adopts in one
step.) An adopted asset has no id, so it isn't a content reference you can save to
disk; it lives only as long as a handle holds it.

## Mounting an archive from memory

`MountMemory` mounts an in-memory archive on top of the on-disk ones, so ids it
contains resolve to it first. It returns a handle you drop to unmount. This is how
the [editor](../tools/editor.md) does cook-on-demand: it cooks an edited asset into
an in-memory archive, mounts it, and reloads behind the existing handle, so your
change shows up live.
