#include <Veng/Event.h>

namespace Veng
{
    template <typename T, typename F>
    bool EventDispatcher::Dispatch(const F& func)
    {
        if (m_Event.GetEventType() == T::GetStaticType())
        {
            m_Event.Handled |= func(static_cast<T&>(m_Event));
            return true;
        }
        return false;
    }
}
