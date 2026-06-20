# planset-25 — occlusion + GPU/compute-driven culling

**Phase goal:** take culling past [planset-21](../planset-21/README.md)'s mesh-granularity
frustum gather and [planset-23](../planset-23/README.md)'s **delivered BVH broadphase** to
**GPU-driven**: per-**submesh** BVH leaves, **hi-Z occlusion** culling, and a **compute
occlusion test → indirect draw** path that moves the visibility decision onto the GPU and
issues the surviving draws through a `multiDrawIndirect` command buffer. These are three of
the named refinements `future/scene-renderer.md` records behind the delivered broadphase:
"GPU/occlusion culling" and "per-submesh leaves" (`future/scene-renderer.md` lines 442, 546).
It is the **deepest** culling increment — it reworks how draws are submitted, not just which
the CPU records — and the most GPU-infrastructure-heavy increment in the culling area.

The work is staged so each layer is independently valuable and verifiable: per-submesh BVH
leaves (Plan 01) are a pure refinement of the delivered broadphase; the hi-Z pyramid (Plan 02)
and the occlusion-test primitive (Plan 03) build and prove the GPU occlusion stage without
yet dropping a draw; the indirect-draw infrastructure (Plan 04) and the compute cull (Plan 05)
move the test GPU-side and submit the survivors indirectly. The rendered image stays
**byte-identical** under frustum-only culling; occlusion culling drops only genuinely-occluded
draws (conservative, never a visible artifact), pinned by draw-count fixtures and a
GPU-survivor-set ↔ CPU-cull-set equivalence test rather than the golden.

> **Status:** reviewed. This README and all six plan files are drafted and approved (`ready` in
> the table below); implementation follows the cadence in **Process & conventions**, one commit
> per plan in the table order.

## Dependencies & relationship to the delivered broadphase

planset-23's BVH broadphase is **delivered**: a renderer-owned `SceneBroadphase`
([engine/include/Veng/Scene/SceneBroadphase.h](../../engine/include/Veng/Scene/SceneBroadphase.h))
holds a BVH over the resident draw candidates, rebuilt only when the scene's spatial version
moves, and every view (`SceneRenderer.cpp:371`) culls by **tree descent** rather than a linear
scan. The pure per-frame gather it caches is planset-21's `GatherMeshes`
([engine/include/Veng/Scene/Visibility.h](../../engine/include/Veng/Scene/Visibility.h)). This
planset is a set of **refinements on top of that delivered tree**, not a parallel structure:

- **Plan 01 makes the tree's leaves per-submesh** — a finer granularity of the *same*
  `SceneBroadphase`, exactly the "per-submesh leaves" refinement `future/scene-renderer.md`
  names (line 546).
- **The GPU path's candidate source is the broadphase's frustum survivors, not a fresh gather.**
  The CPU descends the BVH with the camera frustum (`SceneBroadphase::Cull`), and *that
  survivor set* is what the GPU path uploads and **occlusion-tests** — the GPU does **not**
  re-run frustum culling the tree already did. So the division of labor is fixed: **BVH
  frustum-descent (CPU) → GPU hi-Z occlusion → indirect draw.**

It does **not** depend on [planset-24](../planset-24/README.md) (shadowed punctual lights,
delivered), though planset-24's `N` spot + `6N` cube shadow views are the workload where
GPU-driven culling pays off most — named as the next consumer in decision 9, not built here.

## A verified MoltenVK finding fixes the indirect-draw mechanism

**Finding (2026-06, verified with `vulkaninfo` against the installed MoltenVK 1.4 ICD on the
M-series dev box):** `drawIndirectCount = false`, `multiDrawIndirect = true`,
`maxDrawIndirectCount = 1073741824`. So `vkCmdDrawIndexedIndirectCount` (which requires the
Vulkan 1.2 `drawIndirectCount` feature) is **not available on the primary platform**;
`vkCmdDrawIndexedIndirect` over a multi-draw command buffer **is**.

