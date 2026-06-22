#include <Veng/InputRouter.h>

#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Window.h>
#include <Veng/WindowEvents.h>

namespace Veng
{
    namespace
    {
        /// @brief True for the key/mouse events that fold into the Input snapshot.
        bool IsInputEvent(EventType type)
        {
            switch (type)
            {
            case EventType::KeyPressed:
            case EventType::KeyReleased:
            case EventType::KeyTyped:
            case EventType::MouseButtonPressed:
            case EventType::MouseButtonReleased:
            case EventType::MouseMoved:
            case EventType::MouseScrolled:
            case EventType::MouseEntered:
                return true;
            default:
                return false;
            }
        }
    }

    InputRouter::InputRouter(Window* window, Input& input, ImGuiLayer* imgui)
        : m_Window(window), m_Input(input), m_ImGui(imgui)
    {
    }

    void InputRouter::PushFocus(InputFocus focus)
    {
        m_Stack.push_back(focus);
        SyncCursor();
    }

    void InputRouter::PopFocus()
    {
        if (!m_Stack.empty())
        {
            m_Stack.pop_back();
        }
        SyncCursor();
    }

    InputFocus InputRouter::GetFocus() const
    {
        return m_Stack.empty() ? InputFocus::UI : m_Stack.back();
    }

    void InputRouter::SyncCursor()
    {
        if (m_Window == nullptr)
        {
            return;
        }
        if (GetFocus() == InputFocus::Gameplay)
        {
            m_Window->CaptureMouse();
        }
        else
        {
            m_Window->ReleaseMouse();
        }
    }

    void InputRouter::Dispatch(Event& event)
    {
        const EventType type = event.GetEventType();

        // Window-focus loss frees a held gameplay capture, so alt-tab releases the cursor.
        if (type == EventType::WindowFocus)
        {
            if (!static_cast<WindowFocusEvent&>(event).IsFocused() && IsGameplayFocused())
            {
                PopFocus();
            }
            if (m_ImGui != nullptr)
            {
                m_ImGui->ForwardEvent(event);
            }
            return;
        }

        // Window/system events are not owned by a focus layer; ImGui always sees them.
        if (!IsInputEvent(type))
        {
            if (m_ImGui != nullptr)
            {
                m_ImGui->ForwardEvent(event);
            }
            return;
        }

        if (IsGameplayFocused())
        {
            // Shift+Esc releases gameplay focus and is consumed here, never delivered to the game.
            if (type == EventType::KeyPressed)
            {
                const Key key = static_cast<KeyPressedEvent&>(event).GetKey();
                const bool shift =
                    m_Input.IsKeyDown(Key::LeftShift) || m_Input.IsKeyDown(Key::RightShift);
                if (key == Key::Escape && shift)
                {
                    PopFocus();
                    return;
                }
            }

            // Exclusive: only the gameplay snapshot sees the event; ImGui is starved.
            m_Input.ApplyEvent(event);
            return;
        }

        // UI focus: ImGui consumes input and the snapshot mirrors it for the editor camera.
        m_Input.ApplyEvent(event);
        if (m_ImGui != nullptr)
        {
            m_ImGui->ForwardEvent(event);
        }
    }
}
