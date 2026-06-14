#pragma once

#include <Veng/Veng.h>

#define EVENT(type)                                                             \
    static EventType GetStaticType() { return EventType::type; }                \
    virtual EventType GetEventType() const override { return GetStaticType(); } \
    virtual const char *GetName() const override { return #type; }

namespace Veng
{
    enum class EventType
    {
        None = 0,
        WindowClose,
        WindowResize,
        WindowFocus,
        WindowLostFocus,
        WindowMoved
    };

    class Event
    {
    public:
        virtual ~Event() = default;

        bool Handled = false;

        virtual EventType GetEventType() const = 0;
        virtual const char* GetName() const = 0;
        virtual string ToString() const { return GetName(); }
    };

    class EventDispatcher
    {
    public:
        EventDispatcher(Event& event)
            : m_Event(event)
        {
        }

        // F will be deduced by the compiler
        template <typename T, typename F>
        bool Dispatch(const F& func)
        {
            if (m_Event.GetEventType() == T::GetStaticType())
            {
                m_Event.Handled |= func(static_cast<T&>(m_Event));
                return true;
            }
            return false;
        }

    private:
        Event& m_Event;
    };

    using EventCallback = std::function<void(Event&)>;
}