This decides Plan 04/05's submission shape. The indirect buffer holds a **fixed maximum** of
`VkDrawIndexedIndirectCommand` slots — one per uploaded candidate submesh — and the compute
cull writes each command's `instanceCount` to **1 for a survivor and 0 for a culled
candidate** (a zero-instance command executes as a no-op). The geometry pass issues a single
`vkCmdDrawIndexedIndirect` over the whole fixed buffer; there is **no GPU-sourced count**.
Compaction-into-a-count-buffer is recorded as the refinement for when MoltenVK gains
`drawIndirectCount`. Plan 04 additionally **probes `drawIndirectFirstInstance`** — the cull carries
each candidate's index in the command's `firstInstance`, read back as an instance-rate vertex
attribute — and verifies that base-instance read path on the dev box before Plan 05 depends on it;
`CullMode::GPU` is gated on both `multiDrawIndirect` and `drawIndirectFirstInstance`. This is the
same dev-box-verified-finding discipline as the multiview block in `future/scene-renderer.md`
(lines 478–489).

## Scope decisions

Ten decisions fix the boundary of this work.

1. **Per-submesh bounds derive at load — no cooked-format change.** Each `SubMesh`
   ([Mesh.h:28](../../engine/include/Veng/Asset/Mesh.h)) gains a local-space `AABB` folded over
   its index range (`[IndexOffset, IndexOffset + IndexCount)`) through the canonical vertex
   positions at load — a **new** fold distinct from the whole-mesh `Mesh::ComputeBounds`, which
   folds a contiguous vertex span and does not index. It is populated on **both** the cooked
   `MeshLoader` path and the runtime `MeshData`/`Primitives` path, and never serialized (no
   on-disk format change). The per-submesh bound is the leaf granularity the tree and the GPU
   path operate at.

2. **Plan 01 makes the BVH's leaves per-submesh — not a linear sub-scan.** The delivered cull
   is a BVH descent at mesh granularity (`SceneBroadphase::Cull`); per-submesh culling is
   expressed by emitting **one BVH leaf per submesh** (`BVH::Leaf` already carries an opaque
   `u32 Id` payload, [BVH.h:21](../../engine/include/Veng/Math/BVH.h)), so `Cull` returns
   per-submesh survivors by the same sub-linear descent. It is **not** a linear `Intersects`
   loop under the tree — that would regress the sub-linearity planset-23 delivered. Byte-identical
   (a submesh the frustum touches still draws); guarded by a draw-count fixture over a
   partially-off-frustum mesh and device-free bound-math unit tests. Lands first, no GPU machinery.

3. **Hi-Z is a depth pyramid built by compute, persisted across frames.** Plan 02 builds a
   **max-Z** mip chain (the reduction that makes "fully behind the nearest stored depth"
   conservative for the engine's `D32Sfloat` `[0,1]` depth, [GBuffer.h:50](../../engine/include/Veng/Renderer/GBuffer.h))
   from the depth target via a compute reduction into a **renderer-owned resource persisted
   across frames** — not a graph transient, because the occlusion test reads *last frame's*
   pyramid (decision 4), which a single-copy cleared depth target does not retain. Building the
   chain extends `RenderGraph` with a **per-mip (subresource) image surface** — the image-axis
   analogue of decision 5's buffer surface — so the chain's per-mip read-after-write barriers stay
   graph-derived; the cross-frame read is a renderer-recorded transition (the graph compiles once
   and cannot see across frames). A CPU reference-reduction test pins correctness.

4. **The occlusion test is conservative and temporal (previous-frame pyramid).** Plan 03 tests
   a candidate's screen-space AABB (the projected 8 corners' screen bound, mip selected from the
   footprint's pixel extent) against the **previous-frame** hi-Z pyramid: a candidate fully
   behind the nearest stored depth over its footprint is occluded. Temporal hi-Z avoids a
   mid-frame depth dependency at the cost of one frame of latency on newly disoccluded objects
   (a brief pop, the standard trade). **Conservative is the correctness bar — occlusion never
   drops a visible draw**, so where last frame's depth is invalid (the first frame, the frame
   after a `Resize`/`Configure` recreates the pyramid, or a detected large view delta) the test
   is **skipped** that frame (frustum-only) rather than risk a stale false-cull. The two-pass
   form (depth prepass this frame → build hi-Z → cull the main pass) is the named
   higher-quality alternative (decision 10), not built.

