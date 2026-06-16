# Plan 03 — cook-on-demand plumbing

**Goal:** wire `libveng_cook` into the editor host so source assets can be cooked on demand,
off the render thread, with the result hot-reloaded into the running `AssetManager`. This is
the infrastructure the texture editor (plan 04) drives; the full texture editor loop lands
there.

## `libveng_cook` linked into the editor

`libveng_cook` is a CMake SHARED library (built behind `VENG_BUILD_TOOLS`). The editor exe
target links it PRIVATE. `libveng_editor` itself does **not** link it — the editor library
stays importer-free; cook-on-demand calls are issued from the editor exe layer only.

The editor exe's `CMakeLists.txt` (emitted by `veng_add_editor`) links:
```cmake
target_link_libraries(${NAME}-editor PRIVATE
    veng::veng
    veng_editor::veng_editor
    veng_cook::veng_cook)
```

`libveng` and `libgame` never see `libveng_cook` — the planset-5 boundary holds.

## `CookSession`

A new type in the editor exe layer (not in `libveng_editor`):

```cpp
struct CookRequest
{
    path    SourcePath;   // e.g. assets/textures/brick.tex.json
    AssetId TargetId;     // the AssetId the cooked blob should be addressable as
    AssetType Type;
};

class CookSession
{
public:
    // Submit a cook request; runs on a task worker. Returns a Task<> whose
    // continuation fires on the main thread with the cooked archive bytes or
    // an error string.
    Task<Result<vector<u8>>> Cook(const CookRequest& request, TaskSystem& tasks);
};
```

`CookSession::Cook` submits a `TaskSystem::Submit` lambda that:
1. Calls the appropriate cooker importer (e.g. `TextureImporter::Import`) with the source
   path and a staging `ArchiveWriter`.
2. Serializes the writer into a `vector<u8>` in-memory archive.
3. Returns it as `Result<vector<u8>>`.

The main-thread continuation (from `Task<>::Then`) mounts the in-memory archive and
triggers a hot-reload.

## Hot-reload via in-memory archive

`AssetManager` does not currently support mounting in-memory archives — it mounts `path`-
addressable `.vengpack` files. Extend it with:

```cpp
// Mount an in-memory archive. The bytes are owned by the AssetManager.
// The archive is addressable by the AssetIds it contains, shadowing any
// on-disk version. Returns a MountHandle that unmounts the archive on destruction.
MountHandle MountMemory(vector<u8> archiveBytes, string debugName);
```

The `MountHandle` (a RAII token) unmounts the in-memory archive on drop, freeing the bytes.
The editor holds one `MountHandle` per in-flight asset — when a recook completes it replaces
the old `MountHandle` with the new one (the old archive bytes are freed via the handle drop).

After `MountMemory`, calling `AssetManager::Load<T>(targetId)` resolves from the in-memory
archive (shadow priority over the on-disk pack). The existing async `Load` path is
unchanged; the editor issues a new `Load` for the target id and the result fires the preview
update.

**Alternative considered:** patching the on-disk `.vengpack` in place. Rejected — modifying
the source pack while the runtime has it mapped risks corruption and complicates the
revert path. In-memory shadow archives are isolated: mount, preview, discard on revert, or
promote to disk on save.

## `EditorHost` integration

`EditorHost` gains a `CookSession m_CookSession` and exposes:

```cpp
// Called by a panel (e.g. the texture editor) to cook a source asset.
// The callback fires on the main thread after the cook completes.
void RequestCook(const CookRequest& request,
                 function<void(Result<MountHandle>)> onComplete);
```

`RequestCook` submits the cook task and arranges the main-thread callback via the existing
`TaskSystem::PumpMainThread()` continuation path. The callback receives either a live
`MountHandle` (mounted, ready to `Load`) or an error.

## Error surfacing

Cook errors (invalid JSON, missing source file, import failure) surface in the
`ConsolePanel` via `Log::Error`. The panel that requested the cook also receives the error
through its `onComplete` callback so it can show an inline error state.

## Tests

- The cook-on-demand path is exercised end-to-end by the texture editor in plan 04. This
  plan verifies the plumbing at the unit level: a `CookSession::Cook` call for the
  `brick.tex.json` source asset succeeds and produces a non-empty byte vector on the test
  thread (a new `cooker`-suite unit test, headless, no GPU required — the texture importer
  does not call into `libveng`).
- `ctest --output-on-failure` green; smoke PPM unchanged; `include_hygiene` green.

## Acceptance

Clean build; `libveng_cook` linked into the editor exe; `AssetManager::MountMemory` and
`MountHandle` implemented; `CookSession::Cook` submits off-thread and delivers a mounted
in-memory archive to the main thread; the cooker unit test passes; `ctest` green; smoke
unchanged. Commit: `Plan 03: cook-on-demand plumbing — CookSession, AssetManager::MountMemory,
EditorHost::RequestCook`.
