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

    /// @brief Opens a render encoder for @p pass, ending an open compute encoder first.
    ///
    /// Ordered like ComputeEncoder: after every earlier submission when it is the recording's first
    /// encoder of its stages, and after what a recorded barrier waits on.
    /// @param cmd  The command buffer.
    /// @param pass The render pass.
    /// @return The open render encoder; the caller ends it.
    id<MTL4RenderCommandEncoder> RenderEncoder(const CommandBuffer& cmd,
                                               MTL4RenderPassDescriptor* pass);

    /// @brief Applies the recorded barriers a newly opened encoder must wait on.
    /// @param native        The command buffer's state.
    /// @param encoder       The encoder just opened.
    /// @param encoderStages The stages the encoder runs.
    void OrderEncoder(CommandBuffer::Native& native, id<MTL4CommandEncoder> encoder,
                      MetalStage encoderStages);

    /// @brief Ends the open encoder, if any.
    /// @param cmd The command buffer.
    void EndEncoder(const CommandBuffer& cmd);

    /// @brief Orders every later encoder's @p before stages after every earlier @p after stage.
    ///
    /// Ends the open compute encoder; each encoder opened after it applies the barrier as a
    /// consumer barrier on the stages it runs.
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