5. **Indirect-draw infrastructure is net-new engine capability — including graph-tracked
   buffers.** Plan 04 adds: a **graph buffer-resource class** (the `RenderGraph` is image-only
   today — `TransientDesc`, `PassContext::Resolved`, and `AccessKind`
   ([Types.h:177](../../engine/include/Veng/Renderer/Types.h)) all name images), a
   `BufferUsage::Indirect` flag, an `AccessKind::IndirectRead` with its
   `eDrawIndirect`/`eIndirectCommandRead` barrier scope, and `CommandBuffer::DrawIndexedIndirect`
   over a `multiDrawIndirect` buffer. This is the reusable seam the compute cull (Plan 05) drives,
   and is the **buffer axis** of the graph extension — orthogonal to Plan 02's **per-mip image**
   axis (decision 3). The device-feature set
   ([Context.cpp:800](../../engine/src/Renderer/Backend/Context.cpp)) gains `multiDrawIndirect` +
   `drawIndirectFirstInstance`, gated on a support check; validation under `VE_DEBUG`.

6. **GPU-driven culling uploads the broadphase's frustum survivors and emits indirect draws.**
   Plan 05 uploads the camera-frustum survivors (`SceneBroadphase::Cull` →
   `GetSubMeshCandidates()[idx]`, world matrix + bounds + per-submesh draw args) to a GPU buffer; a
   **compute** shader runs the **hi-Z occlusion test** (decision 4) per candidate and writes each
   `VkDrawIndexedIndirectCommand`'s `instanceCount` (1 survivor / 0 culled); the geometry pass
   issues one `vkCmdDrawIndexedIndirect` per mesh group. The compute does **not** re-run frustum
   culling — the BVH already did. The CPU gather/broadphase is the *upload source*; the occlusion
   decision and draw submission move GPU-side. The per-draw data the CPU pushes today (the MVP and
   normal-matrix push constants, the per-submesh material bind — `SceneRenderer.cpp:357–362`) moves
   into GPU buffers indexed by the candidate id — carried in the command's `firstInstance`, read as
   an instance-rate vertex attribute — since an indirect draw runs with no per-draw CPU callback;
   and **the CPU path migrates onto the same buffer-indexed draw** so the two share one surface
   shader (decision 7).

