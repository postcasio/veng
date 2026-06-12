# veng API recommendations

Notes from the 2026-06 deep clean. None of these are bugs — the current API works —
but they're worth considering before more consumers build against veng, since each
one gets more expensive to change later.

Part 1 is small, mostly mechanical fixes. Part 2 is the bigger design directions
(ownership, resource tracking, descriptor sets, API insulation) that deserve a
deliberate decision each.

---

## Part 1 — Targeted fixes

### Correctness-adjacent

**`Upload` should take `std::span<const u8>`.**
`Buffer::Upload(std::span<u8>)` and `Image::Upload(std::span<u8>)` only read the
data but demand mutable spans, so callers can't upload from const storage without
a `const_cast`.

**`PipelineShaderStageInfo::Module` is a raw `Shader*`.**
Everything else in the API passes `Ref<T>`, and pipeline creation additionally
relies on the `Shader` outliving the call because `pName` points into the shader's
entry-point string. Take `Ref<Shader>` (or `const Shader&`) to make the lifetime
requirement structural instead of a comment.

**`DescriptorSet` assumes dense binding numbers.**
`m_BoundResources` is sized by binding *count* but indexed by binding *number*
(`m_BoundResources[binding++]`). A layout with sparse bindings (0, 2, 5) writes out
of bounds. (Superseded by the descriptor-set redesign in Part 2, but worth an
assert in the meantime.)

**`UpdateDescriptorSet` silently ignores unsupported descriptor types.**
The switch handles `eCombinedImageSampler` and `eUniformBuffer`; anything else hits
`default: break` and the write is dropped without a sound. Storage buffers/images
will "bind nothing". At minimum this should assert until the redesign lands.

**`WindowInfo::Extent` is `vec2` (float).**
Window dimensions are integer pixels; the float type invites fractional extents and
silently truncates. Change to `uvec2` (matches `ApplicationInfo::InternalRenderExtent`).
Breaks ApolloSim's `.Extent = vec2{1280, 720}` — a one-line fix there.

### Lifecycle

**Window creation/ownership is inverted.**
`Context::Initialize(const ContextInfo&)` *creates* the `Window`, returns ownership
to `Application`, and keeps a non-owning `Window*` behind. Either `Application`
creates the window and lends it to the context, or the context owns it outright and
`Application::GetWindow()` delegates.

**No engine-driven teardown.**
`Application::Run` ends with `WaitIdle()` + `OnDispose()` but never calls
`Context::DisposeResources()`, `Context::Dispose()`, or `Window::Dispose()` —
device, instance, surface, and GLFW window all leak at exit. Teardown order is
subtle (sync frames → ImGui → device → instance → window/glfwTerminate), so the
engine should own it rather than each consumer.

### Consistency

- `GetName()` returns `string` by value on `Buffer`, `Image`, `DescriptorSet`,
  `RenderPass`, `Sampler`, pipelines, but `const string&` on `Shader`,
  `DescriptorPool`, `Framebuffer`. Standardize on `const string&`.
- `ImGUITexture` / `CreateImGUITexture` vs `RenderImGui` / `GetImGuiImage` —
  pick `ImGui` (matches upstream) and rename the `ImGUI` variants.
- `Veng.h` aliases `string`, `vector`, `map`, `optional`, … into the `Veng`
  namespace and they appear in public signatures. Consider keeping the aliases
  internal and using `std::` names publicly.
- `CommandBuffer::Create(vk::CommandBufferLevel level = {})` — `{}` happens to be
  `ePrimary`; spell the default explicitly.
- `Context::GetOutputFormat()` / `GetDepthFormat()` return hardcoded formats and
  aren't `const`. Make them `const`; consider sourcing from `ContextInfo`.
- `SwapChain::OnInvalidated` stores a single callback — a second registration
  silently replaces the first.
- `DescriptorSet::UpdateDescriptorSet(...)` → `DescriptorSet::Update(...)`.

### Build / packaging

- Pin the `master`-tagged FetchContent deps (VulkanMemoryAllocator, nfd, stb,
  imnodes) to release tags for reproducible builds.
- `target_link_directories(veng PUBLIC /opt/homebrew/lib)` bakes a Homebrew path
  into the exported interface; prefer `find_library`/imported targets.

---

## Part 2 — Design directions

