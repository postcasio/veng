#include <Veng/InputRouter.h>

#include <Veng/Assert.h>
#include <Veng/Input.h>
#include <Veng/Input/InputConsumer.h>
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

    InputRouter::InputRouter(Window* window, Input& input) : m_Window(window), m_Input(input) {}

    void InputRouter::RegisterConsumer(InputConsumer& consumer)
    {
        m_Consumers.push_back(&consumer);
    }

    void InputRouter::SetCursorSeat(Entity seat)
    {
        m_CursorSeat = seat;
        SyncCursorState();
    }

    FocusToken InputRouter::PushFocus(Entity seat, InputFocus focus)
    {
        const FocusToken token{.Value = m_NextToken++};
        m_Stacks[seat].push_back(FocusEntry{.Token = token, .Focus = focus});
        if (seat == m_CursorSeat)
        {
            SyncCursorState();
        }
        return token;
    }

    void InputRouter::PopFocus(FocusToken token)
    {
        VE_ASSERT(token.IsValid(), "PopFocus given an invalid (default) focus token");

        // The token names one entry in exactly one seat's stack; find and remove it. A token that
        // names no live entry is a mispaired or double pop — fatal, never a silent no-op.
        for (auto& [seat, stack] : m_Stacks)
        {
            const auto entry = std::ranges::find(stack, token, &FocusEntry::Token);
            if (entry != stack.end())
            {
                stack.erase(entry);
                if (seat == m_CursorSeat)
                {
                    SyncCursorState();
                }
                return;
            }
        }

        VE_ASSERT(false,
                  "PopFocus given a token naming no live focus entry (mispaired or double pop)");
    }

    void InputRouter::PopFocus()
    {
        const auto stack = m_Stacks.find(m_CursorSeat);
        if (stack != m_Stacks.end() && !stack->second.empty())
        {
            stack->second.pop_back();
        }
        SyncCursorState();
    }

    InputFocus InputRouter::GetFocus(Entity seat) const
    {
        const auto stack = m_Stacks.find(seat);
        if (stack == m_Stacks.end() || stack->second.empty())
        {
            return InputFocus::UI;
        }
        return stack->second.back().Focus;
    }

    void InputRouter::SyncCursorState()
    {
        const bool gameplay = CursorFocus() == InputFocus::Gameplay;

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

        // A polling consumer (the GLFW-backed overlay) reads the disabled cursor in its own frame
        // start, so swallowing mouse events is not enough to stop hover drift; signal the capture
        // so it can suspend that poll.
        for (InputConsumer* consumer : m_Consumers)
        {
            consumer->OnCursorCaptured(gameplay);
        }
    }

    void InputRouter::OfferConsumers(const Event& event)
    {
        // Offer the event to the consumers in priority order; the first to accept it stops the
        // fall-through so a later consumer never sees an already-handled event.
        for (InputConsumer* consumer : m_Consumers)
        {
            if (consumer->ForwardEvent(event))
            {
                return;
            }
        }
    }

    void InputRouter::Dispatch(Event& event)
    {
        const EventType type = event.GetEventType();

        // Window-focus loss frees a held gameplay capture on the cursor seat, so alt-tab releases
        // the cursor.
        if (type == EventType::WindowFocus)
        {
            if (!static_cast<WindowFocusEvent&>(event).IsFocused() && IsGameplayFocused())
            {
                PopFocus();
            }
            OfferConsumers(event);
            return;
        }

        // Window/system events are not owned by a focus layer; the consumers always see them.
        if (!IsInputEvent(type))
        {
            OfferConsumers(event);
            return;
        }

        if (IsGameplayFocused())
        {
            // Shift+Esc releases the cursor seat's gameplay focus and is consumed here, never
            // delivered to the game.
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

            // Exclusive: only the gameplay snapshot sees the event; the consumers are starved.
            m_Input.ApplyEvent(event);
            return;
        }

        // UI focus: the consumers see the input and the snapshot mirrors it for the editor camera.
        m_Input.ApplyEvent(event);
        OfferConsumers(event);
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

    const Renderer::Viewport* InputRouter::ResolvePointerViewport(ivec2 pointerWindowPoint,
                                                                  bool captured) const
    {
        // Captured: the cursor belongs wholly to the cursor seat, so its scope is that seat's
        // associated viewport. None when the cursor seat has no association (the default single-seat
        // path), leaving the caller to fall back to the primary world.
        if (captured)
        {
            const auto association =
                std::ranges::find(m_Associations, m_CursorSeat, &ViewportAssociation::Viewer);
            return association != m_Associations.end() ? association->Viewport : nullptr;
        }

        // Free cursor: the first associated viewport whose region contains the point, hit-tested
        // through WindowToViewport so the containment matches ResolvePointer / SelectPointerOwner.
        for (const ViewportAssociation& association : m_Associations)
        {
            if (association.Viewport->WindowToViewport(pointerWindowPoint).has_value())
            {
                return association.Viewport;
            }
        }
        return nullptr;
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
