# Plan 04 — Indirect-draw infrastructure

**Goal:** add the net-new engine capability the compute cull (Plan 05) drives — **graph-tracked
GPU buffers**, an **indirect-read barrier**, and `CommandBuffer::DrawIndexedIndirect` over a
`multiDrawIndirect` command buffer — behind the engine's vocabulary types and the verified
MoltenVK finding. No `SceneRenderer` topology change here; this is the reusable seam. Independent
of Plans 02/03; the other prerequisite for Plan 05.

## What lands

### Graph buffer resources ([engine/include/Veng/Renderer/RenderGraph.h](../../engine/include/Veng/Renderer/RenderGraph.h) + src)

The `RenderGraph` is **image-only** today: `TransientDesc`
([RenderGraph.h:58](../../engine/include/Veng/Renderer/RenderGraph.h)) carries Format/Extent/
`ImageUsage`, `Import` and `PassContext::Resolved` resolve to `Ref<ImageView>`, and a pass's
declared reads/writes name images. A GPU-resident indirect-args buffer the compute pass writes
and the graphics pass reads as draw arguments has **no representation**, so its barrier cannot
fall out of declared use — the graph's entire contract. This plan adds the buffer as a
first-class graph resource:

- **`CreateTransientBuffer({.Bytes, .Usage})`** — a graph-owned transient buffer, allocated at
  compile and resolved per frame, alongside `CreateTransient` for images. (Indirect args are
  small and persist within a frame; aliasing is not required and is left to the image path.)
- **`ImportBuffer(name)`** — an external `Ref<Buffer>` supplied per frame as an `ImportBinding`,
  the buffer analogue of the existing image import (Plan 05 imports the candidate-upload buffer).
- **`PassContext::ResolvedBuffer(ResourceId)`** — the per-frame concrete `Ref<Buffer>` a callback
  records against, mirroring `Resolved(id)` for images.

### `BufferUsage::Indirect` + `AccessKind::IndirectRead` ([engine/include/Veng/Renderer/Types.h](../../engine/include/Veng/Renderer/Types.h))

- `BufferUsage` gains `Indirect` (→ `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` in
  [TypeMapping.h](../../engine/include/Veng/Renderer/Backend/TypeMapping.h)); the cull also needs
  `Storage` so compute can write the command buffer.
- `AccessKind` ([Types.h:177](../../engine/include/Veng/Renderer/Types.h)) gains:
  - **`IndirectRead`** — read as draw/dispatch arguments (`eDrawIndirect` stage,
    `eIndirectCommandRead` access).
  - **`StorageBufferRead`/`StorageBufferWrite`** — buffer storage access, distinct from the
    existing `StorageRead`/`StorageWrite`, which are storage-*image* layouts. (A buffer has no
    layout, so the buffer barrier path is a stage+access memory barrier, not an image transition.)

`BarrierDecision`'s `ScopeFor` ([engine/src/Renderer/Backend/BarrierDecision.cpp](../../engine/src/Renderer/Backend/BarrierDecision.cpp))
grows the buffer-access cases — for a buffer resource it emits a `vk::BufferMemoryBarrier2`
(stage/access only, no layout) rather than the image transition the current branches return. The
**load-bearing barrier** this unlocks: a compute pass declaring `.StorageBufferWrite` on the
indirect buffer followed by a graphics pass declaring `.IndirectRead` on it yields a
`eComputeShader/eShaderWrite → eDrawIndirect/eIndirectCommandRead` barrier, derived, not
hand-written.

### `CommandBuffer::DrawIndexedIndirect` ([engine/include/Veng/Renderer/CommandBuffer.h](../../engine/include/Veng/Renderer/CommandBuffer.h) + src)

