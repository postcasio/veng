# 05 — Compute dispatch

**Goal:** make compute actually runnable. Today a `ComputePipeline` can be
created and bound (compute bind point) and `RenderGraph::AddComputePass` exists,
but there is **no `cmd.Dispatch(...)`** — so a compute pass can never do anything.
Close the gap.

**Dependencies:** planset-1/08 (render graph). Independent of the other planset-2
plans. (Exercising it end-to-end needs a trivial compute shader — that's content,
not the shader-reflection work being deferred.)

## Current state

- `ComputePipeline` + `CommandBuffer::BindPipeline(Ref<ComputePipeline>)` exist
  (binds with `vk::PipelineBindPoint::eCompute`).
- `RenderGraph` has `AddComputePass`, `StorageRead`/`StorageWrite` access decls
  and the `eGeneral`/compute-stage barrier scopes.
- Missing: `vkCmdDispatch` exposure. The compute path is dead-ended.

## Design

- **`CommandBuffer::Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ)`** →
  `vkCmdDispatch`. (Optionally `DispatchIndirect` later.)
- A compute pass's `Execute` lambda binds the compute pipeline + descriptor sets
  and calls `Dispatch`; the graph already emits the storage-image barriers from
  the pass's declared `StorageRead`/`StorageWrite` accesses.
- Storage **buffers** in passes: confirm the graph/descriptors cover SSBO
  read/write hazards (image storage is covered; buffer barriers may need a small
  addition — audit during impl).
- Validate the acceptance chain from planset-1/08 that was never exercisable:
  graphics → sampled-read → compute write → graphics read, sync-validation clean.

## Acceptance

- A compute pass that writes a storage image and a later graphics pass that
  samples it runs sync-validation-clean (finishes planset-1/08 acceptance #2,
  which couldn't be tested without dispatch).
- `cmd.Dispatch` records correctly; a minimal compute shader in the sample (or a
  headless test) produces verifiable output.