7. **A `CullMode` setting selects CPU vs GPU; occlusion is the GPU path's own toggle.** The
   `SceneRenderer` settings grow `CullMode` (CPU = BVH frustum descent + direct draws, the
   planset-21/23 path; GPU = BVH frustum descent + GPU hi-Z occlusion + indirect draws) beside
   `FrustumCull`, plus an occlusion on/off toggle. Both modes drive the **same buffer-indexed draw**
   (decision 6) — differing only in submission (direct vs. indirect) and who writes `instanceCount`
   — so there is one surface shader, not a per-mode fork. These are **recompile knobs** (`Configure`
   — the GPU path is a different pass topology, so it invalidates `GetOutput()`, the deliberate
   recompile seam, unlike planset-21's incidental one). The CPU path stays the default and the
   **fallback** where `multiDrawIndirect`/`drawIndirectFirstInstance` is unavailable.

8. **Byte-identical under frustum-only; occlusion guarded by draw-count + set-equivalence, not
   the golden.** GPU frustum survivors equal CPU frustum survivors → the golden does not move.
   The compute compaction emits survivors in dispatch order, **not** `GatherMeshes` order, so the
   byte-identical claim rests on the opaque deferred g-buffer being **draw-order-independent**
   (as planset-23's did, [planset-23/README.md:88](../planset-23/README.md)), stated explicitly.
   Occlusion changes *which* draws issue but not the pixels (a hidden draw contributes nothing),
   so the golden still does not move. Because a conservative *over*-draw bug moves neither the
   golden nor a count, the guard is twofold: a **draw-count fixture** with a provably-occluded
   mesh (an occluder fully covering it), and a **GPU-survivor-set ↔ CPU-`Cull`-set equivalence**
   readback on a frustum-only frame (occlusion off) — the GPU analogue of planset-23's "query ==
   linear scan" test.

9. **The GPU path's biggest payoff — many-view shadow culling — is the named next consumer, not
   built here.** planset-24's per-light shadow cull is `N` spot + `6N` cube frustum queries per
   frame, the workload where uploading once and testing against many frustums in compute wins
   most. This planset deliberately proves the GPU path on the **camera** view first; GPU-driven
   shadow culling reads the indirect/compute seam Plan 05 builds and is the obvious follow-on.

10. **Out of scope:** meshlet/cluster-granularity GPU culling, **two-pass occlusion** (the
    higher-quality alternative to temporal hi-Z, decision 4), portal/PVS visibility, GPU-driven
    *shadow* culling (decision 9), GPU **compaction into a count buffer** (blocked on MoltenVK
    `drawIndirectCount`; the `instanceCount=0` no-op shape ships instead), and predicated/query
    occlusion (hi-Z is the chosen mechanism). Each is a named follow-on behind this delivered
    GPU-driven path.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Per-submesh bounds + per-submesh BVH leaves](01-per-submesh-leaves.md) | `SubMesh` local `AABB` folded over its index range at load (no cooked-format change, both load paths); `SceneBroadphase`/`GatherMeshes` emit a BVH leaf per submesh so `Cull` returns per-submesh survivors by tree descent; the g-buffer + shadow passes draw the surviving submeshes. A submesh-granularity drawn counter. Byte-identical; draw-count fixture (partially-off-frustum mesh); device-free bound + leaf-equivalence unit tests. | ready |
| 02 | [Hi-Z depth pyramid](02-hi-z-pyramid.md) | A compute max-Z mip-chain reduction over the depth target into a renderer-owned, cross-frame-persisted hi-Z resource, on a net-new **per-mip (subresource) graph surface**. GPU; CPU reference-reduction + per-subresource barrier tests; validation gate. | ready |
| 03 | [Hi-Z occlusion-test primitive](03-occlusion-test.md) | The screen-AABB-vs-previous-frame-pyramid occlusion test, proven in isolation (a compute pass writing a per-candidate visibility buffer, read back in a test; occluder-fully-covering-a-mesh → occluded). History-invalid → frustum-only fallback. Not yet wired into drawing; golden unmoved. | ready |
| 04 | [Indirect-draw infrastructure](04-indirect-infra.md) | Graph buffer-resource class + `BufferUsage::Indirect` + `AccessKind::IndirectRead`/barrier scope; `CommandBuffer::DrawIndexedIndirect` over `multiDrawIndirect`; the `multiDrawIndirect` + `drawIndirectFirstInstance` device gate (the base-instance index path verified here) + the verified MoltenVK finding; validation gate. | ready |
| 05 | [GPU compute occlusion cull → indirect draw](05-compute-cull.md) | Upload the BVH-frustum survivors; a compute pass runs hi-Z occlusion and writes each indirect command's `instanceCount` (1/0); the geometry pass drives one `vkCmdDrawIndexedIndirect` per mesh group. Per-draw transform/material moves to GPU buffers indexed by the candidate id (`firstInstance` + instance attribute); the **CPU path migrates onto the same buffer-indexed draw** (one shared surface shader). Golden byte-identical (order-independent g-buffer); set-equivalence + draw-count guards. | ready |
| 06 | [`CullMode` settings + debug + docs/roadmap](06-settings-docs.md) | `CullMode` (CPU/GPU) + occlusion toggle (recompile knobs); CPU fallback where `multiDrawIndirect`/`drawIndirectFirstInstance` is absent, with `GetActiveCullMode()` reporting the real mode; debug stats (gathered/frustum-survived/occlusion-survived/drawn); `CLAUDE.md` + `future/scene-renderer.md` re-cut (meshlet/cluster culling, two-pass occlusion, GPU-driven shadow culling named next). | ready |

## Dependency analysis

```
01 (per-submesh BVH leaves — pure CPU refinement of the delivered broadphase)
   │
   └──► 05 (compute occlusion cull → indirect draw) ──► 06 (settings + docs)
            ▲                    ▲
02 (hi-Z pyramid) ──► 03 (occlusion test) ─┘          04 (indirect infra) ─┘
```

Per-submesh BVH leaves (01) refine the delivered tree and land first — independent of all GPU
work. The hi-Z pyramid (02) and the occlusion-test primitive (03) build and prove the GPU
occlusion stage in isolation, dropping no draws — **02 also lands the per-mip (subresource)
image-graph surface** the reduction needs. The indirect infrastructure (04) — the **buffer**-graph
surface + `multiDrawIndirect`/`drawIndirectFirstInstance` — is independent of 02/03 (a different
graph axis) and is the other prerequisite for the compute cull (05), the deepest change: it
consumes 01's per-submesh candidates, 03's occlusion test, and 04's indirect seam, moving the
occlusion decision GPU-side, the draw to indirect, and the CPU path onto the shared buffer-indexed
draw. Settings/debug/docs (06) close it. **Order: 01, then (02 → 03) ∥ 04, then 05 → 06.**

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify (clean
build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a device is
present, the smoke PPM correct size + exit 0, the `smoke_golden` capture **re-checked and
unmoved**) → update this table → one commit per plan (`Plan NN: <summary>`, `Co-Authored-By`
trailer).

Common to all plans:

- **`smoke_golden` stays byte-identical across the whole planset.** Per-submesh and GPU frustum
  culling produce the same visible set; occlusion drops only the provably hidden; the opaque
  deferred g-buffer is draw-order-independent, so GPU-compacted (unordered) survivors render the
  same pixels. A moved golden is a bug — no golden regeneration.
- **The occlusion guard is draw-count + set-equivalence, not the golden** (decision 8). A
  draw-count fixture with a provably-occluded mesh, plus a GPU-survivor-set ↔ CPU-`Cull`-set
  equivalence readback (occlusion off) — the GPU analogue of planset-23's pure equivalence test.
  Plan 02's hi-Z reduction is pinned by a CPU reference-reduction comparison.
- **New core-pack compute shaders need minted `AssetId`s.** Plans 02, 03, and 05 each add a
  compute shader to the core pack — author with a clearly-marked placeholder id while
  implementing, then mint the real ids with `vengc generate-id` once the build is verified (hex
  in C++, decimal in JSON), per the working norm.
- **Run the validation gate under `VE_DEBUG`** (`ctest --test-dir build-debug -L validation`).
  The GPU plans (02–05) add compute passes, a storage/indirect buffer, and the compute-write →
  indirect-read barrier; pre-empt the indirect footguns (`INDIRECT_BUFFER_BIT` usage, the
  compute→indirect barrier, 4-byte count/offset alignment) so the gate stays clean.
- **Contract comments are present-tense facts** — the `CLAUDE.md` comment policy applies; no
  plan-citation narrative, no future-work hedging in code or doc comments.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

The deferred renderer culls at **submesh granularity** through the delivered BVH, occludes
against a **hi-Z pyramid**, and — under `CullMode::GPU` — runs the frustum-survivors' occlusion
test in a **compute** pass that writes per-candidate `instanceCount` into an indirect command
buffer the geometry pass issues through `vkCmdDrawIndexedIndirect` (`multiDrawIndirect`, the
`drawIndirectCount`-free shape this MoltenVK supports), with the broadphase's CPU frustum
descent as the upload source and the CPU path as the fallback. The rendered image is unchanged
(frustum-identical, occlusion drops only the provably hidden, the g-buffer order-independent);
draw-count fixtures and a GPU↔CPU set-equivalence test are the guard. The named refinements
(decision 10) sit behind this delivered path — chief among them **GPU-driven shadow-caster
culling**, the highest-payoff consumer (planset-24's `N + 6N` shadow views).
</content>
</invoke>
