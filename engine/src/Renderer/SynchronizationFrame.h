#pragma once

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Fence.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Owns one frame-in-flight slot's primitives: the in-flight fence and the primary
    /// command buffer.
    ///
    /// The presentation semaphores are not here: they belong to the backend's swap chain, since
    /// a present holds them past the point the in-flight fence bounds and not every backend
    /// signals an acquire with one.
    class VE_API SynchronizationFrame
    {
    public:
        /// @brief Allocates the slot's fence and command buffer.
        ///
        /// The in-flight fence is created pre-signalled so the first frame does not
        /// wait indefinitely.
        explicit SynchronizationFrame(Context& context);

        /// @brief Returns the fence waited on before reusing this frame's resources.
        [[nodiscard]] Fence& GetInFlightFence() const { return *m_InFlightFence; }
        /// @brief Returns the primary command buffer for this frame.
        [[nodiscard]] Ref<CommandBuffer> GetCommandBuffer() const { return m_CommandBuffer; }

    private:
        /// @brief The fence the slot's submit signals.
        Unique<Fence> m_InFlightFence;
        /// @brief The slot's primary command buffer.
        Ref<CommandBuffer> m_CommandBuffer;
    };
}
