# 01 — Part 1 targeted fixes

**Goal:** land all the small, mostly mechanical Part 1 fixes in one pass.
Everything here is shippable independently of the Part 2 redesigns.

**Dependencies:** none. Do this first.

## Correctness-adjacent

### Const-correct `Upload`
- `include/Veng/Renderer/Backend/Buffer.h:28` — `Upload(std::span<u8>)` →
  `Upload(std::span<const u8>)`.
- `include/Veng/Renderer/Backend/Image.h:75` — same.
- `include/Veng/Renderer/Backend/Shader.h:18` — `ShaderBinaryInfo::Binary` is
  `std::span<u8>`; make it `std::span<const u8>` for the same reason.
- Update the matching `.cpp` definitions; callers compile unchanged (non-const
  spans convert to const spans).

### `PipelineShaderStageInfo::Module` lifetime
- `include/Veng/Renderer/Backend/PipelineLayout.h:8` — `PipelineShaderStageInfo`
  holds a raw `Shader*` and pipeline creation reads `pName` out of the shader's
  entry-point string.
- Change `Module` to `const Shader&` (pipelines don't retain the shader after
  creation, so a reference states exactly the requirement: alive for the call).
  Note `Shader::Create` returns `Unique<Shader>`, so `Ref<Shader>` would force an
  ownership change; reference is the lighter fix.
- Touches `GraphicsPipeline.cpp`, `DynamicGraphicsPipeline.cpp`,
  `ComputePipeline.cpp` (struct consumers).

### Interim descriptor-set asserts (deleted again by plan 05)
- `src/Renderer/Backend/DescriptorSet.cpp` —
  - assert that the layout's binding numbers are dense (`0..count-1`) before
    sizing/indexing `m_BoundResources`, or index by binding number with a
    sparse-safe container;
  - in `UpdateDescriptorSet`, replace `default: break` with a failed assert
    naming the unsupported `vk::DescriptorType`.

### `WindowInfo::Extent` → `uvec2`
- `include/Veng/Window.h:22` — `vec2 Extent` → `uvec2 Extent`.
- `src/Window.cpp` — remove any float→int conversions that existed to cope.
- Breaks consumers' `.Extent = vec2{1280, 720}` (one-line fix at call sites).

## Consistency

- **`GetName()` returns `const string&` everywhere.** By-value offenders:
  `Buffer.h:33`, `Image.h:41`, `ImageView.h:38`, `DescriptorSet.h:70`,
  `DescriptorSetLayout.h:30`, `RenderPass.h:25`, `Sampler.h:40`,
  `GraphicsPipeline.h:40`, `DynamicGraphicsPipeline.h:40`, plus any others a
  grep for `string GetName() const` turns up. (`Event.h` returns `const char*`
  by design — leave it.)
- **`ImGUI` → `ImGui` naming.** Rename `ImGUITexture` → `ImGuiTexture`
  (header `ImGUITexture.h` → `ImGuiTexture.h`, source likewise),
  `Context::CreateImGUITexture` → `CreateImGuiTexture`,
  `Context::DestroyImGUITexture` → `DestroyImGuiTexture`. Update
  `CMakeLists.txt` source list and all includes.
- **Explicit command-buffer level default.**
  `include/Veng/Renderer/Backend/CommandBuffer.h:68` —
  `Create(vk::CommandBufferLevel level = {})` →
  `Create(vk::CommandBufferLevel level = vk::CommandBufferLevel::ePrimary)`.
- **`Context::GetOutputFormat()`/`GetDepthFormat()`**
  (`include/Veng/Renderer/Backend/Context.h:68-69`) — make `const`; add
  `OutputFormat`/`DepthFormat` fields to `ContextInfo` with the current values
  as defaults, store them in members, return those.
- **`SwapChain::OnInvalidated`**
  (`include/Veng/Renderer/Backend/SwapChain.h:54`) — single `std::function`
  silently replaced on re-registration. Change to
  `vector<std::function<void()>>` + `AddInvalidationCallback`, invoking all in
  `SwapChain.cpp:129-131`. (Alternative: assert on double-registration; vector
  is barely more code.)
- **`DescriptorSet::UpdateDescriptorSet` → `Update`** — *skip here*; plan 05
  replaces the method entirely. Doing the rename now churns callers twice.
- **`Veng::string`/`vector`/... aliases in public signatures** — deferred to
  plan 07, where public headers are reworked wholesale.

## Build / packaging

- `CMakeLists.txt` — pin `master`-tagged FetchContent deps to release tags:
  - VulkanMemoryAllocator (`:53`) → latest release (v3.x)
  - nativefiledialog-extended (`:62`) → latest release
  - stb (`:81`) → a specific commit hash (stb has no release tags)
  - imnodes (`:105`) → latest release or commit hash
- `CMakeLists.txt:189` — replace
  `target_link_directories(veng PUBLIC /opt/homebrew/lib)` with proper
  discovery: the deps that needed it should come from `find_package`/
  `find_library` imported targets carrying their own paths. Identify which
  library actually required this (likely glfw or Vulkan loader via Homebrew)
  and fix that one properly.

## Acceptance

- veng builds clean; the sample app builds against it with only the documented
  one-line `WindowInfo::Extent` fix and the `ImGui` renames.
- A descriptor-set layout with sparse bindings or a storage-buffer write now
  fails loudly instead of corrupting memory / silently no-op'ing.
- Fresh clone configures with pinned dep versions; no `/opt/homebrew/lib` in
  the exported interface (`veng-targets.cmake`).
