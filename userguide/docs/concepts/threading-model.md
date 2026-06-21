# The threading model

veng renders on a single thread, and that thread is the only one allowed to call
into the engine. Frame begin and end, draw recording, input, timing, and the UI
all assume it.

!!! warning "Don't call the API from another thread"
    The engine does no locking on this path. Calling in from another thread will
    corrupt state. Use the task system for off-thread work.

## Doing work off the render thread

When you need to do work without stalling the frame, hand it to the task system.
It runs the work on a pool of worker threads and gives you back a `Task<T>`. The
result is delivered to the render thread at the start of a later frame, so your
continuation always runs back on the main thread.

```
main thread:   ──▶ frame ──▶ frame ──▶ frame ──▶ …
                                ▲
worker pool:   decode + upload ─┘   (result delivered here)
```

This is how asset loading works: the decode and GPU upload happen on a worker, and
the asset becomes usable on the render thread once it's done. See
[Loading at runtime](../assets/loading.md).

## Async by default, blocking on request

The operations that can run off-thread are asynchronous by default, and each has a
blocking version with a `Sync` suffix:

| Async (default) | Blocking |
| --- | --- |
| `AssetManager::Load<T>` | `AssetManager::LoadSync<T>` |
| `AssetManager::Build<T>` | `AssetManager::BuildSync<T>` |
| `Buffer/Image::Upload` | `Buffer/Image::UploadSync` |

The async call returns immediately and the asset finishes loading in the
background; the `Sync` call blocks until it's done.