### 2.1 Ownership (`Ref`/`Unique`) and frame-resource tracking

Today three mechanisms overlap:

1. `Create()` factories split between `Unique` (Shader, Fence, Semaphore, pools,
   VertexBufferLayout) and `Ref` (Buffer, Image, ImageView, pipelines,
   DescriptorSet) without a stated rule.
2. `CommandBuffer::m_BoundResources` — every `Bind*`/`BeginRendering` pushes a
   `Ref<void>` keep-alive, cleared on `Reset()`.
3. `SynchronizationFrame::SubmitResource` / `Command::SubmitResource(s)` — a
   *manual* keep-alive the consumer must remember to call, cleared at
   `BeginFrame`.

Problems: the manual path is a use-after-free trap (forget `SubmitResource` →
GPU reads freed memory, no warning); the two keep-alive lists make it unclear
which one is load-bearing; and `Ref` is doing double duty as both "shared
ownership" and "GPU still needs this", which is why almost everything ended up
`Ref` whether or not it's actually shared.

**Recommended direction: deferred destruction queue.** Decouple C++ lifetime from
GPU lifetime so ownership types can be chosen on pure ownership grounds:

- Resource destructors don't call `vkDestroy*`/`vmaDestroy*` directly; they push
  the raw handle (+ allocation) onto a per-frame retire list in the context,
  tagged with the current frame index.
- The context drains a frame's retire list after that frame's fence has been
  waited on (i.e., at the top of `AcquireNextFrame`).
- `SubmitResource`, `Command::SubmitResource(s)`, and the `Ref<void>` lists in
  `CommandBuffer`/`SynchronizationFrame` all become unnecessary and can be
  deleted. Dropping your last reference mid-frame is now always safe.

Then the ownership rule becomes simple and statable:

- **`Unique`** — single owner, not referenced by GPU work after the owner dies:
  per-frame sync primitives, pools, layouts owned by a renderer module.
- **`Ref`** — genuinely shared between systems (an `ImageView` referenced by both
  a material and a post-process pass). Sharing for *convenience* is fine too, but
  it's no longer required for correctness.

An intermediate step, if the retire queue feels like too much at once: delete the
manual `SubmitResource` path and make `CommandBuffer` tracking the only mechanism
— everything that touches a frame goes through the command buffer anyway, so
automatic tracking can be complete.

### 2.2 Descriptor set updates

`DescriptorSetWriteInfo` with a `variant<vector<ImageInfo>, vector<BufferInfo>>`
is verbose at the call site, repeats information the layout already knows
(descriptor type per binding), and only supports two descriptor types.

Layered fix, each step useful on its own:

1. **Infer the type from the layout.** `DescriptorSet` holds its
   `DescriptorSetLayout`; a write to binding N can look up the descriptor type
   and validate the payload kind matches. Removes `Type` from the write struct
   and turns "unsupported type" from silent no-op into an error.
2. **Typed writer methods instead of the variant struct:**

   ```cpp
   set->Write(0, imageView, sampler);          // sampled image
   set->Write(1, buffer);                      // whole buffer
   set->Write(1, buffer, offset, range);
   set->WriteArray(2, span<Ref<ImageView>>);   // bindless-style arrays
   ```

   or a batcher if grouping matters for perf:
   `DescriptorWriter(set).Write(0, view, sampler).Write(1, buf).Commit();`
3. **Longer term — bind groups (WebGPU model).** Immutable `BindGroup` created
   from `{layout, entries...}` in one shot, no post-hoc updates. Maps cleanly to
   Vulkan, kills update-ordering bugs, and pairs naturally with shader
   reflection (below). Mutable update-after-bind sets stay as a special case for
   bindless tables.

### 2.3 Insulating consumers from Vulkan (and GLFW)

Long-term goal: no `vk::`/`Vk*`/`GLFW*` in any public signature. Today consumers
must speak Vulkan for formats, usage flags, load/store ops, clear values, shader
stages, sampler config, image layouts (via `ImageBarrier`), and `ImGUITexture`
even exposes a raw `VkDescriptorSet`. `Window.h` drags in `Vulkan.h`, GLFW, and
nfd for every consumer TU — which also means every consumer pays vulkan.hpp's
(very large) compile cost regardless of what they include.

Suggested path, incremental:

