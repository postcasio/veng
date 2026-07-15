#include <Veng/InputRouter.h>

#include <Veng/Assert.h>
#include <Veng/Input.h>
#include <Veng/Input/InputConsumer.h>
#include <Veng/InputEvents.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Window.h>
#include <Veng/WindowEvents.h>

#include <algorithm>
#include <ranges>

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

    InputRouter::InputRouter(Window* window, Input& input,
                             const Renderer::ViewportRegistry& viewportRegistry)
        : m_Window(window), m_Input(input), m_ViewportRegistry(viewportRegistry)
    {
    }

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

    bool InputRouter::IsFocusTokenLive(const FocusToken token) const
    {
        if (!token.IsValid())
        {
            return false;
        }
        return std::ranges::any_of(m_Stacks,
                                   [token](const auto& entry)
                                   {
                                       return std::ranges::any_of(entry.second,
                                                                  [token](const FocusEntry& focus)
                                                                  { return focus.Token == token; });
                                   });
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

    void InputRouter::PostInjectedEvent(const Event& event)
    {
        switch (event.GetEventType())
        {
        case EventType::KeyPressed:
            m_InjectedQueue.push_back(
                InjectedEvent{.Kind = InjectedKind::KeyDown,
                              .KeyCode = static_cast<const KeyPressedEvent&>(event).GetKey()});
            break;
        case EventType::KeyReleased:
            m_InjectedQueue.push_back(
                InjectedEvent{.Kind = InjectedKind::KeyUp,
                              .KeyCode = static_cast<const KeyReleasedEvent&>(event).GetKey()});
            break;
        case EventType::MouseButtonPressed:
            m_InjectedQueue.push_back(InjectedEvent{
                .Kind = InjectedKind::MouseDown,
                .Button = static_cast<const MouseButtonPressedEvent&>(event).GetButton()});
            break;
        case EventType::MouseButtonReleased:
            m_InjectedQueue.push_back(InjectedEvent{
                .Kind = InjectedKind::MouseUp,
                .Button = static_cast<const MouseButtonReleasedEvent&>(event).GetButton()});
            break;
        case EventType::MouseMoved:
            m_InjectedQueue.push_back(
                InjectedEvent{.Kind = InjectedKind::MouseMove,
                              .Vector = static_cast<const MouseMovedEvent&>(event).GetPosition()});
            break;
        case EventType::MouseScrolled:
            m_InjectedQueue.push_back(
                InjectedEvent{.Kind = InjectedKind::Scroll,
                              .Vector = static_cast<const MouseScrolledEvent&>(event).GetOffset()});
            break;
        default:
            // Not one of the six foldable input kinds an injected batch carries; ignore it rather
            // than route a non-input event through the synthetic path.
            break;
        }
    }

    void InputRouter::ApplyInjected(const InjectedEvent& injected)
    {
        switch (injected.Kind)
        {
        case InjectedKind::KeyDown:
        {
            KeyPressedEvent event(injected.KeyCode, 0, 0);
            Dispatch(event);
            break;
        }
        case InjectedKind::KeyUp:
        {
            KeyReleasedEvent event(injected.KeyCode, 0, 0);
            Dispatch(event);
            break;
        }
        case InjectedKind::MouseDown:
        {
            MouseButtonPressedEvent event(injected.Button, 0);
            Dispatch(event);
            break;
        }
        case InjectedKind::MouseUp:
        {
            MouseButtonReleasedEvent event(injected.Button, 0);
            Dispatch(event);
            break;
        }
        case InjectedKind::MouseMove:
        {
            MouseMovedEvent event(injected.Vector);
            Dispatch(event);
            break;
        }
        case InjectedKind::Scroll:
        {
            MouseScrolledEvent event(injected.Vector);
            Dispatch(event);
            break;
        }
        }
    }

    void InputRouter::DrainInjectedEvents()
    {
        // Apply one paced segment: events in order, stopping before any event that would *reverse* a
        // control's level already set this segment — a release of a control pressed here, or a press
        // of one released here. Deferring the reversing event to the next frame guarantees each
        // press-then-release (or release-then-press) straddles a frame, so a control is observed at
        // each level for at least one tick and two rapid taps of one control stay distinct rather than
        // folding into a single held span. Distinct controls (a chord) still apply together, and a
        // move/scroll never reverses a level, so it never stops the segment.
        const auto contains = [](const auto& values, const auto value)
        { return std::ranges::find(values, value) != values.end(); };

        usize applied = 0;
        vector<Key> pressedKeys;
        vector<Key> releasedKeys;
        vector<MouseButton> pressedButtons;
        vector<MouseButton> releasedButtons;
        for (const InjectedEvent& injected : m_InjectedQueue)
        {
            bool reverses = false;
            switch (injected.Kind)
            {
            case InjectedKind::KeyDown:
                reverses = contains(releasedKeys, injected.KeyCode);
                break;
            case InjectedKind::KeyUp:
                reverses = contains(pressedKeys, injected.KeyCode);
                break;
            case InjectedKind::MouseDown:
                reverses = contains(releasedButtons, injected.Button);
                break;
            case InjectedKind::MouseUp:
                reverses = contains(pressedButtons, injected.Button);
                break;
            case InjectedKind::MouseMove:
            case InjectedKind::Scroll:
                break;
            }
            if (reverses)
            {
                break;
            }

            ApplyInjected(injected);
            ++applied;

            switch (injected.Kind)
            {
            case InjectedKind::KeyDown:
                pressedKeys.push_back(injected.KeyCode);
                break;
            case InjectedKind::KeyUp:
                releasedKeys.push_back(injected.KeyCode);
                break;
            case InjectedKind::MouseDown:
                pressedButtons.push_back(injected.Button);
                break;
            case InjectedKind::MouseUp:
                releasedButtons.push_back(injected.Button);
                break;
            case InjectedKind::MouseMove:
            case InjectedKind::Scroll:
                break;
            }
        }

        m_InjectedQueue.erase(m_InjectedQueue.begin(),
                              m_InjectedQueue.begin() + static_cast<std::ptrdiff_t>(applied));
    }

    void InputRouter::SweepDeadAssociations()
    {
        const auto dead = std::ranges::remove_if(
            m_Associations, [this](const ViewportAssociation& association)
            { return m_ViewportRegistry.Resolve(association.Id) == nullptr; });
        m_Associations.erase(dead.begin(), dead.end());
    }

    void InputRouter::AssociateViewportSeat(const Renderer::Viewport& viewport, Entity viewer)
    {
        SweepDeadAssociations();

        const Renderer::ViewportId id = viewport.GetId();
        const auto existing = std::ranges::find(m_Associations, id, &ViewportAssociation::Id);
        if (existing != m_Associations.end())
        {
            existing->Viewer = viewer;
            return;
        }
        m_Associations.emplace_back(ViewportAssociation{.Id = id, .Viewer = viewer});
    }

    void InputRouter::ClearViewportSeat(const Renderer::Viewport& viewport)
    {
        ClearViewportSeat(viewport.GetId());
    }

    void InputRouter::ClearViewportSeat(Renderer::ViewportId id)
    {
        SweepDeadAssociations();

        const auto removed = std::ranges::remove(m_Associations, id, &ViewportAssociation::Id);
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

        // Free cursor: resolve each association's id to its live viewport, gather that viewport's
        // current region in association order, and select the last containing the point (topmost —
        // see SelectPointerOwner). An id that no longer resolves is skipped — never mutated away
        // here, so this stays const.
        vector<PointerRegionSeat> regions;
        regions.reserve(m_Associations.size());
        for (const ViewportAssociation& association : m_Associations)
        {
            const Renderer::Viewport* viewport = m_ViewportRegistry.Resolve(association.Id);
            if (viewport == nullptr)
            {
                continue;
            }
            regions.emplace_back(
                PointerRegionSeat{.Region = viewport->GetRegion(), .Viewer = association.Viewer});
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
            return association != m_Associations.end() ? m_ViewportRegistry.Resolve(association->Id)
                                                       : nullptr;
        }

        // Free cursor: the last associated viewport whose region contains the point — associations
        // track registration (render) order, so the latest containing one is the topmost the user
        // sees (an overlay's viewport over a full-window world). Hit-tested through
        // WindowToViewport so the containment matches ResolvePointer / SelectPointerOwner. An id
        // that no longer resolves is skipped.
        for (const ViewportAssociation& association : m_Associations | std::views::reverse)
        {
            const Renderer::Viewport* viewport = m_ViewportRegistry.Resolve(association.Id);
            if (viewport != nullptr && viewport->WindowToViewport(pointerWindowPoint).has_value())
            {
                return viewport;
            }
        }
        return nullptr;
    }

    PointerRouting SelectPointerOwner(std::span<const PointerRegionSeat> regions,
                                      ivec2 pointerWindowPoint)
    {
        // Later entries win any overlap: the regions arrive in association (registration = render)
        // order, so the last containing region is the topmost viewport the user sees under the
        // cursor — an overlay's full-window viewport over an associated world viewport routes to
        // the overlay's seat, not the covered one's.
        for (const PointerRegionSeat& entry : regions | std::views::reverse)
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
