# planset-25 — occlusion + GPU/compute-driven culling

**Phase goal:** take [planset-21](../planset-21/README.md)'s mesh-granularity **CPU**
frustum cull all the way to **GPU-driven**: per-submesh bounds, **hi-Z occlusion**
culling, and a **compute cull → indirect draw** path that moves the visibility test
onto the GPU and issues the surviving draws through `DrawIndexedIndirectCount`. These
are the named refinements behind the delivered frustum cull
([future/scene-renderer.md](../future/scene-renderer.md): "occlusion culling (hi-Z /
queries), per-submesh bounds + culling, and GPU/compute-driven culling … each reading
the same `AABB`/`Frustum`/gather surface"). It is the **deepest** culling increment —
it reworks how draws are submitted, not just which the CPU records — and the most
GPU-infrastructure-heavy of the three culling areas.

The work is staged so each layer is independently valuable and verifiable: per-submesh
CPU culling (Plan 01) is a pure refinement of the existing path; the hi-Z pyramid
(Plan 02) and occlusion test (Plan 03) add a real GPU occlusion stage; the
indirect-draw infrastructure (Plan 04) and the compute cull (Plan 05) move the whole
test GPU-side. The rendered image stays **byte-identical** under frustum-only culling;
occlusion culling drops only genuinely-occluded draws (conservative, never a visible
artifact), pinned by draw-count fixtures rather than the golden.

> **Status:** scoped (this README). Per-plan files are written when the planset is
> taken up — the plan table below fixes the shape and order; the detailed plan files
> land at execution time.

## Dependencies & ordering

This planset **builds on [planset-23](../planset-23/README.md)** (the gather/cache is
the candidate source the GPU path uploads) and is **complementary to the named BVH
broadphase** (a tree narrows the candidate set the GPU then occlusion-tests; GPU
culling narrows what survives frustum/occlusion). It does **not** depend on
[planset-24](../planset-24/README.md), though shadowed-light caster culls are an
obvious later consumer of the same GPU path. **Recommended order: after 23**; 24 and
25 are independent.

## Scope decisions

Nine decisions fix the boundary of this work.

1. **Per-submesh bounds derive at load — no cooked-format change.** Each `SubMesh`
   gains a local-space `AABB` folded from its index range over the canonical vertex
   positions at load, exactly as `Mesh::GetBounds()` folds the whole-mesh bound today
   (no on-disk format change). The gather/cull can then reject an off-frustum *submesh*
   of an on-frustum mesh — tighter than the mesh-granularity cull, and the granularity
   the GPU path operates at.

2. **Per-submesh CPU culling first, as a pure refinement.** Plan 01 tests each
   submesh's world bound against the frustum in the existing CPU passes — the same
   `Intersects` loop, one level finer. It is byte-identical (a submesh the frustum
   touches still draws) and guarded by a draw-count fixture with a partially-off-frustum
   mesh. This lands before any GPU machinery, mirroring the foundation-first cadence.

3. **Hi-Z is a depth pyramid built by compute; occlusion is conservative.** Plan 02
   builds a min/max depth mip chain (a hi-Z pyramid) from the depth target via a
   compute reduction — a reusable renderer-owned resource. Plan 03 tests each
   candidate's screen-space AABB against the pyramid: an object fully behind the
   nearest stored depth over its screen footprint is occluded and dropped. The test is
   **conservative** (it culls only what is provably hidden; a straddling or uncertain
   case draws), so occlusion culling **never drops a visible draw** — the same
   correctness bar as frustum culling.

4. **Temporal hi-Z (previous-frame depth) is the default; two-pass is the named
   alternative.** Testing against *last frame's* depth pyramid avoids a mid-frame
   dependency and a second geometry pass, at the cost of one frame of latency on newly
   disoccluded objects (a brief pop, the standard trade). The two-pass form (render a
   depth prepass this frame, build hi-Z, then cull the main pass) is named as the
   higher-quality alternative, not built — the choice is stated here so the plans don't
   re-derive it.

5. **GPU-driven culling uploads the candidate set and emits indirect draws.** Plan 05
   uploads the gather's candidate instances (world matrix, bounds, submesh draw args)
   to a GPU buffer; a **compute** shader runs the frustum + hi-Z tests per candidate
   and **compacts** the survivors' draw commands into an indirect buffer + a count; the
   geometry pass issues `DrawIndexedIndirectCount`. The CPU gather becomes the *upload
   source*; the visibility decision moves GPU-side.

