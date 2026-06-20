# Plan 03 — Wire camera + per-cascade shadow culling

**Goal:** make the deferred renderer cull by frustum. `SceneRenderer::Execute` gathers the
visible candidates once; the g-buffer geometry pass records only the meshes the **camera**
frustum touches, and the cascaded shadow pass records only the casters **each cascade's** light
frustum touches. The rendered image is unchanged — only the draws issued shrink. This is the
planset's one GPU-behavior change, and it moves no golden.

## What lands

### `SceneRenderer` gathers once and threads the list through `SceneView`

`SceneRenderer` gains a per-`Execute` scratch vector `vector<VisibleMesh> m_VisibleMeshes`, filled
at the top of `Execute` by `GatherMeshes(view.World, m_VisibleMeshes, sceneBounds)` — the same place
and shape as the existing per-`Execute` light pack (`View<Light>` → ring buffer). The gather's
`sceneBounds` by-product **replaces the separate `SceneBounds(view.World)` call** that currently
feeds `ComputeCascades` ([SceneRenderer.cpp:1590](../../engine/src/Renderer/SceneRenderer.cpp)), so
`Execute` runs one world-matrix pass per frame, not two. `SceneView` gains a borrowed span the
renderer points at the freshly gathered list:

```cpp
struct SceneView {
    // ... existing fields (World, Camera, Delta, LightCount, Bloom*, CascadeViewProj, ...)

    std::span<const VisibleMesh> Visible;   // set by the renderer each Execute (it gathers)
};
```

`Visible` is set by the renderer in `Execute`, the way it already sets `LightCount` — the app's
`SceneView` carries only its genuine inputs (camera, world, delta, the per-frame bloom knobs). The
gather is per-`Execute` and **read-only during record**, reaching the passes through the same
record-time channel as the rest of `SceneView`.

The **cull on/off knob is a `SceneRendererSettings` field** — `bool FrustumCull = true;` — read by
the passes from the `Configure`-captured settings they already hold (the way `Bloom`/`Shadows`/`AO`
are read), not a `SceneView` field. Toggling it recompiles via `Configure` even though the change is
topology-neutral and the recompile is incidental; the full rationale (and its honest `GetOutput()`
cost, and why `Exposure` is *not* the precedent) is README decision 6.

### The g-buffer pass culls against the camera frustum

`GBufferScenePass`'s `Execute` callback ([SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))
stops walking `Each<Transform, MeshRenderer>` and instead loops `view.Visible`, building the camera
frustum once outside the loop and skipping any culled item:

```cpp
const Frustum cameraFrustum = Frustum::FromViewProjection(view.Camera.ViewProjection());
m_LastDrawn = 0;  // mutable per-record counter; the renderer reads it back (see "Draw counts")

for (const VisibleMesh& item : view.Visible)
{
    // m_FrustumCull is the pass's Configure-captured setting (like m_Bloom etc.).
    if (m_FrustumCull && !Intersects(cameraFrustum, item.WorldBounds))
        continue;

    // material-readiness check (unchanged) — a not-yet-ready mesh is skipped here too,
    // so m_LastDrawn counts actually-recorded meshes, not merely in-frustum ones.
    // Then bind/draw item.Mesh at item.World — item.World replaces the per-draw
    // WorldMatrix(view.World, entity) re-walk.
    const mat4 mvp = view.Camera.ViewProjection() * item.World;
    // ... normal matrix from item.World, per-submesh DrawIndexed as today
    ++m_LastDrawn;
}
```

`item.World` replaces the in-loop `WorldMatrix(view.World, entity)` call (a side win of the gather:
the parent-chain walk happens once in the gather, not per draw). It is **bit-identical** to the
retired per-draw call — both route through the same `WorldMatrix` composition — which is *why* the
golden holds, not merely that "the same meshes draw"; `smoke_golden` is a fuzzy compare, so the
matrix identity is what rules out sub-threshold drift from the matrix-source swap. The
material-readiness check, the vertex/index binds, the per-submesh material bind + `DrawIndexed`, and
the push constants are **unchanged** — only the iteration source (the gathered span) and the cull
skip are new.

