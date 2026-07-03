#include <Veng/InputRouter.h>

#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Window.h>
#include <Veng/WindowEvents.h>

#include <algorithm>

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
        SyncFocusState();
    }

    void InputRouter::PopFocus()
    {
        if (!m_Stack.empty())
        {
            m_Stack.pop_back();
        }
        SyncFocusState();
    }

    InputFocus InputRouter::GetFocus() const
    {
        return m_Stack.empty() ? InputFocus::UI : m_Stack.back();
    }

    void InputRouter::SyncFocusState()
    {
        const bool gameplay = GetFocus() == InputFocus::Gameplay;

        if (m_Window != nullptr)
        {
            if (gameplay)
            {
                m_Window->CaptureMouse();
            }
            else
            {
                m_Window->ReleaseMouse();
            }
        }

        // The GLFW backend polls the disabled cursor in NewFrame, so swallowing mouse events
        // is not enough to stop hover drift; disable ImGui's mouse handling while captured.
        if (m_ImGui != nullptr)
        {
            m_ImGui->SetMouseInputEnabled(!gameplay);
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

    void InputRouter::AssociateViewportSeat(const Renderer::Viewport& viewport, Entity viewer)
    {
        const auto existing =
            std::ranges::find(m_Associations, &viewport, &ViewportAssociation::Viewport);
        if (existing != m_Associations.end())
        {
            existing->Viewer = viewer;
            return;
        }
        m_Associations.emplace_back(ViewportAssociation{.Viewport = &viewport, .Viewer = viewer});
    }

    void InputRouter::ClearViewportSeat(const Renderer::Viewport& viewport)
    {
        const auto removed =
            std::ranges::remove(m_Associations, &viewport, &ViewportAssociation::Viewport);
        m_Associations.erase(removed.begin(), removed.end());
    }

    PointerRouting InputRouter::ResolvePointer(ivec2 pointerWindowPoint, bool captured,
                                               Entity captureOwner) const
    {
        // Captured: the OS cursor is hidden + locked and look reads raw delta, so "which quadrant"
        // is undefined. The pointer belongs wholly to the single keyboard/mouse seat; skip the
        // hit-test. LocalPosition stays zero — the captured seat reads delta, not position.
        if (captured)
        {
            return PointerRouting{.Owner = captureOwner, .LocalPosition = {}};
        }

        // Free cursor: gather each association's live region (a viewport owns its current region)
        // and select the first containing the point.
        vector<PointerRegionSeat> regions;
        regions.reserve(m_Associations.size());
        for (const ViewportAssociation& association : m_Associations)
        {
            regions.emplace_back(PointerRegionSeat{.Region = association.Viewport->GetRegion(),
                                                   .Viewer = association.Viewer});
        }
        return SelectPointerOwner(regions, pointerWindowPoint);
    }

    PointerRouting SelectPointerOwner(std::span<const PointerRegionSeat> regions,
                                      ivec2 pointerWindowPoint)
    {
        for (const PointerRegionSeat& entry : regions)
        {
            const Renderer::ViewportRegion& region = entry.Region;
            if (region.Extent.x == 0 || region.Extent.y == 0)
            {
                continue;
            }

            // Containment + normalized [0,1] remap, matching Viewport::WindowToViewport exactly.
            const ivec2 local = pointerWindowPoint - region.Offset;
            const ivec2 extent = ivec2(region.Extent);
            if (local.x < 0 || local.y < 0 || local.x >= extent.x || local.y >= extent.y)
            {
                continue;
            }

            const vec2 normalized(static_cast<f32>(local.x) / static_cast<f32>(extent.x),
                                  static_cast<f32>(local.y) / static_cast<f32>(extent.y));
            return PointerRouting{
                .Owner = entry.Viewer,
                .LocalPosition = normalized * vec2(region.Extent),
            };
        }

        return PointerRouting{};
    }
}
