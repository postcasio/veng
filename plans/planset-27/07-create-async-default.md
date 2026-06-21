# Plan 07 — `Create` for GPU resources, `Build` for engine assets

**Goal:** split the factory surface along the seam it actually has — **low-level GPU
resources** vs **higher-level engine assets**. A GPU resource is *constructed from a
descriptor*: `Create(const XInfo&)`, synchronous, returns a ready `Ref<T>` (there is
nothing to await). An engine asset is *produced from CPU source data it uploads*:
`Build(...)`, async by default (returns `Task<Ref<T>>`), with `BuildSync(...)` the
blocking sibling — the same async-default rule as `Load`/`Upload`. Rename the asset
build factories (`Mesh`/`Texture`/`Material`) off the overloaded `Create`/`CreateAsync`
onto `Build`/`BuildSync`, fold `AssetManager::CreateAsync<T>` into an
`Adopt(Task<Ref<T>>)` overload, and leave every `Create(XInfo)` GPU-resource factory
untouched. Runs **last** — a mechanical sweep over the final factory set (including
plan 06's new factories and plan 02's `CreatePrimitiveMesh`).

## Why this is its own plan

It is a wide, purely mechanical rename touching ~87 call sites across engine, editor,
examples, and tests — the kind of sweep to isolate from the feature plans. And it
draws a conceptual line worth stating once and cleanly: `Create` makes a GPU object,
`Build` makes an engine asset. Landing it last lets it cover every factory the planset
has by then created.

## The split — root `CLAUDE.md`

State the two tiers in the resource-ownership / async section:

> **`Create` constructs a GPU resource; `Build` produces an engine asset.** A
> low-level GPU resource — `Buffer`, `Image`, `ImageView`, `Sampler`, `Shader`, the
> pipelines, `DescriptorSet`, `Fence`, `Semaphore` — is constructed from its
> descriptor through `X::Create(const XInfo&)`: synchronous, returning a ready
> `Ref<T>`, because constructing the object is immediate and has no async form.
>
> A higher-level engine **asset** that carries CPU source data and *uploads* it —
> `Mesh` (from `MeshData`), `Texture` (from pixels), `Material` — is produced through
> `Build(...)`, **async by default** (returns a `Task<Ref<T>>` / lands a pending
> handle), with `BuildSync(...)` the blocking sibling — the async-default rule of
> `Load`/`LoadSync` and `Upload`/`UploadSync`. The low-level `Create(const XInfo&)`
> constructors these assets *also* expose (`Mesh::Create(MeshInfo)`,
> `Material::Create(MaterialInfo)`) stay `Create`: they are the GPU-object
> construction step, distinct from building the asset from data.

So the verb tells you the tier *and* the sync/async expectation: `Create` → a GPU
object, now; `Build` → an asset, streaming (`BuildSync` to block).

## The renames

| Factory | Today | After |
|---|---|---|
| `Mesh` async build | `CreateAsync(ctx, tasks, data, name)` | `Build(ctx, tasks, data, name)` |
| `Mesh` sync build | `Create(ctx, data, name)` | `BuildSync(ctx, data, name)` |
| `Mesh` allocation | `Create(MeshInfo)` | **unchanged** |
| `Texture` async build | `CreateAsync(…)` (plan 06's `Task<Ref>` form) | `Build(…)` |
| `Texture` sync build | `Create(ctx, TextureInfo)` | `BuildSync(…)` |
| `Material` async build | `CreateAsync(…)` (plan 06) | `Build(…)` |
| `Material` allocation | `Create(MaterialInfo)` | **unchanged** |
| `AssetManager` async create | `CreateAsync<T>(Task<Ref<T>>)` | `Adopt(Task<Ref<T>>)` overload |
| Primitive mesh helper | `CreatePrimitiveMesh(mgr, shape)` (plan 02) | `BuildPrimitiveMesh(mgr, shape)` |

Every `Create(const XInfo&)` factory of a **low-level GPU resource**
(`Buffer`/`Image`/`ImageView`/`Sampler`/`Shader`/pipelines/…) is **untouched** —
they are the low-level tier and already correctly named.

The rule governs that tier only. `Texture` and `Material` are engine **assets**, so
their *descriptor-named* sync factory still moves to the asset side:
`Texture::Create(ctx, TextureInfo)` → `BuildSync` even though `TextureInfo` reads like
an `XInfo`, because it is the from-data build (`Texture` has no separate
allocate-from-handles form), not a GPU-object allocation. The genuine `Create(XInfo)`
that **stays** is the one constructing a GPU object from already-uploaded handles —
`Mesh::Create(MeshInfo)`, `Material::Create(MaterialInfo)`. So "every `Create(XInfo)`
is untouched" is precisely: every *low-level GPU-resource* `Create(XInfo)`; an asset's
descriptor-named *build* is the exception, and the table above is authoritative on
which is which.

Each `Build` / `BuildSync` / `Create` doc-comment cross-references its sibling with
`@see`, so both tiers are visible from either entry point now that the verb no longer
telegraphs that two factories share a class.

## `AssetManager::Adopt(Task<Ref<T>>)` — folding in `CreateAsync`

`Adopt` already wraps a runtime resource into the cache; today `Adopt(Ref<T>)` takes a
**resident** one. Add an overload `Adopt(Task<Ref<T>>)` that takes a **pending** one —
the detached entry is created immediately, the handle returned, and the task finalized
into `Resource` through the continuation pump (the exact `CreateAsync` lifecycle from
planset-26, now reached through the overload). The standalone `CreateAsync<T>` method
is removed; callers write `manager.Adopt(Mesh::Build(...))`. One verb covers "take this
runtime resource into the cache," resident or streaming, chosen by the argument type.

## Call-site sweep

~87 sites: `BuildPrimitiveMesh` and its body (`Mesh::CreateAsync` → `Mesh::Build`,
`manager.CreateAsync<Mesh>` → `manager.Adopt`) and its callers (the resolver, tests);
the sync `Mesh::Create(data)` users (the hello-triangle runtime sphere, tests, the
smoke path → `BuildSync`); plan 06's `Texture`/`Material` factories and tests; and
doc-comment cross-references. `Create(XInfo)` sites are not touched.

## Tests

- Mechanical reference updates only — no new logic; the renames are
  behavior-preserving.
- **Smoke golden unchanged.**

## Acceptance

- Clean build; `ctest` green; `smoke_golden` unchanged; `VE_DEBUG` gate clean;
  `include_hygiene` green.
- GPU resources construct through the unchanged `Create(const XInfo&)`; engine assets
  build through `Build`/`BuildSync`; `AssetManager::Adopt(Task<Ref<T>>)` replaces
  `CreateAsync<T>`; the primitive helper is `BuildPrimitiveMesh`.
- The CLAUDE.md resource section states the `Create`-resource / `Build`-asset split.