### The shadow pass culls each cascade against its light frustum

`ShadowScenePass` ([ShadowScenePass.cpp](../../engine/src/Renderer/ShadowScenePass.cpp)) does the
same, **per cascade**: inside the existing `for (k < count)` cascade loop, build that cascade's
frustum from its light matrix and cull the shared span against it:

```cpp
const Frustum cascadeFrustum = Frustum::FromViewProjection(view.CascadeViewProj[k]);

for (const VisibleMesh& item : view.Visible)
{
    if (m_FrustumCull && !Intersects(cascadeFrustum, item.WorldBounds))
        continue;
    // bind/draw item.Mesh with cascade k's light-space MVP (item.World), as today
}
```

The shadow pass culls by the **cascade** frustum, never the camera frustum — `view.CascadeViewProj[k]`
is planset-20's near-extended ortho fit, so culling against it keeps every caster that can shadow
the slice (including off-screen ones) and drops only what cannot. Each cascade culls
independently, so a near caster outside the far cascade's tight slice is still skipped there. With an
**empty scene** the gather is empty, so the cull loop body never runs and a degenerate cascade
frustum (extracted from an ortho fit to an empty `SceneBounds`) is never tested — `Intersects` is
only ever called when at least one candidate exists.

### The toggle and the draw counts

`SceneRendererSettings` gains `bool FrustumCull = true;` beside the existing `Bloom`/`Shadows`/`AO`
knobs — the one tuning surface. The geometry and shadow passes capture it in `Configure` (as a
`m_FrustumCull` member, like their other captured settings) and read it during record; off → both
passes record every mesh again. Flipping it goes through `Configure` (incidental recompile — README
decision 6 has the rationale and its honest cost). The debug-UI checkbox lands in Plan 04.

**Draw counts land here, not Plan 04.** The drawn count is produced *inside* `GBufferScenePass`'s
`Execute` callback — after the cull skip *and* the material-readiness skip — where `Execute` itself
cannot see it. So the pass holds a `mutable u32 m_LastDrawn` it resets and increments per record, and
the renderer reads it back through `SceneRenderer::GetLastDrawnCount()`; `GetLastVisibleCount()`
returns the gathered candidate total (`m_VisibleMeshes.size()`). Both getters are added **in this
plan**, because the draw-count test below reads `GetLastDrawnCount()`; Plan 04 only renders them in
the debug UI.

## Decisions

1. **Gather in `Execute`, like the light pack.** The visible list is per-`Execute` renderer
   scratch filled exactly where and how the light list already is, then handed to passes by
   reference through `SceneView`. No new lifetime mechanism — the renderer already augments
   `SceneView` per frame (`LightCount`).

2. **The frustum is built per pass (and per cascade), not gathered.** Extraction is a handful of
   row reads; building it once per pass / per cascade outside the draw loop is free, and keeps the
   gather frustum-agnostic (the multi-frustum requirement of decision 4 in the README).

3. **`item.World` retires the per-draw `WorldMatrix` walk.** Both passes recompute the parent
   chain per draw today; the gather computes it once via `ComputeWorldMatrices`. Reading
   `item.World` is the same matrix, computed once — a correctness-preserving simplification the
   cull wiring pays for anyway.

4. **The golden does not move.** Every mesh the camera frustum touches draws identically (same
   pipeline, same push constants, same submesh loop); the shadow it casts is unchanged because the
   cascade cull keeps every caster the cascade frustum contains. A moved `smoke_golden` means the
   cull dropped something visible — a bug to fix, not a golden to re-bless.

