#include <Veng/Input.h>

#include <Veng/InputEvents.h>
#include <Veng/Window.h>

namespace Veng
{
    Input::Input(Window* window) : m_Window(window) {}

    void Input::BeginFrame()
    {
        m_PreviousKeys = m_Keys;
        m_PreviousMouseButtons = m_MouseButtons;

        // Deltas are per-frame: the router accumulates this frame's move/scroll events
        // into them via ApplyEvent after this roll.
        m_MouseDelta = {0, 0};
        m_ScrollDelta = {0, 0};
    }

    void Input::ApplyEvent(const Event& event)
    {
        switch (event.GetEventType())
        {
        case EventType::KeyPressed:
        {
            const auto code =
                static_cast<usize>(static_cast<const KeyPressedEvent&>(event).GetKey());
            if (code < MaxKeys)
            {
                m_Keys[code] = true;
            }
            break;
        }
        case EventType::KeyReleased:
        {
            const auto code =
                static_cast<usize>(static_cast<const KeyReleasedEvent&>(event).GetKey());
            if (code < MaxKeys)
            {
                m_Keys[code] = false;
            }
            break;
        }
        case EventType::MouseButtonPressed:
        {
            const auto index =
                static_cast<usize>(static_cast<const MouseButtonPressedEvent&>(event).GetButton());
            if (index < MaxMouseButtons)
            {
                m_MouseButtons[index] = true;
            }
            break;
        }
        case EventType::MouseButtonReleased:
        {
            const auto index =
                static_cast<usize>(static_cast<const MouseButtonReleasedEvent&>(event).GetButton());
            if (index < MaxMouseButtons)
            {
                m_MouseButtons[index] = false;
            }
            break;
        }
        case EventType::MouseMoved:
        {
            const vec2 position = static_cast<const MouseMovedEvent&>(event).GetPosition();
            // Seed the first position with no delta, so the opening move reports no spurious
            // jump from the {0,0} initial value; later moves accumulate relative motion
            // (correct under a captured cursor's virtual coordinate).
            if (m_HavePosition)
            {
                m_MouseDelta += position - m_MousePosition;
            }
            m_MousePosition = position;
            m_HavePosition = true;
            break;
        }
        case EventType::MouseScrolled:
        {
            m_ScrollDelta += static_cast<const MouseScrolledEvent&>(event).GetOffset();
            break;
        }
        default:
            break;
        }
    }

    bool Input::IsKeyDown(const Key key) const
    {
        const auto code = static_cast<usize>(key);
        return code < MaxKeys && m_Keys[code];
    }

    bool Input::WasKeyPressed(const Key key) const
    {
        const auto code = static_cast<usize>(key);
        return code < MaxKeys && m_Keys[code] && !m_PreviousKeys[code];
    }

    bool Input::WasKeyReleased(const Key key) const
    {
        const auto code = static_cast<usize>(key);
        return code < MaxKeys && !m_Keys[code] && m_PreviousKeys[code];
    }

    bool Input::IsMouseButtonDown(const MouseButton button) const
    {
        const auto index = static_cast<usize>(button);
        return index < MaxMouseButtons && m_MouseButtons[index];
    }

    bool Input::WasMouseButtonPressed(const MouseButton button) const
    {
        const auto index = static_cast<usize>(button);
        return index < MaxMouseButtons && m_MouseButtons[index] && !m_PreviousMouseButtons[index];
    }

    bool Input::WasMouseButtonReleased(const MouseButton button) const
    {
        const auto index = static_cast<usize>(button);
        return index < MaxMouseButtons && !m_MouseButtons[index] && m_PreviousMouseButtons[index];
    }

    vec2 Input::GetMousePosition() const
    {
        return m_MousePosition;
    }

    vec2 Input::GetMouseDelta() const
    {
        return m_MouseDelta;
    }

    vec2 Input::GetScrollDelta() const
    {
        return m_ScrollDelta;
    }

    void Input::SetMouseCaptured(const bool captured)
    {
        if (m_Window == nullptr)
        {
            return;
        }

        if (captured)
        {
            m_Window->CaptureMouse();
        }
        else
        {
            m_Window->ReleaseMouse();
        }
    }

    bool Input::IsMouseCaptured() const
    {
        return m_Window != nullptr && m_Window->IsMouseCaptured();
    }
}
