#include <Veng/Renderer/TimelineSemaphore.h>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    TimelineSemaphore::Native& TimelineSemaphore::GetNative() const
    {
        return *m_Native;
    }

    TimelineSemaphore::TimelineSemaphore(Context& context, const u64 initialValue)
        : m_Context(context), m_Native(CreateUnique<Native>())
    {
        m_Native->Event = [context.GetNative().Device newSharedEvent];
        VE_ASSERT(m_Native->Event != nil, "TimelineSemaphore: failed to create its shared event");
        [m_Native->Event setSignaledValue:initialValue];
    }

    TimelineSemaphore::~TimelineSemaphore() = default;

    void TimelineSemaphore::Signal(const u64 value)
    {
        [m_Native->Event setSignaledValue:value];
    }

    void TimelineSemaphore::Wait(const u64 value) const
    {
        if ([m_Native->Event signaledValue] >= value)
        {
            return;
        }
        [m_Native->Event waitUntilSignaledValue:value timeoutMS:UINT64_MAX];
    }

    u64 TimelineSemaphore::GetValue() const
    {
        return [m_Native->Event signaledValue];
    }
}
