# 07 — Public/backend header split

**Goal:** vulkan.hpp (and GLFW) out of the consumer include graph. Public
headers under `Veng/Renderer/` contain engine types only; Vulkan lives in
`Veng/Renderer/Backend/` (internal) with a single `Veng/Renderer/Native.h`
escape hatch. Also: decide the `Veng::string`-alias question and state the
thread-safety contract.

**Dependencies:** 06 (public structs must already be Vulkan-free). Barriers
(`ImageBarrier`, layouts) remain the one acknowledged hole until 08.

## Current state

- *Everything* lives in `include/Veng/Renderer/Backend/` — there is no public
  layer; "Backend" is currently just a directory name.
- `Window.h` includes `Backend/Vulkan.h` (→ vulkan.hpp + GLFW) and `nfd.h`;
  `Application.h` includes `Backend/Context.h` → every consumer TU pays the
  full vulkan.hpp parse for `#include <Veng/Application.h>`.
- `GetVk*()` accessors and Vulkan members sit in the same class definitions
  consumers use (`Context.h:45-52`, `Buffer.h:31`, `Image.h:42`, etc.).

## Design

### Approach: pimpl-free split via member hiding where cheap, not a rewrite

Full pimpl on every class is a big disruption for a young API. Instead:

1. **Public headers** `include/Veng/Renderer/{Buffer,Image,ImageView,Sampler,
   Shader,DescriptorSet,DescriptorSetLayout,PipelineLayout,GraphicsPipeline,
   DynamicGraphicsPipeline,ComputePipeline,CommandBuffer,RenderPass,
   Framebuffer,Context,...}.h` — class definitions with engine-typed API only.
   Vulkan *members* are the obstacle: hide them with one shared idiom —
   `vk::`-typed members move into a per-class `struct Native;` owned via a
   pointer (pimpl-lite: only the Vulkan handles move, logic stays in the
   class). Classes that are pure handles + name (`Buffer`, `Sampler`,
   `Shader`, ...) end up with a single `Unique<Native> m_Native` member.
   - Cost note: one heap allocation per resource object — negligible next to
     the driver allocation each one already wraps. `Image`'s hot layout array
     (`m_Layouts`) stays in the outer class.
2. **Backend headers** stay in `Backend/`: `Vulkan.h`, `TypeMapping.h`,
   `SwapChain*`, `SynchronizationFrame.h`, `Utils.h`, `DebugMarkers.h`,
   `CommandPool.h`, `DescriptorPool.h` — never included by public headers.
   Each class's `struct Native` definition lives in a backend header
   (`Backend/BufferNative.h` or one combined `Backend/Natives.h`).
3. **`include/Veng/Renderer/Native.h`** — the escape hatch: includes
   `Backend/Vulkan.h` and defines free accessor functions
   (`vk::Buffer GetVkBuffer(const Buffer&)`, `vk::Device GetVkDevice(const
   Context&)`, ...) or exposes the `Native` structs. Consumers that need raw
   handles (custom ImGui backends) include this one header knowingly; the
   `GetVk*()` member functions on public classes are removed.
   `ImGuiTexture`'s raw `VkDescriptorSet` (`ImGUITexture.h:10-21`) moves
   behind this boundary too (or is absorbed by plan 09).
4. **`Window.h` / `Application.h`** — drop `Backend/Vulkan.h` and `nfd.h`
   includes; `GLFWwindow*`/`VkSurfaceKHR` members become forward-declared
   pointer types (`struct GLFWwindow;` is a complete forward declaration; for
   the surface use the `Native` idiom or a `u64`-backed opaque handle).
   `Window::GetHandle()`/`GetSurface()` move to `Native.h`.
5. **Veng.h aliases** (Part 1 carry-over): keep the aliases — they're part of
   veng's house style and the sample app is written in it — but make `Veng.h`
   self-contained and documented as deliberate. Public signatures keep using
   them. (If the decision flips later it's a mechanical sweep; record the
   decision in the header comment.)
6. **Thread-safety contract** (2.4 carry-over): add a "Threading" section to
   the top-level header docs / README: veng v1 is single-threaded — context
   singleton, `Time`, ImGui all assume one thread. A decision, not an accident.
7. **CMake**: `Vulkan::Vulkan`, `glfw`, `GPUOpen::VulkanMemoryAllocator`, `nfd`
   move from `PUBLIC` to `PRIVATE` link visibility (`CMakeLists.txt:176-183`)
   once no public header needs their headers. `glm`, `fmt`, ImGui stay public.

### Compile-time guard

Add a CI-able check: a `tests/include_hygiene.cpp` that includes every public
header with a fake `vulkan/vulkan.hpp`-poisoning include path (or simply
`grep`-based: no `#include <vulkan`, `<GLFW`, `<nfd` outside `Backend/`),
wired as a CMake test. Cheap and keeps the boundary from regressing.

## Order of operations

1. Move `SynchronizationFrame.h`, `SwapChain*.h`, `Utils.h`, `CommandPool.h`,
   `DescriptorPool.h`, `DebugMarkers.h` out of any public include chain
   (`Context.h` currently includes several — its public face needs them only
   as forward declarations + `Unique<>` members, which requires out-of-line
   destructors).
2. `Native` extraction class by class, leaf-first (Sampler → Shader → Buffer →
   Image/ImageView → pipelines → DescriptorSet → CommandBuffer → Context).
3. `Native.h` escape hatch; delete `GetVk*` members.
4. `Window.h`/`Application.h` include pruning.
5. CMake visibility flip + hygiene check.

## Acceptance

- A consumer TU with `#include <Veng/Application.h>` compiles without
  vulkan.hpp/GLFW/nfd on the include path (the hygiene test).
- Consumer full-rebuild time measurably drops (record before/after for
  the sample app).
- `Native.h` is the only public header mentioning Vulkan, and the sample
  app's only use of it is its ImGui/texture glue (verify what remains and
  why).
