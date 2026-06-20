#include <Veng/Renderer/Backend/SynchronizationFrame.h>

namespace Veng::Renderer
{
    /// @brief Allocates the per-frame synchronization primitives and command buffer.
    ///
    /// The in-flight fence is created pre-signalled so the first frame does not wait indefinitely.
    SynchronizationFrame::SynchronizationFrame(Context& context)
    {
        m_ImageAvailableSemaphore = Semaphore::Create(context, "ImageAvailableSemaphore");
        m_RenderFinishedSemaphore = Semaphore::Create(context, "RenderFinishedSemaphore");
        m_InFlightFence = Fence::Create(context, "InFlightFence", /* signaled */ true);
        m_CommandBuffer = CommandBuffer::Create(context);
    }
}
