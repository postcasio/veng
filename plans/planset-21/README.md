# planset-21 — frustum culling

**Phase goal:** cash in the second prime consumer of the [planset-20](../planset-20/README.md)
bounds facility — **view-frustum culling**. Add a `Frustum` math primitive (six planes, a
conservative AABB test) beside `AABB`, gather every resident `(Transform, MeshRenderer)`
entity into a per-frame visible-candidate list once, and have the two scene-drawing passes —
the **g-buffer geometry pass** (culled by the **camera** frustum) and the **cascaded shadow
pass** (culled by each **cascade's light** frustum) — skip the meshes their frustum does not
touch. Culling is a pure optimization: the rendered image is **byte-identical**; only the
draw calls issued change.

Both scene passes today walk `Each<Transform, MeshRenderer>`, recompute `WorldMatrix(entity)`
per draw (re-walking the parent chain), and issue every submesh's `DrawIndexed` regardless of
where the camera looks
([SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp) g-buffer pass,
[ShadowScenePass.cpp](../../engine/src/Renderer/ShadowScenePass.cpp) — the latter once **per
cascade**). Every mesh is drawn into the g-buffer and into every cascade tile, on-screen or
not. Frustum culling is the standard fix: build the view's six bounding planes and reject any
mesh whose world-space `AABB` lies wholly outside one of them before recording its draws.
[planset-20](../planset-20/README.md) named this exactly — "**frustum culling** (the other
prime consumer of mesh bounds) is the named next increment reading the delivered
`AABB`/`SceneBounds` facility" — and built the bound it needs (`Mesh::GetBounds()`, the
`AABB` algebra, the `ComputeWorldMatrices` amortized pass).

The work is **foundation-first**, mirroring planset-20: the `Frustum` primitive and the
`Intersects(Frustum, AABB)` test (Plan 01) and the per-frame gather (Plan 02) are pure,
device-free, and fully unit-tested before any pass consumes them, because the cull decision is
a pure function of a matrix and a box. The pass wiring (Plan 03) and the debug/settings surface
+ docs (Plan 04) build on that analytic core, and the **smoke golden never moves** — a moved
golden in this planset is a bug, since culling must not change the image.

## Scope decisions

Eight decisions fix the boundary of this work.

1. **`Frustum` is a glm-only math primitive in `Veng/Math/`, beside `AABB`.** Six bounding
   half-spaces (`std::array<vec4, 6>` — left/right/bottom/top/near/far, each a plane whose
   inward normal + offset make `dot(plane.xyz, p) + plane.w >= 0` the inside test), extracted
   by **Gribb-Hartmann** directly from a combined view-projection matrix. The extraction is
   adapted to **Vulkan ZO clip** (`z ∈ [0,1]`), **not** the OpenGL `[-1,1]` form — the near
   plane is the third clip row, not row4+row3 (the one extraction subtlety the unit test pins).
   It pulls in nothing but glm, so it stays inside `include_hygiene` and joins `AABB` in the
   `Veng/Math/` area.

2. **The AABB-vs-frustum test is conservative — never a false cull.** `Intersects(frustum,
   box)` returns false only when the box lies **wholly outside** a single plane (the standard
   p-vertex / positive-vertex test against each plane). A box straddling a plane, or just
   outside a frustum corner, returns true (a false *positive* that draws an extra object, never
   a false *negative* that drops a visible one). Correctness over tightness: a missing draw is a
   visible artifact; an extra draw is only wasted work. The cheaper corner-exact test is a named
   refinement, not built.

3. **Mesh-granularity, recompute-on-demand, linear scan — no spatial structure.** Culling
   tests one world-space `AABB` per resident `(Transform, MeshRenderer)` entity, in a linear
   pass, every frame — exactly the shape of `SceneBounds`'s reduction, and built on the same
   `ComputeWorldMatrices` amortized pass. No BVH, no octree, no cached/dirty-tracked visibility.
   A **BVH (or a cached, dirty-tracked scene bound)** is the scaling step this shares with the
   `SceneBounds` reduction — the answer when the linear scan is measured hot — and stays
   **deferred**, named not built (the seam is the gather in Plan 02: a BVH replaces the linear
   scan behind the same `GatherMeshes` surface).

4. **One per-frame gather, two frustums.** `SceneRenderer::Execute` builds the visible-
   candidate list **once** (one `ComputeWorldMatrices` pass producing each entity's world matrix
   + world-space bound + resident mesh), exactly as it already packs the scene's lights into a
   ring buffer each `Execute`. Each consuming pass then iterates that one shared list and applies
   `Intersects` against **its own** frustum inline — the **camera** frustum for the g-buffer
   pass, each **cascade's** light frustum for the shadow pass — so the gather is not redone per
   pass or per cascade. The gathered world matrix also **replaces the per-draw
   `WorldMatrix(entity)` re-walk** both passes do today (a side win the gather pays for once).

