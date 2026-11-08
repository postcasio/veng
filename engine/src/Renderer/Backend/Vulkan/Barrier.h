#pragma once

// The Vulkan backend's out-of-graph barrier helpers. The public API does not expose barriers or
// layouts — transitions are either derived by the render graph from declared passes (through
// BarrierBatch) or emitted here by the engine's own transfer/present paths (Image upload/download,
// ImGui, present). Every decision is the device-free DecideBarrier's.
#include "Vulkan.h"
#include "../../BarrierBatch.h"
#include "../../BarrierDecision.h"

#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class CommandBuffer;
    class Image;
    class Buffer;
}

namespace Veng::Renderer::Backend
{
    /// @brief Layout-only transition: derives destination stage/access from the target layout
    /// (ScopeForLayout), for out-of-graph paths: upload, download, present.
    ///
    /// Reads the source state from the image's tracked per-subresource state and updates it to
    /// reflect the transition.
    VE_API void TransitionImage(CommandBuffer& cmd, Image& image, ImageLayout newLayout,
                                u32 baseLayer = 0, u32 layerCount = 1, u32 baseMip = 0,
                                u32 mipCount = 1);

    /// @brief Explicit transition: the caller supplies the destination layout/stage/access for a
    /// declared use.
    ///
    /// The source side comes from the image's tracked state. No barrier is emitted for a
    /// read-after-read inside the tracked read scope — the scope is widened instead; a read in a
    /// stage outside it takes a layout-preserving barrier (DecideBarrier).
    VE_API void TransitionImage(CommandBuffer& cmd, Image& image, ImageLayout newLayout,
                                SyncStage dstStage, SyncAccess dstAccess, u32 baseLayer,
                                u32 layerCount, u32 baseMip, u32 mipCount);

    /// @brief Marks every subresource of an image as produced on @p producingFamily,
    /// carrying the transfer-timeline value its copy signalled.
    ///
    /// The first graphics use reads this to emit the ownership-acquire half and
    /// fold the timeline value into the frame submit. Called by the async upload
    /// worker after recording its copy.
    VE_API void MarkProducedOn(const Image& image, u32 producingFamily, u64 transferValue);

    /// @brief Emits a buffer-memory barrier over the whole buffer between two
    /// stage/access scopes.
    ///
    /// A buffer has no layout, so the barrier is purely a stage/access dependency, and buffers
    /// carry no tracked state, so both scopes are supplied by the caller.
    /// @param cmd       Command buffer the barrier is recorded into.
    /// @param buffer    The buffer to barrier.
    /// @param srcStage  Pipeline stage(s) of the producing access.
    /// @param srcAccess Access of the producing access.
    /// @param dstStage  Pipeline stage(s) of the consuming access.
    /// @param dstAccess Access of the consuming access.
    VE_API void TransitionBuffer(CommandBuffer& cmd, Buffer& buffer, SyncStage srcStage,
                                 SyncAccess srcAccess, SyncStage dstStage, SyncAccess dstAccess);

    /// @brief Records the release half of a transfer→graphics queue-family ownership
    /// transfer for the whole image (TransferDst → ShaderReadOnly).
    ///
    /// A no-op when the families match (single-queue collapse). Recorded on the
    /// transfer queue's command buffer; its acquire counterpart is emitted on first
    /// graphics use.
    VE_API void ReleaseImageToGraphicsQueue(const CommandBuffer& cmd, const Image& image,
                                            u32 transferFamily, u32 graphicsFamily);
}
