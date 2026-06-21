#include <Veng/Input.h>

#include <Veng/Window.h>

namespace Veng
{
    Input::Input(Window* window) : m_Window(window) {}

    void Input::Update()
    {
        m_PreviousKeys = m_Keys;
        m_PreviousMouseButtons = m_MouseButtons;

        // No window to poll (headless): leave the zero-initialized state, so the
        // reading is the neutral all-zeros a windowed app produces with nothing pressed.
        if (m_Window == nullptr)
        {
            return;
        }

        // GLFW key codes are sparse but bounded by GLFW_KEY_LAST (348), so the
        // bitset is indexed directly by code; unused slots stay false.
        for (usize code = 0; code < MaxKeys; ++code)
        {
            m_Keys[code] = m_Window->KeyPressed(static_cast<Key>(code));
        }

        for (usize button = 0; button < MaxMouseButtons; ++button)
        {
            m_MouseButtons[button] = m_Window->MouseButtonPressed(static_cast<MouseButton>(button));
        }

        m_PreviousMousePosition = m_MousePosition;
        m_MousePosition = m_Window->GetMousePosition();

        // The opening frame has no previous position, so report no motion rather
        // than a spurious jump from the {0,0} initial value to the cursor's spawn.
        if (m_FirstUpdate)
        {
            m_PreviousMousePosition = m_MousePosition;
            m_FirstUpdate = false;
        }

        m_MouseDelta = m_MousePosition - m_PreviousMousePosition;

        m_ScrollDelta = m_Window->ConsumeScrollDelta();
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
