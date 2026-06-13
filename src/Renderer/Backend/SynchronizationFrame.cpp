#include <Veng/Renderer/Backend/SynchronizationFrame.h>

namespace Veng::Renderer
{
    SynchronizationFrame::SynchronizationFrame(Context& context)
    {
        m_ImageAvailableSemaphore = Semaphore::Create(context, "ImageAvailableSemaphore");
        m_RenderFinishedSemaphore = Semaphore::Create(context, "RenderFinishedSemaphore");
        m_InFlightFence = Fence::Create(context, "InFlightFence", /* signaled */ true);
        m_CommandBuffer = CommandBuffer::Create(context);
    }
}
