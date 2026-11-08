#pragma once

// The Metal backend's out-of-graph transitions, for the engine's own transfer paths (upload,
// download). Each decides through the device-free DecideBarrier against the image's tracked state
// and records the result as a Metal stage barrier; there are no layouts to change.

#include "../../BarrierBatch.h"
#include "../../BarrierDecision.h"

#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class Image;
}

namespace Veng::Renderer::Backend
{
    /// @brief Transitions a subresource range to a layout class, deriving the destination scope from
    ///        it (ScopeForLayout).
    void TransitionImage(CommandBuffer& cmd, Image& image, ImageLayout newLayout, u32 baseLayer = 0,
                         u32 layerCount = 1, u32 baseMip = 0, u32 mipCount = 1);

    /// @brief Transitions a subresource range to an explicit use.
    void TransitionImage(CommandBuffer& cmd, Image& image, ImageLayout newLayout,
                         SyncStage dstStage, SyncAccess dstAccess, u32 baseLayer, u32 layerCount,
                         u32 baseMip, u32 mipCount);

    /// @brief Orders a buffer's next access after its last.
    void TransitionBuffer(CommandBuffer& cmd, Buffer& buffer, SyncStage srcStage,
                          SyncAccess srcAccess, SyncStage dstStage, SyncAccess dstAccess);

    /// @brief Marks every subresource of an image as produced by an upload signalling
    ///        @p transferValue, which its first graphics use folds into the frame's waits.
    void MarkProducedOn(const Image& image, u32 producingFamily, u64 transferValue);
}
