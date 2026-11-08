#pragma once

// The Metal backend's recording primitives below the public CommandBuffer calls, for the backend's
// own transfer paths (staged uploads and downloads, barriers between them). Defined in
// CommandBuffer.mm.

#include "Natives.h"

namespace Veng::Renderer::Metal::Recording
{
    /// @brief The command buffer's compute encoder, opened if none is.
    ///
    /// The first encoder of a recording orders itself after every earlier submission on the queue;
    /// an open encoder of another kind is ended first.
    /// @param cmd The command buffer.
    /// @return The open compute encoder.
    id<MTL4ComputeCommandEncoder> ComputeEncoder(const CommandBuffer& cmd);

    /// @brief Ends the open encoder, if any.
    /// @param cmd The command buffer.
    void EndEncoder(const CommandBuffer& cmd);

    /// @brief Orders every later encoder's @p before stages after every earlier @p after stage.
    ///
    /// Recorded on the open encoder (or a new compute encoder) as a producer barrier, which then
    /// ends, so the work it orders lands in later encoders.
    /// @param cmd    The command buffer.
    /// @param after  The stages waited on.
    /// @param before The stages that wait.
    void Barrier(const CommandBuffer& cmd, MetalStage after, MetalStage before);

    /// @brief Copies a byte range between two buffers.
    /// @param cmd               The command buffer.
    /// @param source            The source buffer.
    /// @param sourceOffset      The source offset.
    /// @param destination       The destination buffer.
    /// @param destinationOffset The destination offset.
    /// @param size              The bytes to copy.
    void CopyBuffer(const CommandBuffer& cmd, const Buffer& source, u64 sourceOffset,
                    const Buffer& destination, u64 destinationOffset, u64 size);
}