1. **Engine enums for the vocabulary types** consumers actually use: `Format`,
   `ImageUsage`, `LoadOp`/`StoreOp`, `CompareOp`, `CullMode`, `PolygonMode`,
   `ShaderStage`, `Filter`, `AddressMode`, plus `ClearColor`/`ClearDepth` structs.
   Map to `vk::` in the backend with exhaustive switches.
2. **Split public headers from backend headers.** `Veng/Renderer/*.h` (public,
   engine types only) vs `Veng/Renderer/Backend/*.h` (includes vulkan.hpp,
   internal). `GetVk*()` accessors move behind the backend boundary; a single
   `Veng/Renderer/Native.h` escape hatch can exist for consumers that genuinely
   need raw handles (e.g., custom ImGui backends).
3. **Make image layouts an internal concern.** `ImageBarrier{.NewLayout = vk::...}`
   exposes the most implementation-detail-y concept in Vulkan. Replace with
   intent-based transitions (`cmd.PrepareForSampling(image)`,
   `cmd.PrepareForColorAttachment(image)`) — or eliminate manual barriers
   entirely via a render graph (2.4).
4. Same treatment for GLFW: engine `Key`/`MouseButton` enums so `KeyPressed`
   doesn't take raw GLFW keycodes, and `nfd.h` out of `Window.h` (forward-declare
   or wrap the filter-item type).

Payoffs even if a second backend never happens: consumer build times (vulkan.hpp
out of the include graph), a much smaller API to document, and freedom to change
backend decisions (sync2, dynamic rendering everywhere, descriptor buffers)
without touching consumers.

### 2.4 What else — related directions worth considering

**Render graph / automatic barriers.** `Utils::GetAccessMask(layout)` derives
src/dst access purely from layouts, which is lossy (e.g., it can't know a compute
write happened) and is manual at every call site. Declaring passes with their
reads/writes and letting the engine schedule barriers fixes correctness and
ergonomics at once — and it's what finally makes layouts/barriers disappear from
the public API (2.3). Even a minimal linear graph (no reordering, just barrier
derivation) pays for itself.

**Shader reflection.** `DescriptorSetLayout`/`PipelineLayout`/`VertexBufferLayout`
hand-duplicate information already present in the SPIR-V. Run spirv-reflect (tiny
dependency) at shader load: derive set layouts, push-constant ranges, and vertex
inputs; let consumers write `material->Set("u_Albedo", texture)` by name. Pairs
with bind groups (2.2.3) and removes a whole class of layout-mismatch bugs.

**Typed buffers.** `Buffer` is raw bytes + `vk::BufferUsageFlags`. Thin wrappers —
`VertexBuffer<V>`, `IndexBuffer`, `UniformBuffer<T>` with `Upload(const T&)` /
`Upload(span<const V>)` — remove usage flags from the public API (2.3) and catch
stride/type mismatches at compile time.

**Error handling policy.** Everything throws raw `std::runtime_error` (via
`VE_ASSERT`/`VK_ASSERT`). Fine as a policy, but make it deliberate: a `Veng::Error`
exception type (so consumers can catch engine failures distinctly), and decide
which failures are recoverable (file dialogs, shader compile in a hot-reload
future) vs fatal (device loss, OOM).

**Logging as a library citizen.** `Log` prints to stdout unconditionally. A
library shouldn't own a process's stdout: add `Log::SetSink(callback)` + minimum
level, default sink keeps current behavior. ApolloSim will want logs in its own
console window eventually anyway.

**ImGui as an optional module.** The context unconditionally creates ImGui
render passes/framebuffers, and fonts live in `ContextInfo`. Extract an
`ImGuiLayer` (own init/begin/render/shutdown, own info struct with fonts + theme)
so headless/non-UI consumers don't pay for it, and so `Veng/Vendor/ImGui.h` stops
exposing `imgui_internal.h`/`imnodes_internal.h` to everyone by default.

**Headless mode.** A `Context` that can initialize without a window/swapchain
(off-screen target only) enables CI testing of renderer code and batch tools.
Falls out naturally from fixing window ownership (Part 1) and the singleton.

**Thread-safety contract.** Everything is single-thread-assumed (singleton,
static `Time`, ImGui). That's a fine v1 contract — state it explicitly in the
docs so it's a decision, not an accident.
