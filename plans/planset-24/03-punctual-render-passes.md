# Plan 03 — punctual shadow render passes

**Goal:** the GPU producer side — render each budgeted shadowed punctual light's depth (a spot's one
view, a point's six faces) into the punctual atlas (Plan 02), reusing the `ShadowScenePass` depth
machinery, with **each view/face culling its casters through `SceneBroadphase::Cull` against its own
frustum** and reusing the single per-`Execute` `Sync` (README decision 5). The directional cascade
pass is unchanged. Consumes Plan 02's atlas + records; the lighting sample is Plan 04.

## What lands

### A punctual shadow depth pass ([engine/src/Renderer/ShadowScenePass.{h,cpp}](../../engine/src/Renderer/ShadowScenePass.h) or a sibling `PunctualShadowScenePass`)

A depth-only render of every selected shadowed punctual light into the punctual atlas, modelled on
the cascade pass's existing per-tile-viewport loop
([ShadowScenePass.cpp:143](../../engine/src/Renderer/ShadowScenePass.cpp)). It reuses the **same**
depth-only pipeline, `shadow_depth.vert` (light-space-MVP push at offset 0), and per-submesh draw
loop — only the pushed matrix, the viewport (the record's atlas tile), and the per-view frustum
differ.

`Declare` contributes **one** depth-only `RenderGraph` pass that writes the whole punctual atlas:

- One depth attachment (the punctual atlas), cleared to depth = 1 at `BeginRendering`. Tiles beyond
  the live record/face count keep the clear and are never sampled (the lighting pass gates on the
  record's type and slot).
- For each shadowed record `r` in `[0, view.PunctualShadowCount)`:
  - **Spot** (`Params.x == 2`): one tile. Set the viewport + scissor to the record's tile sub-rect,
    push `ViewProj[0]`, cull against `Frustum::FromViewProjection(ViewProj[0])`, draw the culled set.
  - **Point** (`Params.x == 1`): six tiles. For each face `f`, set the viewport to face `f`'s tile,
    push `ViewProj[f]`, cull against `Frustum::FromViewProjection(ViewProj[f])`, draw the culled set.

So a spot is one viewport draw and a point is six — the per-face redraw the small budget bounds.

### Each view culls its casters through the broadphase ([SceneBroadphase::Cull](../../engine/include/Veng/Scene/SceneBroadphase.h))

The cull is the **exact** mechanism the cascade pass uses
([ShadowScenePass.cpp:205](../../engine/src/Renderer/ShadowScenePass.cpp)): a pass-owned reused
`vector<u32> m_CullScratch`, `view.Broadphase->Cull(frustum, m_CullScratch)` per view/face, then the
per-submesh draw over `view.Visible[idx]` in the returned ascending (`GatherMeshes`) order. With
`FrustumCull` off, the full `view.Visible` span records into every tile.

- **The single per-`Execute` `Sync` is reused, not re-run.** `SceneRenderer::Execute` already calls
  `m_Broadphase.Sync(view.World)` once before the passes
  ([SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp)); the punctual pass queries that
  **same synced tree** — it does not re-sync. So a frame's `N` spot frustums + `6N` cube-face
  frustums are all queries against **one tree** built once, the "one tree, many queries" workload
  planset-23's BVH was built to serve (README decision 5). The g-buffer pass, the cascade pass, and
  now the punctual pass all share the one `Sync`.
- **The light's frustum, not the camera's.** Culling against `ViewProj[f]` keeps an off-screen caster
  that shadows into the light's volume (it is in the light frustum even if behind the camera) and
  drops casters outside the light's range/cone (outside the light frustum) — conservative, never a
  dropped visible shadow.

### The atlas reaches the lighting pass off bindless ([the set-1 bound-view seam](../../engine/include/Veng/Renderer/ScenePass.h), `PassIO`)

The punctual atlas is an `Import`ed, graph-declared resource — the lighting pass declares
`.Sample(punctualAtlas)` so the graph derives its `DepthAttachment → ShaderReadOnly` transition, and
its **binding** reaches the lighting pass through the existing `PassIO` **bound-view** slot (set 1,
binding 4, Plan 02), the same off-bindless delivery the cascade atlas uses. Binding and barrier stay
separate concerns — only the binding sits off bindless; the `.Sample` drives the barrier.

### A `DebugView` arm for a selected punctual map ([engine/include/Veng/Renderer/SceneRenderer.h](../../engine/include/Veng/Renderer/SceneRenderer.h) `DebugView`)

Beside `Shadows`/`Cascades`, a `DebugView::PunctualShadows` arm blits a selected punctual atlas tile
(raw depth) to the output, terminating the chain like the existing shadow-debug arms. It samples the
punctual atlas with an **ordinary (non-comparison) sampler** (raw depth, not a compare result) — the
same bound-view-seam blit pattern the `Shadows` arm uses for the cascade atlas. (The lit sampling of
these maps is Plan 04; this arm visualizes the depth the pass just wrote, so it pins that the pass
produces a non-empty map at this plan's close, before the lighting integration lands.)

## Decisions

1. **One punctual pass, many viewports — mirroring the cascade atlas.** All shadowed punctual views
   write one depth attachment in one graph pass, each via its own viewport; the graph derives one
   write → one sample barrier. This is the cascade pass's decision (planset-20 decision 3) applied to
   the punctual atlas; a per-light pass or multiview would fight veng's one-`RenderingInfo`-per-pass
   model for no gain (MoltenVK implements multiview by instance multiplication regardless, so six
   faces are six draws either way — README decision 2).

2. **The per-`Execute` `Sync` is shared; the punctual cull adds queries, not a tree.** The renderer
   syncs the broadphase once per frame; every shadow view is one more `Cull` against it. This is the
   workload planset-23 named as its prime new consumer — `N` (or `6N`) frustum queries against one
   tree per frame — and the reason the budget stays small.

3. **Cull against the light's own frustum.** An off-screen caster shadowing into the light's volume
   is kept (it is in the light frustum); a caster outside the light's range/cone is dropped. The cull
   is conservative (the broadphase returns the tight `Intersects` scan's exact set, planset-23), so
   the rendered shadow is identical to a no-cull render — only the draws issued differ.

4. **The debug arm visualizes raw depth with an ordinary sampler.** The `PunctualShadows` arm reads
   the atlas as a plain sampled image (raw depth), not through the immutable comparison sampler (which
   returns compare results) — the same split the `Shadows` arm draws for the cascade atlas. It lands
   here, with the pass that produces the map, so the producer is pinned before the consumer (Plan 04).

5. **The directional cascade pass is untouched.** The punctual pass is additive — a second depth pass
   writing a second atlas. The cascade pass keeps its atlas, its loop, and its `Sample` barrier
   exactly as planset-20 built them.

## Files

| File | Change |
|---|---|
| `engine/src/Renderer/ShadowScenePass.{h,cpp}` (or a new `PunctualShadowScenePass.{h,cpp}`) | The punctual depth pass: per-record/per-face viewport + raw-matrix push + per-view `SceneBroadphase::Cull` (pass-owned `m_CullScratch`) + per-submesh depth draw, reading `SceneView::PunctualShadows`; owns/exposes the punctual atlas view for the bound-view handoff. |
| `engine/CMakeLists.txt` | Add the new `.cpp` if a sibling pass. |
| `engine/include/Veng/Renderer/SceneRenderer.h` | `DebugView::PunctualShadows`. |
| `engine/src/Renderer/SceneRenderer.cpp` | Insert the punctual pass into the internal graph (gated by the shadow budget / Plan 05 toggle); declare the lighting pass's `.Sample(punctualAtlas)`; wire the atlas into the set-1 bound-view slot; the `PunctualShadows` debug-arm blit. |
| `tests/gpu/scene_renderer.cpp` (extend) + the `gpu` suite list | Punctual-shadow presence + per-view cull-equivalence assertions. |

## Verification

- Clean build; `ctest` green across the bands.
- **`gpu` band** (`SKIP_RETURN_CODE 77`, skips with no ICD):
  - **Presence:** a scene with a shadowed spot (and a shadowed point) over a receiver renders a
    non-zero, non-uniform depth into the punctual atlas — the `DebugView::PunctualShadows` blit shows
    a depth gradient, not a flat clear (the producer works before the consumer in Plan 04).
  - **Per-view cull equivalence:** with `FrustumCull` on, each shadow view's drawn count equals a
    linear `Intersects` scan of `view.Visible` against that view's `ViewProj[f]` frustum (the tree
    culls identically through the renderer); the rendered atlas is identical with culling on and off
    (the cull drops only non-casters — conservative, byte-identical depth).
  - **One `Sync`, many queries:** rendering a frame with a point light issues six face queries against
    the **same** broadphase tree without re-syncing — `DidBroadphaseRebuildLastFrame()` is `false` on
    a static frame even with the punctual pass active (the punctual queries do not touch the scene or
    re-sync the tree).
- **`smoke_golden` is byte-identical** — the sample scene gains no shadowed punctual light until Plan
  05, and the lighting pass does not yet sample the punctual atlas (Plan 04), so the lit smoke image
  does not move. The punctual pass renders into its own atlas, consumed by no visible pixel yet. No
  golden regeneration.
- **Validation gate clean under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`): the
  punctual atlas's `RenderingInfo` (one depth attachment), the per-record/per-face viewport draws, the
  `DepthAttachment → ShaderReadOnly` transition derived from the lighting pass's `.Sample`, and the
  bound-view binding carry **no MoltenVK validation error** — pin the gate after this plan, since it
  adds a new render pass and a new sampled depth target (README common-to-all).
- Smoke PPM correct size (≈ 2,764,816 bytes) + exit 0 through the launcher.
