# Plan 05 — GPU compute occlusion cull → indirect draw

**Goal:** move the occlusion decision and the draw submission GPU-side. Under `CullMode::GPU`,
upload the broadphase's camera-frustum survivors, run a **compute** pass that hi-Z-occludes each
and writes its indirect command's `instanceCount` (1/0), and issue the geometry pass through one
`vkCmdDrawIndexedIndirect` per mesh group. The per-draw transform/material data moves to GPU
buffers indexed by the candidate id (`firstInstance` + instance attribute), and the CPU path
migrates onto the **same** buffer-indexed draw. The deepest change in the planset; depends on
Plans 01 (per-submesh candidates), 03 (the occlusion test), and 04 (the indirect seam). Golden
byte-identical.

## What lands

### The candidate upload ([engine/src/Renderer/SceneRenderer.cpp](../../engine/src/Renderer/SceneRenderer.cpp))

The CPU descends the BVH with the camera frustum exactly as the CPU path does
([SceneRenderer.cpp:371](../../engine/src/Renderer/SceneRenderer.cpp)
`Broadphase->Cull(cameraFrustum, scratch)`), then uploads **those frustum survivors** — not a
fresh gather, and not the full candidate set — into a per-frame GPU buffer. The compute pass does
**not** re-run frustum culling; the tree already did (the division of labor fixed in the planset
README). Each uploaded record (one per surviving submesh candidate, Plan 01) carries:

```hlsl
struct CullCandidate {
    float3 boundsMin;    // world-space submesh AABB
    float3 boundsMax;
    float4x4 world;      // per-instance model matrix (replaces the CPU MVP push)
    uint  indexCount;    // VkDrawIndexedIndirectCommand fields:
    uint  firstIndex;
    int   vertexOffset;
    uint  materialIndex; // selects the per-draw material record
};
```

The buffer is ring-allocated for frames-in-flight (like the light/view-constants buffers) and
`ImportBuffer`ed into the internal graph (Plan 04).

### The cull compute pass

A core-pack compute shader (`occlusion_cull.comp.slang`, placeholder `AssetId`) runs one
invocation per uploaded candidate:

1. Calls `IsOccluded(boundsMin, boundsMax, prevViewProj, hiZ, …)` from Plan 03's shared header —
   **but only if** the renderer's `m_HiZHistoryValid` flag (Plan 03) is set; when history is
   invalid the pass treats every candidate as visible (frustum-only that frame).
2. Writes the candidate's `VkDrawIndexedIndirectCommand` into the indirect buffer at its slot:
   `{ indexCount, instanceCount = occluded ? 0 : 1, firstIndex, vertexOffset, firstInstance =
   <candidate index> }`. A culled candidate's `instanceCount = 0` makes its command a no-op
   (Plan 04's shape — no compaction, since `drawIndirectCount` is absent).

The pass declares `.StorageBufferWrite` on the indirect buffer and `.Sample` on the hi-Z
pyramid; the graphics pass declares `.IndirectRead` on the indirect buffer — the
compute→indirect barrier falls out (Plan 04). A GPU survivor **count** is also written (an atomic
add) for the debug stat only — it is read back next frame, never gating the draw.

### The unified buffer-indexed draw (CPU and GPU share it)

An indirect draw runs **with no per-draw CPU callback**, so the per-draw state the CPU pushes today
([SceneRenderer.cpp:357–362](../../engine/src/Renderer/SceneRenderer.cpp) — the per-submesh material
bind, the MVP push, the normal-matrix push) must move into GPU buffers indexed per draw. Rather than
fork a second shader path for the indirect case, **both `CullMode`s use the same buffer-indexed
draw** (decision 3); they differ only in *submission* — a direct `DrawIndexed` per surviving submesh
(CPU) vs. one `vkCmdDrawIndexedIndirect` per mesh group (GPU) — and in *who sets `instanceCount`*
(the CPU loop issues only survivors; the GPU compute zeroes culled slots). One surface shader, one
per-draw data layout, for both.

The per-draw index reaches the shader as an **instance-rate vertex attribute** carrying the
candidate id, fetched at the draw's `firstInstance` (for `instanceCount == 1` the fetched element is
`firstInstance`) — the portable mechanism Plan 04 verifies (its decision 5): no `shaderDrawParameters`
dependency, identical for the CPU direct draw and the GPU indirect command, with
`drawIndirectFirstInstance` gating only the indirect non-zero `firstInstance`. The shader then reads:

