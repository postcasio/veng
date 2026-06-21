# Plan 06 — Runtime async build factories for `Texture` and `Material`

**Goal:** give `Texture` and `Material` the same runtime async build path `Mesh`
has — a `Task<Ref<T>>`-returning factory that feeds `AssetManager::CreateAsync<T>`,
so a **runtime-generated** (id-less, not cooked) texture or material streams in
off-thread exactly like a primitive mesh. Independent of the spawn-resolve seam;
depends only on planset-26's `Mesh::CreateAsync`/`AssetManager::CreateAsync` shape and
planset-6's async `Upload`.

## Why this is its own plan

planset-26 built the async build path for **`Mesh` only** (the primitive needed it);
`Texture` and `Material` were left out of scope ("same mechanism, not needed here").
This adds the missing siblings so the async-resource story is uniform across the three
data-bearing assets — a self-contained addition with no dependency on the resolve
work, parallelizable with it.

## The shape to match

`Mesh::CreateAsync(Context&, TaskSystem&, MeshData, string) → Task<Ref<Mesh>>` runs
the build + host-visible `Upload` on a worker and assembles the `Ref<Mesh>` in a
main-thread continuation; `AssetManager::CreateAsync<Mesh>(task)` returns the pending
`AssetHandle<Mesh>`. Texture and Material gain the **same** `Task<Ref<T>>`-returning
form.

## `Texture`

`Texture` already has a *loader-shaped* `CreateAsync(Context&, const TextureInfo&,
TaskSystem&, Task<void>& outUpload)` — it returns an unregistered `Ref` plus an upload
task the caller must wait on before `Finalize()`. That two-phase form serves the
`TextureLoader`'s pipeline; it is **not** the `Task<Ref<Texture>>` shape
`AssetManager::CreateAsync` consumes.

Add the high-level sibling — a `Task<Ref<Texture>>`-returning build factory that folds
the decode/upload + finalize into one task (reusing the existing two-phase internals),
so `manager.CreateAsync<Texture>(Texture::CreateAsync(ctx, tasks, info))` works the way
the `Mesh` call does. The loader's two-phase path is unchanged.

## `Material`

`Material` has only the synchronous `Create(const MaterialInfo&)` (the param-block
build is CPU-side + a host-visible write — no transfer-queue copy). Add the async
build factory in the `Task<Ref<Material>>` shape so a runtime-assembled material
streams its param-block write through the continuation pump rather than blocking,
matching the others. (Material's build is light; the async form is for *uniformity*
of the resource API, not because the work is heavy.)

Because `Material::Build` async-wraps a *synchronous* host-visible write, it adds
nothing but symmetry — and a frame of continuation latency — over the sync `Create`.
It is accepted here as a no-op-shaped factory for API uniformity, not a performance
win. If, at review, the uniformity is not worth a consumer-less factory, the fallback
is to drop the `Material` async form and keep only `Texture`'s (which defers a real
upload). Both ship with no in-tree consumer regardless (see Scope honesty).

## Scope honesty

There is **no in-tree consumer** of these yet — the primitive path generates only
meshes, and cooked textures/materials load via the already-async `Load`. The factories
are provided for API uniformity and tested standalone, so a future runtime generator
(a procedural texture, a code-assembled material) is a non-blocking one-liner. This is
the deliberate "build the sibling now that the pattern is proven" call.

## Naming note

This plan authors the new factories under the **current** `CreateAsync` convention
(matching `Mesh::CreateAsync`). Plan 07 then draws the `Create`-resource / `Build`-asset
split in one sweep — `Mesh`/`Texture`/`Material`::`CreateAsync` → `Build` (and the
sync builds → `BuildSync`), and `AssetManager::CreateAsync<T>` folds into an
`Adopt(Task<Ref<T>>)` overload — so these new factories are renamed there alongside
the existing ones, never authored twice. (Authoring them here as `CreateAsync` keeps
this plan consistent with the tree state when it runs.)

## Tests — `tests/gpu`

- Build a runtime `Texture` via `manager.CreateAsync<Texture>(Texture::CreateAsync(…))`
  from CPU pixels, pump to residency, assert `IsLoaded()` and the expected extent/format.
- Build a runtime `Material` the same way, assert it becomes resident and its param
  block is the authored values.
- Both ride planset-6's host-visible `Upload`, already validation-clean — no allowlist
  change.

## Acceptance

- Clean build; `ctest` green (the new GPU cases; skip with no device).
- `Texture` and `Material` each have a `Task<Ref<T>>`-returning async build factory
  feeding `AssetManager::CreateAsync<T>`, mirroring `Mesh`.
- `VE_DEBUG` validation gate clean.
