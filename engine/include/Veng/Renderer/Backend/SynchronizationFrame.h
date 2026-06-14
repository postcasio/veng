#pragma once

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Fence.h>
#include <Veng/Renderer/Semaphore.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    class Context;

    class SynchronizationFrame
    {
    public:
        explicit SynchronizationFrame(Context& context);

        [[nodiscard]] Semaphore& GetImageAvailableSemaphore() const { return *m_ImageAvailableSemaphore; }
        [[nodiscard]] Semaphore& GetRenderFinishedSemaphore() const { return *m_RenderFinishedSemaphore; }
        [[nodiscard]] Fence& GetInFlightFence() const { return *m_InFlightFence; }
        [[nodiscard]] Ref<CommandBuffer> GetCommandBuffer() const { return m_CommandBuffer; }

    private:
        Unique<Semaphore> m_ImageAvailableSemaphore;
        Unique<Semaphore> m_RenderFinishedSemaphore;
        Unique<Fence> m_InFlightFence;
        Ref<CommandBuffer> m_CommandBuffer;
    };
}