```cpp
/// @brief Issues drawCount indexed draws from GPU-resident VkDrawIndexedIndirectCommand records.
///
/// Maps to vkCmdDrawIndexedIndirect (multiDrawIndirect). A command with
/// instanceCount == 0 executes as a no-op — the cull (Plan 05) zeroes culled
/// candidates rather than compacting, since drawIndirectCount is unavailable on
/// MoltenVK (see the planset finding).
/// @pre buffer was created with BufferUsage::Indirect and is barriered for eDrawIndirect.
/// @pre A command with a non-zero firstInstance requires the drawIndirectFirstInstance
///      device feature (Plan 05 carries the candidate index there).
void DrawIndexedIndirect(const Ref<Buffer>& buffer, u64 offset, u32 drawCount, u32 stride);
```

`stride` is `sizeof(VkDrawIndexedIndirectCommand)` (20 bytes); `offset` is 4-byte aligned. No
count-buffer variant ships (`vkCmdDrawIndexedIndirectCount` needs `drawIndirectCount`, absent on
the dev box) — the count is the CPU-known fixed candidate maximum.

### The device features ([engine/src/Renderer/Backend/Context.cpp](../../engine/src/Renderer/Backend/Context.cpp))

The enabled-feature set ([Context.cpp:800](../../engine/src/Renderer/Backend/Context.cpp))
requests `sampleRateShading`, `samplerAnisotropy`, and `shaderSampledImageArrayDynamicIndexing`
(plus `dynamicRendering`/`timelineSemaphore` via feature-2 chains). The GPU cull adds two core
`vk::PhysicalDeviceFeatures` bools, each gated on a `getFeatures2` support check:

- **`multiDrawIndirect`** — `vkCmdDrawIndexedIndirect` over a multi-draw command buffer.
- **`drawIndirectFirstInstance`** — a **non-zero `firstInstance`** in an indirect command. The
  cull carries each candidate's index in `firstInstance` (Plan 05's draw-data reorg), so the
  indirect draw needs this feature; a direct (CPU-path) draw with a non-zero `firstInstance` does
  not.

If either is unsupported, neither is enabled and `CullMode::GPU` is unavailable — the CPU path is
the fallback (Plan 06). The verified finding (`multiDrawIndirect = true`, `drawIndirectCount =
false`, `maxDrawIndirectCount = 1073741824` on the dev MoltenVK ICD) is recorded in the planset
README; **`drawIndirectFirstInstance` and the candidate-index read mechanism are verified on the
dev box the same way before Plan 05 builds on them** (decision 5). The absent `drawIndirectCount`
is why the count-buffer path is out of scope.

## Decisions

1. **Buffers become first-class graph resources, not a side channel.** The compute→indirect
   hazard is exactly what the graph exists to derive; smuggling the indirect buffer past the
   graph (a manually-barriered app buffer) would re-introduce the hand-written barriers
   `RenderGraph` abolished and break under the validation gate. Adding the buffer resource +
   `IndirectRead` access keeps the barrier derived. This is generally useful (any future
   compute→graphics buffer handoff reuses it), which is why it is its own plan, ahead of the cull.

2. **`multiDrawIndirect` + `instanceCount`-gating, not count-buffer compaction.** The verified
   absence of `drawIndirectCount` on MoltenVK rules out `vkCmdDrawIndexedIndirectCount`. The
   portable shape is a **fixed-max** command buffer (one slot per uploaded candidate) where the
   cull writes `instanceCount = 0` into culled slots so they no-op, and a single
   `vkCmdDrawIndexedIndirect` over the whole buffer issues the survivors. `maxDrawIndirectCount`
   is effectively unbounded (`2^30`), so the fixed max never hits the limit at veng's scene
   scale. Count-buffer compaction is the named refinement for a future MoltenVK with
   `drawIndirectCount`.

3. **No `SceneRenderer` change in this plan.** Plan 04 lands the seam (graph buffers, the access
   kinds, the draw entry point, the feature) and pins it with a focused `gpu`-band test driving an
   indirect draw from a hand-filled command buffer. Wiring it into the deferred topology — the
   candidate upload, the cull compute pass, the draw-data reorg — is Plan 05, so a barrier or
   feature bug surfaces here in isolation.