- **Per-draw transform** — a GPU array of `world` (and the derived normal matrix) indexed by the
  candidate id, replacing the `MeshPushConstants{.MVP}`/`NormalMatrixPush` pushes. The vertex shader
  reads its `world` by that id and multiplies by the view-projection (the same ring-buffered
  per-frame view constants the lighting pass uses).
- **Per-draw material selector** — a GPU array of `materialIndex` indexed by the candidate id,
  replacing the per-submesh `materials[idx].Get()->Bind(cmd)` selector push. The fragment shader
  folds the frame base (`BindlessRegistry::GetCurrentFrameBase()`) into it exactly as
  `Material::Bind` does today, then reads the material's parameter block from the bindless SSBO.
- **Geometry sourcing** — neither a direct nor an indirect multi-draw rebinds vertex/index buffers
  between commands, so candidates are **grouped by source mesh**: the mesh's vertex/index buffers
  bound once, then its surviving submeshes drawn — the CPU path as a `DrawIndexed` per submesh, the
  GPU path as one `vkCmdDrawIndexedIndirect` over that mesh's contiguous command slots — with
  `firstIndex`/`vertexOffset` selecting each submesh's range. This keeps the existing per-mesh buffer
  ownership (no unified mega-buffer) on both paths. (A single global indirect draw over a shared
  geometry buffer is the named refinement, out of scope.)

So Plan 05 **migrates the CPU draw loop** Plan 01 wrote onto these transform/material buffers + the
instance-attribute index, and the `CullMode::CPU`/`GPU` split narrows to submission shape. The CPU
path stays the default and the fallback where `multiDrawIndirect`/`drawIndirectFirstInstance` is
absent — it just no longer carries its own shader.

## Decisions

1. **The compute does occlusion only; the BVH did frustum.** Uploading the frustum survivors and
   re-running frustum in compute would do the tree's work twice. The CPU BVH descent narrows the
   set sub-linearly; the GPU adds the one test the CPU cannot do cheaply (it would need the
   pyramid read back). This is the division of labor the planset README fixes.

2. **`instanceCount = 0` no-ops, not compaction.** With `drawIndirectCount` absent (verified),
   the GPU cannot tell the draw how many compacted commands to issue, so the draw covers the
   fixed candidate maximum and culled slots no-op. The cost is the GPU iterating zero-instance
   commands — negligible at veng's candidate counts and far cheaper than a CPU readback to
   compact. Count-buffer compaction is the refinement behind a future MoltenVK `drawIndirectCount`.

3. **One buffer-indexed draw for both paths — not a second shader fork.** The indirect path forces
   the per-draw push/bind state ([SceneRenderer.cpp:357–362](../../engine/src/Renderer/SceneRenderer.cpp))
   into instance-indexed buffers (no callback between indirect draws). The CPU path *could* keep its
   push-constant loop, but that forks the surface vertex/fragment shaders — a push-constant variant
   and a buffer-indexed variant to keep in sync for every future g-buffer change, with CPU the
   permanent fallback. Instead the CPU path **migrates onto the same buffer-indexed draw**; the
   `CullMode` split reduces to submission (direct vs. indirect) and who writes `instanceCount`. One
   shader, one data layout — the smaller long-term surface, at the cost of touching the CPU draw
   loop Plan 01 wrote. Grouping by source mesh preserves per-mesh buffer ownership on both, with no
   unified geometry buffer.

