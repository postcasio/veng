#pragma once

#include <Veng/Veng.h>

/// @brief Injects the static/virtual type-identity boilerplate into an Event subclass.
#define EVENT(type)                                                                                \
    static EventType GetStaticType()                                                               \
    {                                                                                              \
        return EventType::type;                                                                    \
    }                                                                                              \
    virtual EventType GetEventType() const override                                                \
    {                                                                                              \
        return GetStaticType();                                                                    \
    }                                                                                              \
    virtual const char* GetName() const override                                                   \
    {                                                                                              \
        return #type;                                                                              \
    }

namespace Veng
{
    /// @brief Discriminator tag for every concrete Event subclass.
    enum class EventType
    {
        None = 0,
        /// @brief The window close button was pressed.
        WindowClose,
        /// @brief The window was resized.
        WindowResize,
        /// @brief The window gained focus.
        WindowFocus,
        /// @brief The window lost focus.
        WindowLostFocus,
        /// @brief The window was moved.
        WindowMoved
    };

    /// @brief Abstract base for all engine events.
    class Event
    {
    public:
        virtual ~Event() = default;

        /// @brief Set by a handler to suppress further dispatch.
        bool Handled = false;

        /// @brief Returns the runtime event type of this instance.
        virtual EventType GetEventType() const = 0;
        /// @brief Returns a human-readable name for this event type.
        virtual const char* GetName() const = 0;
        /// @brief Returns a string representation of this event.
        virtual string ToString() const { return GetName(); }
    };

    /// @brief Type-safe single-event dispatcher: calls a typed handler if the event matches T.
    class EventDispatcher
    {
    public:
        /// @brief Constructs a dispatcher bound to the given event.
        EventDispatcher(Event& event) : m_Event(event) {}

        /// @brief Calls func with the event cast to T if its type matches; returns true on match.
        /// @tparam T  Concrete event type to dispatch.
        /// @tparam F  Callable accepting T& and returning bool (whether the event was handled).
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
        /// @brief The event being dispatched.
        Event& m_Event;
    };

    /// @brief Callback signature for event listeners installed on Window.
    using EventCallback = std::function<void(Event&)>;
}