5. **Shadow casters cull against the cascade frustum, not the camera frustum.** An off-screen
   mesh behind or beside the camera can still cast a shadow into view, so the shadow pass must
   **not** cull by the camera frustum. It culls each cascade by **that cascade's** light-space
   view-projection frustum (`Frustum::FromViewProjection(view.CascadeViewProj[k])`), which
   planset-20 already fits to the camera slice **and extends toward the light** to catch
   off-screen casters — so culling against it is both correct (keeps the casters CSM needs) and
   conservative (drops only what cannot shadow that slice).

6. **Cull on/off is a `SceneRendererSettings` knob, read by the passes — recompiling incidentally.**
   It sits in the one tuning surface beside the `Bloom`/`Shadows`/`AO` toggles (`bool FrustumCull =
   true`), captured by the passes in `Configure`. Unlike those, the cull changes **no pass topology**
   (the pass stays compiled in and records *fewer* draws), so the `Configure` recompile it triggers is
   a no-op rebuild — **incidental, not semantic**. The honest cost: `Configure` also retires and
   recreates the output target and **invalidates `GetOutput()`**, so a toggle forces consumers to
   re-fetch — wasteful for a knob that rewires nothing. A per-frame runtime field is the alternative
   (correct, no recompile, no `GetOutput()` churn) and is the technically cleaner shape; the `Settings`
   home is chosen for call-site consistency with the other knobs, with that cost accepted because the
   toggle is a human debug control, not a per-frame programmatic sweep. **`Exposure` is *not* the
   precedent** — it is a per-frame `SetParam`, not a `Configure` recompile. This decision is stated
   here once; the plans reference it rather than re-deriving it.

7. **The smoke golden stays byte-identical; a draw-count test is the guard.** Culling is a
   pure optimization — every mesh the frustum touches renders exactly as before, so the
   `smoke_golden` capture **must not move** (a move is a bug, an over-aggressive cull dropping a
   visible mesh). A green golden proves *nothing* about whether culling happened, so the planset is
   pinned by a **draw-count** assertion over a purpose-built fixture: a scene **containing an
   off-frustum mesh** gathers it but does **not draw** it, and the same scene with culling off draws
   strictly more. **The fixture is the guard** — "strictly more" holds only because the fixture places
   a mesh outside the frustum; an all-visible scene draws the same count either way. An unchanged image
   alone cannot prove an optimization ran; the draw-count test is what does.

8. **No per-submesh, occlusion, or GPU culling.** This lands mesh-granularity CPU frustum
   culling. Per-submesh bounds + culling, occlusion culling (hi-Z / queries), GPU/compute-driven
   culling, and portal/PVS visibility are named follow-ons reading the same `AABB`/`Frustum`
   facility — out of scope. Frustum culling is also a **`SceneRenderer` battery only**: an app
   driving `RenderGraph` directly (the composite path the sample retains) is unculled by default and
   opts in via the same public `Frustum`/`GatherMeshes` facility if it wants it.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Frustum primitive + AABB intersection](01-frustum-primitive.md) | New `Veng/Math/Frustum.h` (glm-only): the six-plane `Frustum`, `FromViewProjection` (Gribb-Hartmann, **Vulkan ZO**), and a conservative `Intersects(Frustum, AABB)` p-vertex test. Device-free unit tests — extraction against a known perspective + ortho matrix, a box wholly inside / wholly outside / straddling each plane, a behind-camera box culled, the conservative no-false-cull property. Within `include_hygiene`. | done |
