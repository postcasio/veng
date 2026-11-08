#include <Veng/Renderer/Semaphore.h>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    Semaphore::Native& Semaphore::GetNative() const
    {
        return *m_Native;
    }

    Semaphore::Semaphore(Context& context, const string& name)
        : m_Context(context), m_Name(name), m_Native(CreateUnique<Native>())
    {
        m_Native->Event = [context.GetNative().Device newSharedEvent];
        VE_ASSERT(m_Native->Event != nil, "Semaphore '{}': failed to create its shared event",
                  name);
        [m_Native->Event setLabel:Metal::ToNSString(name)];
    }

    Semaphore::~Semaphore() = default;
}
