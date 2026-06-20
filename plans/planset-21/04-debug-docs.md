# Plan 04 — Debug/settings exposure + docs/roadmap re-cut

**Goal:** surface the `FrustumCull` knob (a `Configure` checkbox) and a drawn/total mesh-count stat in the example
debug UI, and re-cut the docs/roadmap to record frustum culling as delivered. The last plan —
no render-path change (the count getters it reads are added in Plan 03).

## What lands

### Debug-UI exposure (example)

The hello-triangle debug panel gains, through `Veng::UI` (no raw `ImGui::`):

- A **Frustum culling** checkbox that flips `SceneRendererSettings::FrustumCull` and calls
  `Configure` with the updated settings — exactly the path the existing `Bloom`/`Shadows`/`AO`
  checkboxes take — so the user can A/B culled vs. unculled live (the recompile is imperceptible).
- A **drawn / total** mesh-count readout — `GetLastDrawnCount()` (the meshes the g-buffer pass
  recorded this frame, post-cull and post-material-readiness) over `GetLastVisibleCount()` (the
  gathered candidate total), shown as `UI::Text(fmt::format("Meshes: {} / {}", drawn, total))`. **The
  getters are added in Plan 03**, where the drawn count is produced; this panel only reads them.

On the minimal sample scene (a sphere + a large plane) the drawn count rarely drops below the total —
the plane's world bound spans most views — so a steady "n / n" there is expected, not a bug. The
readout earns its keep on denser scenes and as the human-facing companion to the Plan 03 draw-count
test.

### Docs

- **`CLAUDE.md`** — the `SceneRenderer` / scene paragraphs gain a present-tense sentence that the
  g-buffer pass culls by the camera frustum and the shadow pass by each cascade's light frustum,
  over a single per-frame gather (`GatherMeshes`) reading the `AABB`/`Frustum` facility,
  recompute-on-demand. The `Veng/Math/` note gains `Frustum` beside `AABB`. No "used to draw every
  mesh" narrative (the comment/doc policy).
- **`plans/future/scene-renderer.md`** — move **frustum culling** from "still future" to
  delivered (a planset-21 section beside the planset-20 CSM one), and re-point the named next
  increments: a **BVH / cached, dirty-tracked scene bound** is now the immediate shared scaling
  step (the linear gather is its seam), with **shadowed punctual lights** still the other named
  increment, and **occlusion / per-submesh / GPU culling** the named refinements behind the
  delivered frustum cull.
- **`plans/future/README.md`** — mark frustum culling delivered in the area-8 (scene renderer)
  status; the BVH stays the explicit deferred scaling step.
- **`plans/README.md`** — add the planset-21 entry (frustum culling: the `Frustum` primitive, the
  per-frame gather, camera + per-cascade culling, golden-unchanged), and note it cashes in
  planset-20's named "other prime consumer of mesh bounds."

### Status

Finalize this planset's README table to `done` and confirm the legend.

## Decisions

1. **The checkbox drives the `SceneRendererSettings::FrustumCull` knob via `Configure`** — the
   same path as the `Bloom`/`Shadows`/`AO` checkboxes, the single tuning surface those already use.
   The recompile it triggers is incidental (the cull changes no topology) and imperceptible for a
   human toggle; surface consistency is why it lives in `Settings` rather than a separate runtime
   setter (Plan 03 decision 6).

2. **The drawn/total stat is a debug readout, not a render feature.** It is the human-facing companion
   to the Plan 03 draw-count test. The renderer's last-frame count getters are added in **Plan 03**
   (where the drawn count is produced inside the g-buffer pass); this panel only formats them with
   `fmt` per the `Veng::UI` text idiom. `drawn` is post-cull/post-material-readiness; `total` is the
   gathered candidate count (geometry-resident, so it can exceed `drawn` during async material load).

3. **Docs state the delivered shape, name the next step.** Per the doc policy, the prose is
   present-tense fact (what culls against what, over which gather) plus the standard "named next
   increment" framing (BVH the scaling step, punctual-light shadows / occlusion culling the
   siblings) — no roadmap-history narrative in `CLAUDE.md`.

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/main.cpp` | `Veng::UI` cull checkbox (flips `FrustumCull`, calls `Configure`) + drawn/total `UI::Text` readout (reads the Plan 03 getters). |
| `CLAUDE.md` | Frustum-culling sentence in the renderer/scene paragraphs; `Frustum` in the `Veng/Math/` note. |
| `plans/future/scene-renderer.md` | Frustum culling → delivered; BVH the next shared scaling step. |
| `plans/future/README.md` | Area-8 status: frustum culling delivered. |
| `plans/README.md` | The planset-21 entry. |
| `plans/planset-21/README.md` | Status column → `done`. |

## Verification

- Clean build; full `ctest` green across the unit/death/cooker bands and the `gpu` band where
  present.
- **`smoke_golden` byte-identical** — the debug-UI panel and the count getters add no scene-render
  change; the example's smoke pose is unaffected.
- The windowed example shows the **Frustum culling** checkbox and a live **drawn / total** readout;
  toggling the checkbox changes the drawn count when the scene has off-frustum meshes (on the minimal
  sample the two may stay equal — the Plan 03 test, not the sample readout, is the cull guard).
- Smoke PPM correct size + exit 0 through the launcher.
- Docs read clean: `CLAUDE.md`, `future/scene-renderer.md`, `future/README.md`, and
  `plans/README.md` agree that frustum culling is delivered and the BVH is the next shared scaling
  step; no roadmap/history narrative leaked into `CLAUDE.md`.
</content>