6. **Indirect-draw infrastructure is net-new engine capability.** Plan 04 adds the
   `Context`/`RenderGraph`/`CommandBuffer` surface for GPU-resident draw-argument
   buffers and `DrawIndexedIndirectCount` (count sourced from a GPU buffer), behind the
   engine's vocabulary types — the reusable seam the compute cull (Plan 05) drives.
   **MoltenVK constraint:** indirect-count draws map to Metal indirect command buffers;
   the plan verifies the feature/limit support on the dev device and validates under
   `VE_DEBUG`, the same gate discipline as every GPU feature.

7. **A `CullMode` setting selects CPU vs GPU; occlusion is its own toggle.** The
   `SceneRenderer` settings grow `CullMode` (CPU frustum / GPU frustum+occlusion) and an
   occlusion toggle — recompile knobs (the GPU path is a different pass topology). The
   CPU path planset-21 built stays the default and the fallback where the indirect/compute
   feature set is unavailable.

8. **Byte-identical under frustum-only; occlusion guarded by draw-count, not the
   golden.** GPU frustum culling produces the same visible set as CPU frustum culling →
   the golden does not move under it. Occlusion culling changes *which* draws issue
   (dropping hidden ones) but **not the resulting pixels** (a hidden draw contributes
   nothing), so the golden still should not move — the guard is a draw-count fixture
   with a provably-occluded mesh (an occluder fully covering it), asserting it is
   gathered, uploaded, but not drawn, and that disabling occlusion draws strictly more.

9. **Out of scope:** meshlet/cluster-granularity GPU culling, two-pass occlusion (the
   named higher-quality alternative, decision 4), portal/PVS visibility, GPU-driven
   *shadow* culling (an obvious later consumer), and predicated/query-based occlusion
   (hi-Z is the chosen mechanism). Each is a named follow-on behind this delivered
   GPU-driven path.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | Per-submesh bounds + CPU per-submesh cull | `SubMesh` local `AABB` derived at load (no cooked-format change); the CPU gather/passes cull per-submesh against the frustum. Pure refinement, byte-identical, draw-count fixture (partially-off-frustum mesh). Device-free bound math unit-tested. | proposed |
| 02 | Hi-Z depth pyramid | A compute min/max depth mip-chain reduction over the depth target into a reusable renderer-owned hi-Z resource. GPU; validation gate. | proposed |
| 03 | Hi-Z occlusion test (temporal) | Test each candidate's screen AABB against the previous-frame hi-Z pyramid; drop provably-occluded candidates (conservative). Draw-count fixture (occluder fully covering a mesh); golden unmoved. | proposed |
| 04 | Indirect-draw infrastructure | `Context`/`RenderGraph`/`CommandBuffer` support for GPU draw-arg buffers + `DrawIndexedIndirectCount` (GPU-sourced count), behind vocabulary types; MoltenVK feature/limit verification + validation gate. | proposed |
| 05 | GPU compute-driven cull → indirect draw | Upload the candidate set; a compute pass runs frustum + hi-Z tests and compacts survivors into the indirect buffer + count; the geometry pass drives `DrawIndexedIndirectCount`. Golden byte-identical; draw-count via the GPU counter. | proposed |
| 06 | `CullMode` settings + debug + docs/roadmap | `CullMode` (CPU/GPU) + occlusion toggle (recompile knobs); CPU fallback where the feature set is absent; debug stats (gathered/uploaded/survived/drawn); roadmap re-cut (meshlet/cluster culling + two-pass occlusion + GPU-driven shadow culling named next). | proposed |

## Dependency analysis

```
01 (per-submesh bounds + CPU cull, pure-ish)
   └──► 02 (hi-Z pyramid) ──► 03 (occlusion test)
                                   └──► 04 (indirect infra) ──► 05 (compute cull → indirect) ──► 06 (settings + docs)
```

Per-submesh CPU culling (01) is the foundation refinement and lands first. The hi-Z
pyramid (02) and occlusion test (03) add the occlusion stage on the CPU/test side. The
indirect infrastructure (04) is the prerequisite for the compute-driven cull (05),
which is the deepest change — moving the whole frustum+occlusion test GPU-side and the
draw submission to indirect. Settings/debug/docs (06) close it.

## On completion

The deferred renderer culls at **submesh granularity**, occludes against a **hi-Z
pyramid**, and — under `CullMode::GPU` — runs the entire frustum+occlusion test in a
**compute** pass that compacts survivors into an **indirect draw** buffer the geometry
pass issues through `DrawIndexedIndirectCount`, with the CPU gather as the upload
source and the CPU path as the fallback. The rendered image is unchanged
(frustum-identical, occlusion drops only the provably hidden); draw-count fixtures are
the guard. Meshlet/cluster-granularity culling, two-pass occlusion, and GPU-driven
shadow-caster culling are the named refinements behind this delivered GPU-driven path,
and the GPU candidate set is the natural consumer of the named **BVH broadphase**
(which narrows the candidates the compute pass tests).
</content>
