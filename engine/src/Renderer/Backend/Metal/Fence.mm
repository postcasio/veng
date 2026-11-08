#include <Veng/Renderer/Fence.h>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    Fence::Native& Fence::GetNative() const
    {
        return *m_Native;
    }

    Fence::Fence(Context& context, const string& name, const bool signaled)
        : m_Context(context), m_Name(name), m_Native(CreateUnique<Native>())
    {
        m_Native->Event = [context.GetNative().Device newSharedEvent];
        VE_ASSERT(m_Native->Event != nil, "Fence '{}': failed to create its shared event", name);
        [m_Native->Event setLabel:Metal::ToNSString(name)];
        // The event starts at 0, so a target of 0 reads signalled and of 1 unsignalled until a
        // submission signals it.
        m_Native->Target = signaled ? 0 : 1;
    }

    Fence::~Fence() = default;

    void Fence::Wait() const
    {
        if ([m_Native->Event signaledValue] >= m_Native->Target)
        {
            return;
        }
        [m_Native->Event waitUntilSignaledValue:m_Native->Target timeoutMS:UINT64_MAX];
    }

    void Fence::Reset() const
    {
        // A fence is reset once its last signal is complete, so the next target is one past it.
        m_Native->Target = [m_Native->Event signaledValue] + 1;
    }

    bool Fence::IsSignaled() const
    {
        return [m_Native->Event signaledValue] >= m_Native->Target;
    }
}
