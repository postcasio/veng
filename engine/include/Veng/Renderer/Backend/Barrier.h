#pragma once

// Internal image-barrier helpers. The public API does not expose barriers or
// layouts — transitions are either derived by the render
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

    // Mark every subresource of an image as produced on producingFamily, carrying
    // the transfer-timeline value its copy signalled. The first graphics use reads
    // this to emit the ownership-acquire half and fold the timeline value into the
    // frame submit. Set by the async upload worker after recording its copy.
    void MarkProducedOn(Image& image, u32 producingFamily, u64 transferValue);

    // Record the release half of a transfer->graphics queue-family ownership
    // transfer for the whole image (TransferDst -> ShaderReadOnly). A no-op when
    // the families match (the single-queue collapse). Recorded on the transfer
    // queue's command buffer; its acquire counterpart lands on first graphics use.
    void ReleaseImageToGraphicsQueue(CommandBuffer& cmd, Image& image,
                                     u32 transferFamily, u32 graphicsFamily);
}
