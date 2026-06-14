# Plan 11 — Docs + roadmap re-cut

**Goal:** make the docs and roadmap reflect the asset system as built. Document the
new project layout, the cook→pack→load workflow, the Slang toolchain requirement,
the bindless set-0 model, and the `AssetHandle`-vs-`Ref` ownership distinction; then
re-cut the future roadmap so area 1's synchronous slice **and the bindless rework**
read as delivered, leaving threading (async loads) as the remaining chain.
Roadmap-only commit (`planset-5:`).

## Why this is its own plan

Every prior plan touched code with a green build; this one is purely documentation
and roadmap bookkeeping, kept separate so it lands once at the end against the
finished surface rather than chasing a moving API.

## Work

1. **`CLAUDE.md`** — the load-bearing project doc:
   - **Layout** section: the per-lib subdirectory tree (`engine/`, `assetformat/`,
     `cooker/`, plus examples/tests) — replaces the old `include/`+`src/` layout.
   - **Build & test**: the `vengc` tool + `VENG_BUILD_TOOLS`; the cooker's
     heavy/toolchain deps (assimp, Slang, json) are cooker-only and never reach
     `libveng` or its consumers.
   - **A new "Assets" core-conventions subsection**: cook is offline (no
     cook-on-demand); load is by opaque `u64` `AssetId` through mounted
     `.vengpack` archives; `AssetManager` is owned by `Application` and threaded a
     `Context&`; `LoadSync` blocks (async is the next planset); apps release
     handles in `OnDispose`. Authoring shape: a pack is a pure
     `{ id, type, source }` manifest and **every** asset type — texture, mesh,
     shader, material — has its own per-asset JSON source file
     (`*.tex.json` / `*.mesh.json` / `*.shader.json` / `*.vmat.json`) that the
     entry points at; the manifest holds no per-asset settings.
   - **A "Bindless" note**: set 0 is the engine-provided `BindlessRegistry`
     (textures/samplers/storage arrays), bound once per frame; `PipelineLayout`
     reserves it; author bindings live in sets ≥ 1.
   - **Shaders & materials**: shaders are a first-class asset authored in **Slang**
     (a `*.shader.json` naming the `.slang` source, entry point, and optional
     vertex-layout id); the cooker **always** compiles them — there is no
     precompiled-inline path — and reflects them **offline** into a
     `ShaderInterface`; the engine loads SPIR-V only. A material (`*.vmat.json`)
     references its vertex/fragment shaders by `AssetId` and declares an **ordered,
     explicitly-typed** field list; the cook validates those fields against the
     fragment shader's reflected `MaterialData`.
   - **Update the validation-gap note**: plan 05 (via planset-2/06) closes the
     descriptor-pool / `UPDATE_AFTER_BIND` gap; remove/replace the corresponding
     allowlist entry and the "known validation gap" paragraph.
2. **`docs/ownership.md`** — add `AssetHandle<T>` / `WeakAssetHandle<T>`: a
   high-level reference *to* an asset (indirection into the `AssetManager` cache),
   distinct from `Ref<T>` *inside* an asset (the GPU resources, unchanged rule).
   Note bindless handles (`TextureHandle`/`SamplerHandle`) as `u32` slot ids, not
   owners — the `BindlessRegistry` keeps the owning `Ref`. Eviction + slot release
   are deferred through the existing per-frame retire queue — no new reclamation
   mechanism.
3. **`plans/future/README.md`** — mark **area 1 (asset system)** taken up by
   planset-5 for its **synchronous slice** (struck like area 3 was for planset-4):
   the cooker, asset packs/archives, `AssetManager`/`LoadSync`, texture/mesh/
   shader(Slang)/material types, offline shader reflection, **and the bindless
   subsystem** are delivered. Leave **area 2 (threading → async loads)** as the one
   remaining chain item; re-cut the ordering diagram to
   `5 (sync assets + bindless ✅) → 2 threading (async)`.
4. **`plans/future/bindless-descriptors.md`** — banner that the core subsystem
   shipped in planset-5 (plan 05); trim to what genuinely remains (BDA-vs-arrayed
   buffers if not chosen, array growth, descriptor-buffer avoidance rationale,
   G-buffer-in-lighting access) and strike the resolved open decisions.
5. **`plans/future/asset-system.md`** — trim to the enduring **end-state vision**
   it still uniquely holds (async `Load` default, hot-reload, the
   dependency-graph-driven eviction), with a banner that the synchronous foundation
   + bindless material shipped in planset-5 and the resolved decisions
   (no cook-on-demand, opaque-u64 ids, separate cooker lib, Slang always cooked
   from source — no precompiled-inline path, a per-asset JSON source file for every
   type, explicitly-typed material fields, bindless material) are settled there.
   Strike the now-answered open-decisions.
6. **`plans/README.md`** — update the planset-5 index entry to reflect bindless is
   included; flip its descriptor and future-area cross-refs.
7. **`plans/planset-5/README.md`** — flip the status column to `done`.

## Dependencies

All prior plans (01–10) landed and verified.

## Acceptance

- `CLAUDE.md` describes the actual built layout, the `vengc` workflow, the bindless
  set-0 model, and the Slang/asset conventions; a newcomer could cook + load a pack
  from it alone. The stale validation-gap note is gone.
- `docs/ownership.md` distinguishes `AssetHandle` and bindless handles from `Ref`.
- The future roadmap shows area 1's sync slice + bindless done and area 2 as the
  remaining chain; `plans/README.md` lists planset-5 accurately.
- No code changes — docs/roadmap only.

## Notes

- Keep `threading-task-system.md` intact — it is the next phase's design doc and
  this planset deliberately stops short of async.
- Mention the one deliberate departure from the original sketch (no
  `CookOnDemand`) so future readers don't reintroduce it.
