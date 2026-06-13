# Plan 01 — Context back-reference (mechanism + internal de-global)

**Goal:** establish the de-global *mechanism* without changing any public API —
give every resource that reaches `Context::Instance()` in a destructor or member a
stored `Context&` back-reference, and route those uses through it. After this plan,
`Context::Instance()` survives **only** as the capture expression at each
construction point. This isolates the risky part (rerouting the deferred-destruction
`Retire` path) so it is verified once, alone, under validation, with zero public-API
churn — before plans 02/03 do the mechanical signature plumbing.

## Why this is its own plan

The destructor `Retire` reroute is the one genuinely load-bearing change: a resource
that retires into the wrong frame bin (or a destroyed context) is a use-after-free
that the non-deterministic smoke render can easily miss (CLAUDE.md: validation
errors don't fail tests). Separating "store and use a back-ref" from "change the
`Create` signature" means this lands and is validated on its own, and 02/03 become
pure plumbing on a proven pattern.

## What reaches the global today

Resources reaching `Context::Instance()` outside their constructor (from a grep of
`src/Renderer/Backend/`):

- **Destructor `Retire`:** `Buffer`, `Image`, `ImageView` (one branch),
  `Sampler`, `Shader`, `PipelineLayout`, `GraphicsPipeline`, `ComputePipeline`,
  `DescriptorSet`.
- **Destructor direct-destroy:** `DescriptorSetLayout`
  (`destroyDescriptorSetLayout`). (`DescriptorPool`/`CommandPool` are
  context-internal — deferred to plan 04.)
- **Member methods:** `Buffer::Upload` / `Buffer::Download` (allocator),
  `Image::Upload` (`SubmitImmediateCommands` + allocator),
  `DescriptorSet::Write` ×4 (`updateDescriptorSets`).

> The **context-internal primitives** (`CommandBuffer`, `SwapChain`, `Fence`,
> `Semaphore`, `CommandPool`, `DescriptorPool`, `SynchronizationFrame`) and the
> free `DebugMarkers::Mark*` helpers also reach the global, but they are
> constructed *by* the `Context` itself — they naturally take `*this` and are
> converted in **plan 04**, not here. This plan is scoped to the **app-facing
> resource** back-ref pattern.

## Work

1. **Add the back-reference.** To each app-facing resource above, add a
   `Context& m_Context` member, initialized in the constructor's member-init list
   from `Context::Instance()` for now:

   ```cpp
   Buffer::Buffer(const BufferInfo& info)
       : m_Context(Context::Instance()), m_Name(info.Name), /* … */ { … }
   ```

   The capture source becomes the `Create` argument in plans 02/03; only the
   initializer changes there, not the member or its uses.

2. **Route every non-constructor use through `m_Context`.** Replace
   `Context::Instance()` with `m_Context` in destructors and member methods —
   `m_Context.GetNative().Retire(...)`, `GetVkDevice(m_Context)`,
   `GetVmaAllocator(m_Context)`, `m_Context.SubmitImmediateCommands(...)`. The
   constructor body keeps using `Context::Instance()` for now (it *is* the capture
   point; collapse it to `m_Context` only where it reads cleanly).

3. **Leave `Create` signatures and the sample untouched.** No public-API change,
   no `examples/` or `tests/` edits. The sample and all three test executables
   build and run unchanged.

## Dependencies

None — foundation of the de-global arc. Must land before 02/03 (they change where
`m_Context` is captured from).

## Acceptance

- Clean build, `ctest` green, smoke binary writes a correct-sized PPM
  (1280×720 RGB ≈ 2,764,816 bytes).
- **Validation-verified:** `headless_smoke` and `compute_dispatch` run directly
  from `build-debug/` show no new `Vulkan validation` ERROR lines vs. before
  (the known storage-image gap unchanged, not widened). This is the plan whose
  reroute could corrupt the retire path, so this check is mandatory, not optional.
- `grep -rn "Context::Instance()" src/` now appears **only** inside constructors
  / `Create` bodies and the context-internal primitives deferred to plan 04 — no
  destructor or member method still reaches the global directly.

## Notes

- **Behaviour-preserving.** Same Vulkan calls, same frame bin, same ordering —
  only the *path* to the context changes (member ref vs. static accessor). Since
  `m_Context` is captured from `Instance()`, it is the same object.
- Member-init order: `m_Context` should be declared/initialized early so it is
  valid for any later member that needs it. Watch the declaration order vs.
  init-list order (compilers warn on mismatch).
- `DescriptorSet` already holds `Ref`s to its bound resources (`m_BoundResources`)
  — that is unrelated ownership, leave it. The new `m_Context` is a non-owning
  back-ref, consistent with "must not outlive the context."
