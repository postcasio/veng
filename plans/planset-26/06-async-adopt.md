# Plan 06 — `AssetManager::CreateAsync`

**Goal:** an async sibling of `Adopt` that returns an `AssetHandle<T>` immediately and
fills it from a `Task<Ref<T>>` later, so a runtime-created resource streams in with the
same handle semantics as a cooked-asset `Load` — `IsLoaded()` is false until the work
lands, then flips true. Pure asset-system work — **independent of the variant arc**; runs
in parallel with plans 01–04, alongside plan 05. Deduplication is **not** here — it lives
in plan 07's caller-owned `PrimitiveMeshCache`; `CreateAsync` itself, like `Adopt`, does
not dedup.

## Why this is its own plan

The cache-entry lifecycle (a *detached* but *pending* entry, finalized through the pump)
is the subtle piece. `Load` makes pending entries that live in the `AssetId` map;
`Adopt` makes detached entries that are already resident. `CreateAsync` is the
intersection — detached **and** pending — and that lifecycle deserves its own plan and
tests, separate from the mesh specifics (plan 05) and the primitive specifics (plan 07).

## What residency already is

```cpp
struct AssetCacheEntry { AssetId Id; AssetType Type; RefAny Resource; }; // Resource null until resident
bool AssetHandle<T>::IsLoaded() const { return m_Entry && m_Entry->Resource != nullptr; }
```

`Adopt` creates a detached entry with `Resource` already set. The async `Load` path
creates an entry with `Resource == nullptr`, returns the handle, and a main-thread
continuation assigns `Resource`. `CreateAsync` combines them: a detached entry created
**empty**, finalized by a continuation.

## The API — `AssetManager.h`

```cpp
/// @brief Wraps a not-yet-built runtime resource in an AssetHandle<T> that becomes
///        resident when `factory` completes.
///
/// Returns immediately with a pending handle (IsLoaded() == false). The factory's Task
/// runs on the task system; its result is assigned into a detached cache entry through
/// the main-thread continuation pump, after which IsLoaded() is true. Like Adopt, the
/// entry carries the invalid AssetId and is never inserted into the AssetId map, so a
/// reflective serializer records it as "no asset" and CollectGarbage() leaves it alone;
/// it stays alive exactly as long as a handle references it.
template <class T>
[[nodiscard]] AssetHandle<T> CreateAsync(Task<Ref<T>> factory);
```

### Implementation

1. Create a detached `AssetCacheEntry{ .Id = AssetId{}, .Type = AssetTypeTrait<T>::Type,
   .Resource = nullptr }`. Hold it alive through the pending window with a manager-owned
   strong `Ref` — reuse the existing `PendingLoad` list `Load` uses (the second `Ref` that
   already keeps a pending entry off `CollectGarbage`'s `use_count() == 1` eviction). The
   entry is detached: it is **never** inserted into the `AssetId` map.
2. Return `AssetHandle<T>(AssetId{}, entry)` immediately (`IsLoaded() == false`).
3. Attach a main-thread continuation to the factory task — `factory.Then([entry](Result<Ref<T>> r){ … })`
   drained by `TaskSystem::PumpMainThread()` — that assigns
   `entry->Resource = std::static_pointer_cast<void>(r.value())` and then drops the
   manager's keep-alive `Ref`. After the continuation runs, `IsLoaded()` is true and the
   entry reverts to the `Adopt` lifetime — alive exactly as long as a handle references it.

**Main-thread invariant.** The detached-entry creation, the `PendingLoad` bookkeeping, and
the continuation that mutates `entry->Resource` all run on the render thread — the worker
only produces the `Ref<T>`. This matches the single-threaded asset-system contract; no
asset-cache state is touched off-thread. Reuse the existing finalization path `Load` uses;
do not introduce a second mechanism.

**GC ordering.** While pending, the entry's `use_count()` is ≥ 2 (cache-less detached entry
+ the `PendingLoad` keep-alive + any live handle), so `CollectGarbage()` never evicts a
still-pending entry. Once the continuation drops the keep-alive, a resolved entry with no
live handle becomes collectible on the next `CollectGarbage()` — exactly the `Adopt`
behavior its GC test pins.

## Tests — `tests/unit` (+ a `gpu` smoke)

- **Pending → resident.** A `CreateAsync` over a task that resolves after K pumps yields
  a handle that is `!IsLoaded()` before and `IsLoaded()` after the pump; `Get()` is null
  then non-null.
- **Detached / GC-safe.** The entry is never in the `AssetId` map; `CollectGarbage()`
  with no live handle does not touch a still-pending entry, and frees a resolved one once
  the last handle drops (mirror the `Adopt` GC test).
- A `gpu` smoke wiring `Mesh::CreateAsync` (plan 05) through `CreateAsync` to confirm the
  end-to-end pending-mesh-handle path.

(Deduplication is tested in plan 07, against its `PrimitiveMeshCache`.)

## Acceptance

- Clean build; `ctest -L unit` green (+ the `gpu` smoke skips cleanly with no device).
- `CreateAsync` returns a pending handle that becomes resident through the main-thread
  pump with no blocking; the entry is detached, GC-safe like `Adopt`'s, and never evicted
  while pending.