5. **Zero barrier impact, and an empty scene is safe.** Which meshes a pass records does not change
   the pass's declared reads/writes, so the graph-derived `RenderingInfo`/barrier schedule is
   identical. A pass that records **zero** draws (its frustum empty, or every mesh culled) is valid —
   it still clears/transitions its attachments per the baked schedule. An **empty scene** yields an
   empty gather, so the cull loop body never runs and a degenerate cascade frustum (an ortho fit to an
   empty `SceneBounds`) is never tested. The validation gate pins the zero-draw path (Verification
   forces an all-culled frame, which the smoke scene never produces).

6. **The cull knob lives in `SceneRendererSettings` (README decision 6).** It sits in the one tuning
   surface beside `Bloom`/`Shadows`/`AO`, captured by the passes in `Configure` — **not** a
   `SceneView`/runtime field and **not** a topology change. The full rationale (the incidental
   recompile, its honest `GetOutput()` cost, the rejected per-frame-field alternative, and why
   `Exposure` is *not* the precedent) is stated once in README decision 6 and not repeated here.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/SceneRenderer.h` | `SceneView` gains `std::span<const VisibleMesh> Visible`; `SceneRendererSettings` gains `bool FrustumCull = true`; `SceneRenderer` gains `GetLastVisibleCount()` / `GetLastDrawnCount()`. |
| `engine/src/Renderer/SceneRenderer.cpp` | `m_VisibleMeshes` scratch; `GatherMeshes(..., sceneBounds)` in `Execute` **replacing** the separate `SceneBounds` call; set `Visible`; g-buffer pass captures `FrustumCull` in `Configure`, loops the span + camera-frustum cull + `item.World`, increments `m_LastDrawn`; renderer reads the counts back through the getters. |
| `engine/src/Renderer/ShadowScenePass.cpp` | Per-cascade light-frustum cull over the shared span. |
| `tests/gpu/` (the relevant scene-renderer GPU test source) | The draw-count culling assertions, reading `GetLastDrawnCount()` / `GetLastVisibleCount()`. |

## Verification

- Clean build; full `ctest` green across the unit/death/cooker bands and the `gpu` band where a
  device is present.
- **`smoke_golden` is byte-identical** (`ctest -R smoke_golden`) — re-checked, **not** regenerated.
  A move is a cull bug.
- **The cull guard — a `gpu` draw-count test** reading `GetLastDrawnCount()` / `GetLastVisibleCount()`
  (the planset's distinguishing assertion, since the golden cannot prove culling ran):
  - **Off-frustum mesh dropped.** A scene with one mesh **inside** and one **far outside** the camera
    frustum records the inside mesh and **skips** the outside one; the **same scene re-Configured with
    `FrustumCull` off** records **both** — strictly more draws. (The fixture's off-frustum mesh is what
    makes "strictly more" hold — the fixture is the guard, per README decision 7.) A behind-camera mesh
    is likewise culled from the g-buffer pass.
  - **All-visible scene: drawn == total** with culling **on** — a scene whose meshes all sit inside the
    camera frustum draws every one, pinning the no-false-cull property on a scene shaped like the smoke
    path (the case the off-frustum fixture does not cover).
  - **Off-screen shadow caster survives.** A caster placed **outside the camera frustum on the
    light-ward side**, whose shadow falls **inside** the camera view, is **not** culled by any cascade
    frustum it should cast into — the case the byte-identical golden structurally cannot cover (a
    wrongly-culled caster is a missing shadow, not a moved smoke pixel). Asserted on the shadow pass's
    per-cascade drawn set, or device-free over `ComputeCascades` + `Frustum` + `Intersects`.
  - **Per-cascade independence.** A mesh outside a far cascade's slice but inside a near one is culled
    from the far cascade only.
- **Validation gate under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`): a pass
  recording a culled (smaller, possibly **empty** — a camera-faces-away / all-culled frame) draw set
  strands no barrier and leaves no attachment mis-transitioned — no MoltenVK validation error. The
  all-culled frame is forced explicitly, since the smoke scene never produces one.
- Smoke PPM correct size + exit 0 through the launcher.
</content>
