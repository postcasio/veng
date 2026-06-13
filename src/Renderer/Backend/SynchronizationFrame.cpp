#include <Veng/Renderer/Backend/SynchronizationFrame.h>

namespace Veng::Renderer
{
    SynchronizationFrame::SynchronizationFrame()
    {
        m_ImageAvailableSemaphore = Semaphore::Create("ImageAvailableSemaphore");
        m_RenderFinishedSemaphore = Semaphore::Create("RenderFinishedSemaphore");
        m_InFlightFence = Fence::Create("InFlightFence", /* signaled */ true);
        m_CommandBuffer = CommandBuffer::Create();
    }
}
