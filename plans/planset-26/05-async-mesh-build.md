# Plan 05 — Async `Mesh` upload factory

**Goal:** add an async sibling to planset-7's blocking `Mesh::Create(context, data,
name)`. It runs the geometry copy off the render thread on a task-system worker and
returns a `Task<Ref<Mesh>>` that yields the resident mesh — the same shape the async
`MeshLoader` path already uses. Pure asset/renderer infrastructure — **no dependency on
the variant arc**; runs in parallel with plans 01–04.

## Why this is its own plan

planset-7 deliberately shipped only the blocking factory and noted "an async overload of
the factory is a later addition with no change to either [`MeshData` or the generators]"
(planset-7 README, decision 4). This is that addition. It is the GPU half of the
streaming story; plan 06 is the cache-entry half, and plan 07 joins them.

## The factory

Add an async overload beside the existing `Mesh::Create`:

```cpp
/// @brief Builds a resident Mesh from CPU geometry off the render thread.
///
/// Submits one worker job that creates the canonical vertex + index buffers, memcpys the
/// geometry into them (the buffers are HOST_VISIBLE | HOST_COHERENT, so the copy is a
/// plain memcpy — no staging, no transfer-queue command, no device wait), folds the
/// bounds, and assembles the Ref<Mesh>. The returned Task yields that Ref; a caller
/// publishes it to the render thread through the continuation pump (see
/// AssetManager::CreateAsync). The blocking sibling is Mesh::Create(context, data, name).
/// @return A Task yielding the resident Ref<Mesh>.
[[nodiscard]] static Task<Ref<Mesh>> CreateAsync(
    Renderer::Context& context, TaskSystem& tasks, MeshData data, string name);
```

`MeshData` is taken **by value** (moved into the worker job) because the source must
outlive the caller's frame; the embedded `vector<AssetHandle<Material>>` only has its
atomic refcount bumped across the thread boundary (its cache entries are never
dereferenced on the worker), so the move is safe.

### Implementation — `engine/src/Asset/Mesh.cpp`

The model mirrors `MeshLoader`'s async branch (`engine/src/Asset/Loaders/MeshLoader.cpp`),
which already creates the buffers, copies, and assembles the `Mesh` inside the worker job —
nothing is on the transfer queue and there is no fan-in of separate upload tasks.

1. Run the same validation the sync factory does (non-empty vertices/indices, in-range
   indices, valid submesh `MaterialIndex`). It is a misuse check → `VE_ASSERT`, run on the
   calling thread before scheduling (a fatal is clearer eagerly than inside a worker).
2. `return tasks.Submit([context, data = std::move(data), name = std::move(name)] { … })`,
   whose body, on the worker:
   - Creates the vertex `Buffer` (`BufferUsage::Vertex | TransferDst`) and the typed
     `IndexBuffer` (`IndexType::U32`). (VMA allocation is thread-safe; this matches the
     async `MeshLoader`, which creates its buffers on the worker too.)
   - `UploadSync`s the vertex bytes (the `vector<CanonicalVertex>` as a byte span) and the
     `u32` index bytes — a host-visible memcpy, no GPU command.
   - Folds `Mesh::ComputeBounds` over the canonical vertices and each submesh range.
   - Assembles `MeshInfo` (carrying `data.Materials`, the submesh table — synthesizing the
     whole-range unassigned submesh when empty, exactly like the sync path —
     `CanonicalLayout()`, and the bounds) and returns `Mesh::Create(MeshInfo)`.
3. The returned `Task<Ref<Mesh>>` resolves once the worker job completes. Publication to
   the render thread (assigning the `Ref` into a cache entry's `Resource`) is plan 06's
   `CreateAsync` continuation — this factory does not touch the asset cache.

There is no `Buffer::Upload`/`Task<void>` fan-in and no `when_all` (the task system has no
such combinator): the whole build is one worker job that returns the `Ref<Mesh>`.

### Bounds

The sync path folds `Mesh::ComputeBounds` over the canonical vertices and each submesh's
range at construction; the worker job does the same from the CPU `MeshData`, so the
resident `Mesh` carries correct `GetBounds()` / per-submesh bounds the broadphase reads —
a streamed mesh is broadphase-correct the instant it goes resident.

## Tests — `tests/gpu`

Add a GPU case (labelled `gpu`, skips with no ICD):

- A hand-built two-triangle `MeshData` → `CreateAsync` → pump the task system until the
  `Task` resolves → assert the `Ref<Mesh>` has the right index count, one submesh, its
  materials carried, the canonical layout, and a non-empty `GetBounds()`.
- The same `MeshData` through `CreateAsync` and through the blocking `Create` produce
  meshes with identical index count, submesh table, and bounds (the async path differs
  only in *when*, not *what*).

## Acceptance

- Clean build; `ctest` green; the new GPU case passes (and skips cleanly with no device).
- `Mesh::CreateAsync` returns a `Task<Ref<Mesh>>` that resolves to a resident, drawable
  mesh built on a worker with no `WaitIdle` on the render thread.
- The build is a host-visible memcpy (no staging, no transfer-queue copy); `VE_DEBUG`
  validation gate stays clean — no allowlist change.
- The async mesh's bounds match the blocking path's.
