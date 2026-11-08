#include "SynchronizationFrame.h"

namespace Veng::Renderer
{
    SynchronizationFrame::SynchronizationFrame(Context& context)
    {
        m_InFlightFence = Fence::Create(context, "InFlightFence", /* signaled */ true);
        m_CommandBuffer = CommandBuffer::Create(context);
    }
}