4. **Pre-empt the validation footguns.** GPU-sourced indirect draw trips the validation gate on
   missing `INDIRECT_BUFFER_BIT` usage, a missing compute→indirect barrier, or a misaligned
   offset/stride — and the messenger only logs, it does not abort
   ([CLAUDE.md](../../CLAUDE.md)), so an omission surfaces only as a gate ERROR or a MoltenVK
   silent miss. This plan builds the usage flag, the derived barrier, and 4-byte alignment in
   from the start and pins them under `VE_DEBUG`.

5. **Verify the base-instance path, not just the draw call.** Plan 05 carries each candidate's
   index in the command's `firstInstance` and reads it back in the surface shader. That depends on
   `drawIndirectFirstInstance` *and* on the shader actually receiving the per-command base instance
   — a known MoltenVK weak spot. The portable read mechanism Plan 05 commits to is an
   **instance-rate vertex attribute** carrying the candidate index, fetched at `firstInstance` for
   `instanceCount == 1` (no `shaderDrawParameters` dependency, identical for direct and indirect
   draws); `drawIndirectFirstInstance` gates only the indirect use of a non-zero `firstInstance`.
   This plan's `gpu`-band indirect-draw test asserts the base instance reaches the shader (renders
   the right per-command data), so a MoltenVK base-instance miss surfaces here, in isolation,
   before the cull depends on it.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/RenderGraph.h` + `engine/src/Renderer/RenderGraph.cpp` | `CreateTransientBuffer`, `ImportBuffer`, `PassContext::ResolvedBuffer`; buffer resource in the resource table + compile/resolve. |
| `engine/include/Veng/Renderer/Types.h` | `BufferUsage::Indirect`; `AccessKind::IndirectRead`/`StorageBufferRead`/`StorageBufferWrite`. |
| `engine/include/Veng/Renderer/Backend/TypeMapping.h` | Map the new usage + access values (exhaustive switch — asserts on unmapped). |
| `engine/src/Renderer/Backend/BarrierDecision.cpp` | Buffer-memory-barrier path for the buffer access kinds (stage/access, no layout). |
| `engine/include/Veng/Renderer/CommandBuffer.h` + `engine/src/Renderer/Backend/CommandBuffer.cpp` | `DrawIndexedIndirect`. |
| `engine/src/Renderer/Backend/Context.cpp` | Enable `multiDrawIndirect` + `drawIndirectFirstInstance`, gated on the support check. |
| `tests/gpu/` (new `indirect_draw.cpp`) + `tests/unit/barrier_decision.cpp` + suite lists | Hand-filled indirect draw + the compute→indirect barrier-decision case. |

## Verification

- Clean build; `include_hygiene` still compiles every public header (the new graph/types/command
  surface is glm/vk-free in its signatures — `Ref<Buffer>`, `ResourceId`, vocabulary enums; the
  backend stays PRIVATE).
- **Barrier-decision unit test** (device-free): a `StorageBufferWrite` on a buffer followed by an
  `IndirectRead` on it derives a `eComputeShader/eShaderWrite → eDrawIndirect/eIndirectCommandRead`
  buffer barrier; a buffer access emits a buffer-memory-barrier, never an image transition.
- **GPU indirect-draw test** (`gpu` band): hand-fill a command buffer with two
  `VkDrawIndexedIndirectCommand`s — one `instanceCount = 1`, one `instanceCount = 0` — issue
  `DrawIndexedIndirect` over both, and assert exactly the first geometry is rendered (the
  zero-instance command no-ops). Give the surviving command a **non-zero `firstInstance`** and an
  instance-rate attribute keyed on it, and assert the shader read the right index — the
  base-instance path Plan 05 depends on, verified here (decision 5). Run on the box where
  `multiDrawIndirect`/`drawIndirectFirstInstance` are present; `SKIP` (77) with no ICD or no
  feature.
- **`smoke_golden` byte-identical** — no renderer topology change; re-check, do not regenerate.
- **Validation gate clean under `VE_DEBUG`** — the indirect draw's usage flag, derived barrier,
  and alignment are pinned by the `validation_gate` run.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present.
</content>
