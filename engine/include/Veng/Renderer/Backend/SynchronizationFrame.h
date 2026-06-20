#pragma once

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Fence.h>
#include <Veng/Renderer/Semaphore.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Owns the per-frame-in-flight synchronization primitives: the
    /// image-available semaphore, the render-finished semaphore, the in-flight
    /// fence, and the primary command buffer.
    class SynchronizationFrame
    {
    public:
        /// @brief Allocates the per-frame primitives and command buffer.
        ///
        /// The in-flight fence is created pre-signalled so the first frame does not
        /// wait indefinitely.
        explicit SynchronizationFrame(Context& context);

        /// @brief Returns the semaphore signalled by vkAcquireNextImageKHR.
        [[nodiscard]] Semaphore& GetImageAvailableSemaphore() const
        {
            return *m_ImageAvailableSemaphore;
        }
        /// @brief Returns the semaphore signalled when rendering is complete.
        [[nodiscard]] Semaphore& GetRenderFinishedSemaphore() const
        {
            return *m_RenderFinishedSemaphore;
        }
        /// @brief Returns the fence waited on before reusing this frame's resources.
        [[nodiscard]] Fence& GetInFlightFence() const { return *m_InFlightFence; }
        /// @brief Returns the primary command buffer for this frame.
        [[nodiscard]] Ref<CommandBuffer> GetCommandBuffer() const { return m_CommandBuffer; }

    private:
        Unique<Semaphore> m_ImageAvailableSemaphore;
        Unique<Semaphore> m_RenderFinishedSemaphore;
        Unique<Fence> m_InFlightFence;
        Ref<CommandBuffer> m_CommandBuffer;
    };
}