4. **Byte-identical rests on the order-independent g-buffer.** The compute writes survivor
   commands in dispatch order, not `GatherMeshes` order, so the GPU path does **not** preserve
   draw order. The opaque deferred g-buffer is draw-order-independent (every fragment writes its
   own texel; depth resolves ties deterministically), so unordered survivors produce identical
   pixels — the same argument planset-23 used ([planset-23/README.md:88](../planset-23/README.md)).
   This is why the golden does not move even though the draw order changed.

5. **History-invalid frames are frustum-only, in the shader.** The `m_HiZHistoryValid` flag
   (Plan 03) gates the `IsOccluded` call; an invalid frame writes `instanceCount = 1` for every
   frustum survivor. So a resize, the first frame, or a view cut can only *draw more*, never drop
   a visible submesh — the conservative guarantee, enforced GPU-side.

## Files

| File | Change |
|---|---|
| `engine/src/Renderer/SceneRenderer.cpp` (+ header) | The unified buffer-indexed draw on **both** paths: per-draw transform/material buffers + the instance-attribute candidate index, group-by-mesh; the CPU loop migrated off push constants. The `CullMode::GPU` arm: candidate upload buffer, cull compute pass + indirect geometry pass in the internal graph, the GPU survivor-count readback stat. |
| `engine/assets/` core pack (`occlusion_cull.comp.slang`; the surface vertex/fragment shaders) | The cull compute shader; the surface shaders read `world`/`materialIndex` by the instance-attribute candidate id and multiply by the per-frame view-projection (replacing the MVP/normal/material pushes — used by both `CullMode`s); placeholder `AssetId`s. |
| `tests/gpu/scene_renderer.cpp` | The provably-occluded draw-count fixture + the GPU↔CPU survivor-set equivalence test. |

## Verification

- Clean build; `include_hygiene` unaffected (the `CullMode` arm is renderer-internal).
- **GPU set-equivalence test** (`gpu` band — the primary occlusion guard, the planset-23 analogue):
  with occlusion **off** on a fixed scene, read back the GPU survivor set (the candidates whose
  `instanceCount` the cull wrote `1`) and assert it **equals** the CPU `SceneBroadphase::Cull` set
  for the same camera frustum. This catches a CPU/GPU divergence the golden cannot (both paths
  render correct pixels even if they cull different hidden sets).
- **GPU draw-count fixture** (`gpu` band): an occluder fully covering a mesh → with occlusion
  **on**, `GetLastDrawnCount()` (the GPU survivor-count readback) is strictly **less** than with
  occlusion off, and the covered mesh's command has `instanceCount = 0`; the golden is **unmoved**
  (the hidden mesh contributed nothing either way). The fixture runs **≥2 frames at a fixed pose** —
  frame 0 builds the pyramid (history-invalid → frustum-only) and frame 1 tests against it — since
  occlusion never fires on a first/history-invalid frame.
- **`smoke_golden` byte-identical** under `CullMode::GPU` — this proves the **draw reorder** is
  pixel-safe (GPU frustum survivors == CPU's, the g-buffer order-independent). It does **not**
  witness occlusion: the smoke scene has nothing to occlude, and its single fixed-pose frame is
  history-invalid anyway (frame 0 → frustum-only). Occlusion-drop coverage is the draw-count fixture
  above, not the golden. Re-check, do not regenerate. (The smoke path runs `CullMode::CPU` by
  default, Plan 06; this verifies the GPU arm explicitly.)
- **Validation gate clean under `VE_DEBUG`** — the compute→indirect barrier, the indirect buffer
  usage, and the count/offset alignment (Plan 04) are pinned with the GPU arm active; run
  `validation_gate` with `CullMode::GPU`.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present. On a box
  lacking `multiDrawIndirect`, the GPU-arm tests `SKIP` (77) and the CPU path is exercised instead.
</content>