| 02 | [The per-frame visibility gather](02-visibility-gather.md) | New `Veng/Scene/Visibility.h` (+ src): `VisibleMesh { Entity Owner; mat4 World; AABB WorldBounds; const Mesh* Mesh; }` and `GatherMeshes(const Scene&, vector<VisibleMesh>&, AABB& outBounds)` — every resident `(Transform, MeshRenderer)` entity in one pass, world bound = `Mesh->GetBounds().Transformed(world)`, **subsuming `SceneBounds`** (`outBounds` is its union, a free by-product). No culling (the unculled candidate set; the consumer applies `Intersects` per frustum). Device-free scene test (MeshInfo-built meshes carrying bounds, the `bounds.cpp` pattern). | proposed |
| 03 | [Wire camera + per-cascade shadow culling](03-pass-culling.md) | `SceneRenderer::Execute` gathers once into renderer scratch (like the light pack) and points `SceneView` at a `std::span<const VisibleMesh> Visible`. The g-buffer pass loops the span, culls against the **camera** frustum, and draws `item.Mesh` at `item.World` (dropping the per-draw `WorldMatrix` walk); the shadow pass culls the span against each **cascade's** light frustum. `SceneRendererSettings::FrustumCull` (default on; recompiles incidentally — README decision 6). Adds the `GetLastDrawnCount()`/`GetLastVisibleCount()` getters the guard reads. Golden **byte-identical**; pinned by a draw-count `gpu` test (off-frustum drop, all-visible, off-screen shadow caster); validation gate under `VE_DEBUG`. | proposed |
| 04 | [Debug/settings exposure + docs/roadmap re-cut](04-debug-docs.md) | Expose the `FrustumCull` knob (a `Configure` checkbox) and a **visible/total** mesh-count stat in the example debug UI (`Veng::UI`). Document frustum culling in `CLAUDE.md`'s renderer paragraphs; mark it delivered in `future/scene-renderer.md` + `future/README.md` (BVH / cached scene bound named the next shared scaling step; shadowed punctual lights still the other named increment); add the `plans/README.md` entry; finalize this table. | proposed |

## Dependency analysis

```
01 (Frustum primitive + Intersects)
   └──► 02 (visibility gather) ──► 03 (wire camera + per-cascade cull) ──► 04 (debug + docs)
```

- **Plan 01** is the primitive: the `Frustum` type, the matrix extraction, and the
  conservative AABB test — a pure function, fully unit-tested with no GPU. Plans 02–03 consume
  it.
- **Plan 02** is the per-frame gather — the candidate list and its one `ComputeWorldMatrices`
  pass, device-free over MeshInfo-built meshes. It is the BVH seam (a spatial structure later
  replaces the linear scan behind it).
- **Plan 03** is the only GPU-behavior change: it wires the gather + both frustums into the two
  passes and the `FrustumCull` knob. The golden stays byte-identical; the draw-count test is the
  move's guard.
- **Plan 04** is the debug/settings surface + docs/roadmap, last.

The order is **01 → 02 → 03 → 04**, fully serial. Plans 01–02 are pure/device-free and land
their value in unit tests before any GPU work; the only render-path change is concentrated in
03, and it moves no golden.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify
(clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a
device is present, the smoke PPM correct size + exit 0, the `smoke_golden` capture
**re-checked and unmoved**) → update this table → one commit per plan (`Plan NN: <summary>`,
`Co-Authored-By` trailer).

Common to all plans:

- **`smoke_golden` must stay byte-identical across the whole planset.** Culling changes which
  draws are issued, never the resulting pixels — a moved golden is an over-aggressive cull
  (a bug), not an expected render change. There is **no** golden regeneration in this planset.
- **The cull guard is a draw-count test over a purpose-built fixture, not the golden** (Plan 03).
  The distinguishing assertion is that a mesh placed **outside** the camera frustum is gathered but
  not drawn, and that the *same* scene records strictly more draws with culling off. "Strictly more"
  is a property of the fixture (it contains an off-frustum mesh), not a universal truth — an
  all-visible scene draws the same count either way — so the fixture, not the property, is the guard,
  and it pins what a re-blessed image cannot.
- **Run the validation gate under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`):
  skipping a mesh's draws must not strand a barrier or leave an attachment in the wrong layout —
  the graph-derived transitions are unaffected by *which* meshes a pass records, and the gate
  pins that no MoltenVK validation error appears when a pass records a culled (smaller) draw set,
  including a pass that records **zero** draws (an empty frustum).
- **The `Frustum` math is glm-only and device-free.** `Frustum.h` and `Intersects` are
  public/pure, inside `include_hygiene`, and tested with no ICD — the value of 01–02 lands
  before any GPU work.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy applies; no
  "used to draw every mesh" or "before culling" narrative.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

The engine has a `Frustum` bounds primitive (`Veng/Math/`) beside `AABB`, and a per-frame
visibility gather reduces a `Scene` to its resident mesh instances (world matrix + world bound)
on demand. The deferred renderer **culls by frustum**: the g-buffer geometry pass records only
the meshes the **camera** frustum touches, and the cascaded shadow pass records only the casters
**each cascade's** light frustum touches — both off a single shared gather, recompute-on-demand,
with the rendered image unchanged. The facility is general: occlusion culling, per-submesh
culling, and a future **BVH** (the scaling step shared with the `SceneBounds` reduction) all read
the same `AABB`/`Frustum`/gather surface — each a named future increment behind this foundation.
</content>
</invoke>
