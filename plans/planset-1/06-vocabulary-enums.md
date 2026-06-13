# 06 — Vocabulary enums (Vulkan + GLFW + nfd)

**Goal:** engine-owned enums/structs for every vocabulary type consumers
actually write, so public signatures stop requiring `vk::`, GLFW keycodes, or
nfd types. Backend maps to Vulkan with exhaustive switches. This is insulation
step 1 — the header split (07) depends on it.

**Dependencies:** 01 (avoid renaming churn). Feeds 07, 08, 11.

## Current state

Consumers must speak Vulkan for: formats (`ImageInfo::Format`,
`ContextInfo` output/depth formats), usage flags (`BufferInfo::Usage`,
`ImageInfo::Usage`), load/store ops + clear values
(`RenderingAttachmentInfo`, `CommandBuffer.h:27-32`), pipeline state
(`GraphicsPipelineInfo`: `PolygonMode`, `CullModeFlags`, `CompareOp`,
`PipelineColorBlendAttachmentState`), shader stages
(`PushConstantsInfo::StageFlags`, `PipelineShaderStageInfo`), sampler config
(`Sampler.h`), image layouts (`ImageBarrier::NewLayout` — handled by plan 08,
not here), bind points (`DescriptorSetBindInfo::PipelineBindPoint`).
GLFW leaks through `Window::KeyPressed(i32 key)` (`Window.h:55`) and
`GLFW_BOOL` (`Window.h:11`); nfd through the file-dialog signatures
(`Window.h:35-38`) and the `#include <nfd.h>` in `Window.h:9`.

## Design

### New header: `include/Veng/Renderer/Types.h` (no Vulkan includes)

```cpp
namespace Veng::Renderer {
    enum class Format : u8 { Undefined, R8Unorm, RGBA8Unorm, RGBA8Srgb, BGRA8Srgb,
                             R16Sfloat, RGBA16Sfloat, R32Sfloat, RG32Sfloat,
                             RGBA32Sfloat, D32Sfloat, /* extend as used */ };
    enum class ImageUsage : u8 { Sampled = 1<<0, Storage = 1<<1,
                                 ColorAttachment = 1<<2, DepthAttachment = 1<<3,
                                 TransferSrc = 1<<4, TransferDst = 1<<5 };  // flags
    enum class BufferUsage : u8 { Vertex = 1<<0, Index = 1<<1, Uniform = 1<<2,
                                  Storage = 1<<3, TransferSrc = 1<<4,
                                  TransferDst = 1<<5 };                      // flags
    enum class LoadOp : u8 { Load, Clear, DontCare };
    enum class StoreOp : u8 { Store, DontCare };
    enum class CompareOp : u8 { Never, Less, Equal, LessOrEqual, Greater,
                                NotEqual, GreaterOrEqual, Always };
    enum class CullMode : u8 { None, Front, Back };
    enum class PolygonMode : u8 { Fill, Line };
    enum class ShaderStage : u8 { Vertex = 1<<0, Fragment = 1<<1,
                                  Compute = 1<<2, All = 0xFF };              // flags
    enum class Filter : u8 { Nearest, Linear };
    enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge,
                                  ClampToBorder };
    enum class PipelineBindPoint : u8 { Graphics, Compute };

    struct ClearColor { f32 R = 0, G = 0, B = 0, A = 1; };
    struct ClearDepth { f32 Depth = 1; u32 Stencil = 0; };
    using ClearValue = std::variant<ClearColor, ClearDepth>;
}
```

- Flag enums get a small `Flags<E>` bit-ops helper (or plain `VE_ENUM_FLAGS(E)`
  macro defining `|`, `&`, `HasFlag`) in `Types.h`.
- **Populate enumerators on demand**: seed `Format` with what veng +
  `examples/` use today (grep `vk::Format::` across both), not the whole
  Vulkan format zoo. The backend switch asserts on unmapped values, so gaps
  are loud and the fix is one line per format.

### Backend mapping: `include/Veng/Renderer/Backend/TypeMapping.h` (internal)

`vk::Format ToVk(Format)`, `Format FromVk(vk::Format)` (needed for swapchain
formats), `vk::ImageUsageFlags ToVk(ImageUsage)`, etc. Exhaustive switches,
`default: VE_ASSERT(false, ...)`.

### Public struct migration

Swap field types in: `BufferInfo`, `ImageInfo`, `SamplerInfo`,
`RenderingAttachmentInfo` (+ `ClearValue`), `GraphicsPipelineInfo` /
`DynamicGraphicsPipelineInfo` / `ComputePipelineInfo`, `PushConstantsInfo`,
`DescriptorSetBindInfo`, `ShaderInfo` (stage), `ContextInfo`
(output/depth formats from plan 01), `DescriptorSetLayout` binding descriptions,
`VertexBufferLayout` attribute formats.
`vk::PipelineColorBlendAttachmentState` in `GraphicsPipelineInfo` needs a small
engine `BlendState` struct (enable, src/dst factors, op — only what the sample
app uses; common presets `BlendState::Opaque()`/`::AlphaBlend()`).

Getter return types follow (`Image::GetFormat()` → `Format`, etc.). `GetVk*()`
accessors keep returning Vulkan types — they move behind the backend boundary
in plan 07, not here.

### Input + dialogs (`Window.h`)

- `include/Veng/Input.h`: `enum class Key : u16` (GLFW-compatible values so
  the mapping is a cast + table for the odd ones), `enum class MouseButton : u8`.
  `Window::KeyPressed(Key)`, `GetMouseButton(MouseButton)` if/when added.
- File dialogs: `struct FileDialogFilter { string Name; string Extensions; };`
  replaces `nfdu8filteritem_t` in the public signature; `nfd.h` include moves
  to `Window.cpp`. (If plan 03 already made these `Result<string>`, keep that
  shape; otherwise convert now.)
- `GLFW_BOOL` macro (`Window.h:11`) moves to `Window.cpp`.

## Order of operations

1. `Types.h` + `TypeMapping.h` + tests of round-tripping (`FromVk(ToVk(x))`).
2. Migrate one resource end-to-end (`Buffer`) to validate the pattern.
3. Sweep the remaining structs in any order — each is independent.
4. `Window.h` input/dialog/nfd changes.

## Acceptance

- No `vk::`, `Vk*`, `GLFW*`, or `nfd*` token in any *Info struct or non-`GetVk`
  public method signature (checked by grep over `include/`, excluding
  `Backend/` internals like `TypeMapping.h` and `Vulkan.h`).
- `Window.h` no longer includes `nfd.h` or (transitively, for its own needs)
  GLFW — full include pruning lands in plan 07.
- The sample app compiles after a mechanical find/replace of enum spellings.
