# Plan 10 — Docs + roadmap re-cut

**Goal:** make the docs and roadmap reflect the asset system as built. Document the
new project layout, the cook→pack→load workflow, the Slang toolchain requirement,
and the `AssetHandle`-vs-`Ref` ownership distinction; then re-cut the future
roadmap so area 1's synchronous slice reads as delivered and the remaining chain
(threading → async + bindless) is clear. Roadmap-only commit (`planset-5:`).

## Why this is its own plan

Every prior plan touched code with a green build; this one is purely documentation
and roadmap bookkeeping, kept separate so it lands once at the end against the
finished surface rather than chasing a moving API.

## Work

1. **`CLAUDE.md`** — the load-bearing project doc:
   - **Layout** section: the per-lib subdirectory tree (`engine/`, `assetformat/`,
     `cooker/`, plus examples/tests) — replaces the old `include/`+`src/` layout.
   - **Build & test**: the `vengc` tool + `VENG_BUILD_TOOLS`; the cooker's
     heavy/toolchain deps (assimp, Slang, SPIRV-Reflect, json) are cooker-only and
     never reach `libveng` or its consumers.
   - **A new "Assets" core-conventions subsection**: cook is offline (no
     cook-on-demand); load is by opaque `u64` `AssetId` through mounted
     `.vengpack` archives; `AssetManager` is owned by `Application` and threaded a
     `Context&`; `LoadSync` blocks (async is the next planset); apps release
     handles in `OnDispose`.
   - **Shaders**: materials author in **Slang**, compiled + reflected **offline**
     by the cooker into a `ShaderInterface`; the engine loads SPIR-V only.
2. **`docs/ownership.md`** — add `AssetHandle<T>` / `WeakAssetHandle<T>`: a
   high-level reference *to* an asset (indirection into the `AssetManager` cache),
   distinct from `Ref<T>` *inside* an asset (the GPU resources, unchanged rule).
   Eviction is deferred through the existing per-frame retire queue — no new
   reclamation mechanism.
3. **`plans/future/README.md`** — mark **area 1 (asset system)** taken up by
   planset-5 for its **synchronous slice** (struck like area 3 was for planset-4):
   the cooker, asset packs/archives, `AssetManager`/`LoadSync`, texture/mesh/
   shader(Slang)/material types, and offline shader reflection are delivered.
   Leave as the remaining chain: **area 2 (threading → async loads)** and the
   **bindless** rework (the material's eventual backing). Re-cut the ordering
   diagram to `5 (sync assets ✅) → 2 threading (async) + bindless`.
4. **`plans/future/asset-system.md`** — trim to the enduring **end-state vision**
   it still uniquely holds (async `Load` default, the bindless "thin material",
   hot-reload, the dependency-graph-driven eviction), with a banner that the
   synchronous foundation shipped in planset-5 and the resolved decisions
   (no cook-on-demand, opaque-u64 ids, separate cooker lib, Slang) are settled
   there. Strike the now-answered open-decisions.
5. **`plans/README.md`** — add the planset-5 index entry (synchronous asset system:
   cooker, packs, loader, types; Slang reflection; sync-only by decision).
6. **`plans/planset-5/README.md`** — flip the status column to `done`.

## Dependencies

All prior plans (01–09) landed and verified.

## Acceptance

- `CLAUDE.md` describes the actual built layout, the `vengc` workflow, and the
  Slang/asset conventions; a newcomer could cook + load a pack from it alone.
- `docs/ownership.md` distinguishes `AssetHandle` from `Ref`.
- The future roadmap shows area 1's sync slice done and area 2 + bindless as the
  remaining chain; `plans/README.md` lists planset-5.
- No code changes — docs/roadmap only.

## Notes

- Keep `bindless-descriptors.md` and `threading-task-system.md` intact — they are
  the next phases' design docs and this planset deliberately stops short of both.
- Mention the one deliberate departure from the original sketch (no
  `CookOnDemand`) so future readers don't reintroduce it.
