#pragma once

// Internal image-barrier helpers. The public API no longer exposes barriers or
// layouts at all (plan 07/08) — transitions are either derived by the render
// graph from declared passes (RenderGraph) or emitted here by the engine's own
// transfer/present paths (Image upload/download, ImGui, present).
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class CommandBuffer;
    class Image;
}

namespace Veng::Renderer::Backend
{
    // Layout-only transition: destination stage/access are derived from the
    // target layout (the legacy behaviour, used by the out-of-graph paths). Reads
    // the source state from the image's tracked per-subresource state and updates
    // it to reflect the transition.
    void TransitionImage(CommandBuffer& cmd, Image& image, ImageLayout newLayout,
                         u32 baseLayer = 0, u32 layerCount = 1,
                         u32 baseMip = 0, u32 mipCount = 1);

    // Explicit transition used by the render graph: the caller supplies the
    // destination layout/stage/access for a declared use. The source side comes
    // from the image's tracked state. No barrier is emitted for a read-after-read
    // that needs none (the tracked read scope is widened instead).
    void TransitionImage(CommandBuffer& cmd, Image& image,
                         vk::ImageLayout newLayout,
                         vk::PipelineStageFlags dstStage, vk::AccessFlags dstAccess,
                         u32 baseLayer, u32 layerCount, u32 baseMip, u32 mipCount);
}
