# Plan 04 — Runtime: `AssetManager`, `AssetHandle`, pack mount, `LoadSync`

**Goal:** the engine-side registry. `AssetManager` (owned by `Application`, given
an explicit `Context&` — the planset-4 lesson) **mounts** cooked archives, resolves
`AssetId`s against them, and synchronously loads assets via a per-type **loader**
table, handing back a typed, refcounted `AssetHandle<T>`. Introduce the structured
`AssetLoadError`. No concrete asset types yet beyond a `Raw` blob handle for
testing — 05–08 register real loaders into this frame.

## Why this is its own plan

This is the second contract of the planset (alongside the format). Settling the
handle semantics, the loader-table dispatch, the mount/resolve path, and the error
type *before* any concrete type means each type plan adds only a `Load()` body and
a registry line — symmetric with the cooker's importer table. It also lands the
`AssetLoadError` promotion the roadmap has long flagged, with a real first user.

## API

```cpp
namespace Veng
{
    enum class AssetError { NotFound, WrongType, Corrupt, VersionMismatch,
                            MissingDependency, LoadFailed };
    struct AssetLoadError { AssetError Kind; AssetId Id; string Detail; };

    template <typename T>
    class AssetHandle                       // typed, refcounted, indirection into the cache
    {
    public:
        [[nodiscard]] bool     IsLoaded() const;
        [[nodiscard]] AssetId  Id() const;
        [[nodiscard]] const T* Get() const;     // nullptr until resident
        const T* operator->() const;            // asserts resident (engine contract)
        explicit operator bool() const { return IsLoaded(); }
    };

    struct AssetManagerInfo { /* mount list, or mount calls below */ };

    class AssetManager
    {
    public:
        AssetManager(Context& context, const AssetManagerInfo& info = {});
        ~AssetManager();

        Result<void> Mount(const path& archive);   // open a .vengpack, index its TOC
        void         Unmount(const path& archive);

        // Synchronous: resolve id → blob → loader → GPU resource → cached handle.
        // Recursively loads dependencies (mesh→material→texture/shader) synchronously.
        template <typename T>
        Result<AssetHandle<T>, AssetLoadError> LoadSync(AssetId id);

        template <typename T>
        optional<AssetHandle<T>> Get(AssetId id) const;   // cached only, no load

        void CollectGarbage();   // evict zero-ref assets (deferred past frames-in-flight)
    };
}
```

- **`AssetManager` is not async this planset** — only `LoadSync`. The name is the
  honest one and stays free when the threading planset adds the async default
  `Load` (see [asset-system.md](../future/asset-system.md) — the marked-verbose
  sync spelling is deliberate so this code never gets renamed out of the default).
- **`AssetHandle<T>` is indirection into the cache**, not a `Ref<T>` itself. The
  engine resources it ultimately owns (`Ref<Image>`, `Ref<Buffer>`) follow the
  existing [ownership rule](../../docs/ownership.md) unchanged. This indirection is
  what later enables hot-reload (swap behind the handle) — even though reload isn't
  built here.

## The loader table (mirror of the cooker's importer table)

```cpp
class AssetLoader   // engine-side; the only place that touches Context for a type
{
public:
    virtual ~AssetLoader() = default;
    [[nodiscard]] virtual AssetType Type() const = 0;
    // cooked blob (assetformat layout) → live engine resource. May LoadSync deps.
    virtual Result<RefAny, AssetLoadError> Load(AssetManager&, Context&,
                                                AssetId, span<const u8> cooked) const = 0;
};
```

`AssetManager` holds a `map<AssetType, Unique<AssetLoader>>`, registers built-in
loaders in its constructor (none real yet — a `Raw` loader for the test), and
`LoadSync<T>` maps `T → AssetType` via a small trait, finds the TOC entry across
mounted archives, checks the type, fetches the cached entry or calls the loader,
and caches the result keyed by `AssetId`.

## Lifetime / GC (the only lifetime work this planset does)

- **Refcount via the handle.** The cache entry holds the live resource + a refcount
  driven by `AssetHandle` copies. When it hits zero the asset becomes *evictable*,
  not freed.
- **Eviction is deferred through the existing retire queue.** `CollectGarbage()`
  drops the `Ref`s of zero-ref entries; the engine resources they held retire via
  the **existing per-frame deferred-destruction path** (planset-1/04) — the same
  mechanism, so an in-flight frame never samples a freed image. No second
  reclamation system, no manual keep-alive list.
- **No hot-reload** (needs a watcher + the async swap path — future). The handle
  indirection is built to *allow* it later; `Reload` is not implemented.

## Work

1. **`AssetHandle<T>` / `WeakAssetHandle<T>`**, `AssetId` already from
   `assetformat`. Public headers — Vulkan-free (guarded by `include_hygiene`).
2. **`AssetLoadError` + `AssetError`** in a public header; this is the structured
   error promotion (`Result.h` cross-ref).
3. **`AssetManager`** + the loader table + mount/resolve + `LoadSync` + `Get` +
   `CollectGarbage`, with a `Raw` loader (blob → an opaque `RawAsset { vector<u8> }`)
   so the path is testable before real types.
4. **`Application` wiring** — owns the `AssetManager` (constructed after the
   `Context`); apps release all handles in `OnDispose` before teardown (extend the
   ownership contract / smoke checks). Document in the same pass.
5. **Tests** — a `tests/gpu` (or a no-GPU `assetmanager`) case: build a `raw`
   archive (reuse plan 03's path or a fixture), mount it, `LoadSync<RawAsset>` a
   present id (bytes match), a missing id (`NotFound`), a wrong type (`WrongType`),
   and prove a dropped handle becomes evictable and `CollectGarbage` reclaims it.

## Dependencies

Plans 01, 02 (`AssetId`, `ArchiveReader`, blob layouts). Independent of the cooker
(can load a fixture archive). Blocks 05–08 (they register loaders here).

## Acceptance

- Clean build, `ctest` green incl. the new AssetManager tests.
- `include_hygiene` still green (the new public headers leak no backend).
- Error-kind branching demonstrated (NotFound / WrongType at minimum).

## Notes

- **`RefAny`** is an internal type-erased owner the loader returns and the cache
  downcasts per `T` (or templated loaders keyed by `AssetType`); keep it private to
  the manager. The public surface is only `AssetHandle<T>`.
- Dependency loads are **synchronous and eager** here: loading a material loads its
  shader + textures before returning. A `MissingDependency` is raised if a
  referenced id isn't in any mounted archive — first-class, not a string.
- Keep the loader table the *only* place a type touches `Context`; mirrors how the
  cooker keeps GPU-free `Cook` separate.
