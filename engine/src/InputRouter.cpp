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
            case EventType::KeyRepeat:
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

    void InputRouter::SetCursorSeat(SeatRef seat)
    {
        m_CursorSeat = StackKey(seat);
        SyncCursorState();
    }

    void InputRouter::MoveCursorSeat(SeatRef seat)
    {
        const SeatRef to = StackKey(seat);
        if (to == m_CursorSeat)
        {
            return;
        }

        // Append rather than replace: an entry already on the destination has a holder that will pop
        // it by token, so it stays live beneath the carried ones, which sit on top as the user's
        // current state. The emptied source is dropped, so a seat left behind holds nothing.
        if (const auto from = m_Stacks.find(m_CursorSeat); from != m_Stacks.end())
        {
            vector<FocusEntry> carried = std::move(from->second);
            m_Stacks.erase(from);
            vector<FocusEntry>& destination = m_Stacks[to];
            destination.insert(destination.end(), carried.begin(), carried.end());
        }
        m_CursorSeat = to;
        SyncCursorState();
    }

    FocusToken InputRouter::PushFocus(SeatRef seat, InputFocus focus)
    {
        seat = StackKey(seat);
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
                const bool cursor = seat == m_CursorSeat;
                if (stack.empty())
                {
                    // An empty stack reads as UI exactly as an absent one does; dropping it keeps the
                    // map bounded by the seats holding focus rather than every seat that ever did.
                    const SeatRef key = seat;
                    m_Stacks.erase(key);
                }
                if (cursor)
                {
                    SyncCursorState();
                }
                return;
            }
        }

        // A token whose entry went with its world: the holder's one pop forgets it.
        if (const auto retired = std::ranges::find(m_RetiredTokens, token);
            retired != m_RetiredTokens.end())
        {
            m_RetiredTokens.erase(retired);
            return;
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
            if (stack->second.empty())
            {
                m_Stacks.erase(stack);
            }
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

    bool InputRouter::IsFocusTokenRetired(const FocusToken token) const
    {
        return token.IsValid() &&
               std::ranges::find(m_RetiredTokens, token) != m_RetiredTokens.end();
    }

    void InputRouter::ForgetWorld(const WorldInstanceId world)
    {
        // Stack keys are StackKey'd, so the implicit seat is filed under no world and never matches.
        const auto names = [world](const SeatRef& seat)
        { return !seat.IsImplicit() && seat.World == world; };

        bool cursorDropped = false;
        for (auto it = m_Stacks.begin(); it != m_Stacks.end();)
        {
            if (!names(it->first))
            {
                ++it;
                continue;
            }
            for (const FocusEntry& entry : it->second)
            {
                m_RetiredTokens.push_back(entry.Token);
            }
            cursorDropped |= it->first == m_CursorSeat;
            it = m_Stacks.erase(it);
        }
        std::erase_if(m_Associations, [&names](const ViewportAssociation& association)
                      { return names(association.Seat); });
        if (cursorDropped)
        {
            SyncCursorState();
        }
    }

    bool InputRouter::IsFocusTokenOn(SeatRef seat, FocusToken token) const
    {
        if (!token.IsValid())
        {
            return false;
        }
        const auto stack = m_Stacks.find(StackKey(seat));
        return stack != m_Stacks.end() &&
               std::ranges::find(stack->second, token, &FocusEntry::Token) != stack->second.end();
    }

    InputFocus InputRouter::GetFocus(SeatRef seat) const
    {
        const auto stack = m_Stacks.find(StackKey(seat));
        if (stack == m_Stacks.end() || stack->second.empty() || stack->second.back().Suspended)
        {
            return InputFocus::UI;
        }
        return stack->second.back().Focus;
    }

    FocusToken InputRouter::ReleaseGameplayFocus(SeatRef seat)
    {
        if (!IsGameplayFocused(seat))
        {
            return FocusToken{};
        }
        const FocusToken token = m_Stacks.at(StackKey(seat)).back().Token;
        PopFocus(token);
        return token;
    }

    void InputRouter::SuspendCursorGameplay()
    {
        if (!IsGameplayFocused())
        {
            return;
        }
        m_Stacks.at(m_CursorSeat).back().Suspended = true;
        SyncCursorState();
    }

    void InputRouter::ResumeSuspended()
    {
        // Every seat, not only the cursor seat: a suspended entry rides MoveCursorSeat with the rest
        // of its stack. An entry popped while suspended is simply gone, so nothing resumes it.
        bool resumed = false;
        for (auto& [seat, stack] : m_Stacks)
        {
            for (FocusEntry& entry : stack)
            {
                resumed |= entry.Suspended;
                entry.Suspended = false;
            }
        }
        if (resumed)
        {
            SyncCursorState();
        }
    }

    void InputRouter::SyncCursorState()
    {
        const bool gameplay = CursorFocus() == InputFocus::Gameplay;
        if (gameplay && !m_CursorCaptured)
        {
            m_Input.WithholdHeldMouseButtons();
        }
        m_CursorCaptured = gameplay;

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

    bool InputRouter::OfferConsumers(const Event& event)
    {
        // Offer the event to the consumers in priority order; the first to accept it stops the
        // fall-through so a later consumer never sees an already-handled event.
        for (InputConsumer* consumer : m_Consumers)
        {
            if (consumer->ForwardEvent(event))
            {
                return true;
            }
        }
        return false;
    }

    bool InputRouter::DispatchRole(const RoleEvent& event)
    {
        for (InputConsumer* consumer : m_Consumers)
        {
            if (consumer->ForwardRole(event))
            {
                return true;
            }
        }
        return false;
    }

    bool InputRouter::IsKeyClaimed(const Key key) const
    {
        return std::ranges::find(m_ClaimedKeys, key) != m_ClaimedKeys.end();
    }

    void InputRouter::Dispatch(Event& event)
    {
        const EventType type = event.GetEventType();

        // Window-focus loss suspends a held gameplay capture on the cursor seat, so alt-tab frees the
        // cursor and refocusing takes it back with no click — unless the seat holds input in the
        // background, where the capture is what keeps a driven app's focus-gated contexts resolving
        // while the operator works in another window.
        if (type == EventType::WindowFocus)
        {
            if (static_cast<WindowFocusEvent&>(event).IsFocused())
            {
                ResumeSuspended();
            }
            else if (!m_BackgroundInput)
            {
                SuspendCursorGameplay();
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

        // A key's claim lasts one press: a new press or the release drops it, whatever the focus, so
        // a claim earned under UI focus never outlives the press across a focus change.
        Key key = Key::Space;
        const bool keyEvent = type == EventType::KeyPressed || type == EventType::KeyReleased ||
                              type == EventType::KeyRepeat;
        if (keyEvent)
        {
            key = type == EventType::KeyPressed    ? static_cast<KeyPressedEvent&>(event).GetKey()
                  : type == EventType::KeyReleased ? static_cast<KeyReleasedEvent&>(event).GetKey()
                                                   : static_cast<KeyRepeatEvent&>(event).GetKey();
            if (type != EventType::KeyRepeat)
            {
                std::erase(m_ClaimedKeys, key);
            }
        }

        if (IsGameplayFocused())
        {
            // Exclusive: only the gameplay snapshot sees the event; the consumers are starved.
            m_Input.ApplyEvent(event);
            return;
        }

        // UI focus: the consumers see the input and the snapshot mirrors it for the editor camera.
        m_Input.ApplyEvent(event);
        const bool accepted = OfferConsumers(event);
        if (accepted && (type == EventType::KeyPressed || type == EventType::KeyRepeat) &&
            !IsKeyClaimed(key))
        {
            m_ClaimedKeys.push_back(key);
        }
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
        case EventType::KeyRepeat:
            m_InjectedQueue.push_back(
                InjectedEvent{.Kind = InjectedKind::KeyRepeat,
                              .KeyCode = static_cast<const KeyRepeatEvent&>(event).GetKey()});
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
        case EventType::KeyTyped:
            m_InjectedQueue.push_back(InjectedEvent{
                .Kind = InjectedKind::Text,
                .Codepoint = static_cast<const KeyTypedEvent&>(event).GetCodepoint()});
            break;
        case EventType::VirtualGamepad:
            m_InjectedQueue.push_back(
                InjectedEvent{.Kind = InjectedKind::Gamepad,
                              .Gamepad = static_cast<const VirtualGamepadEvent&>(event)});
            break;
        default:
            // Not one of the foldable input kinds an injected batch carries; ignore it rather than
            // route a non-input event through the synthetic path.
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
        case InjectedKind::KeyRepeat:
        {
            KeyRepeatEvent event(injected.KeyCode, 0, 0);
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
            // In the window's current basis, so an injected move continues the real cursor's.
            MouseMovedEvent event(injected.Vector,
                                  m_Window != nullptr ? m_Window->GetCursorBasis() : 0);
            Dispatch(event);
            break;
        }
        case InjectedKind::Scroll:
        {
            MouseScrolledEvent event(injected.Vector);
            Dispatch(event);
            break;
        }
        case InjectedKind::Text:
        {
            KeyTypedEvent event(injected.Codepoint);
            Dispatch(event);
            break;
        }
        case InjectedKind::Gamepad:
            if (m_VirtualGamepadSink && injected.Gamepad)
            {
                m_VirtualGamepadSink(*injected.Gamepad);
            }
            break;
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

        // A virtual-pad edit's level is keyed by its pad and control; the touchpad finger and the
        // connection are controls of their own, numbered past the buttons.
        const auto padControl =
            [](const VirtualGamepadEvent& edit) -> optional<std::pair<u64, bool>>
        {
            const u64 slot = static_cast<u64>(edit.GetSlot()) << 32;
            switch (edit.GetOp())
            {
            case VirtualGamepadOp::Button:
                return std::pair{slot | static_cast<u64>(edit.GetButton()), edit.IsDown()};
            case VirtualGamepadOp::Touch:
                return std::pair{slot | 0x10000u, edit.IsDown()};
            case VirtualGamepadOp::Connect:
                return std::pair{slot | 0x20000u, true};
            case VirtualGamepadOp::Disconnect:
                return std::pair{slot | 0x20000u, false};
            case VirtualGamepadOp::Axis:
                return std::nullopt;
            }
            return std::nullopt;
        };

        usize applied = 0;
        vector<Key> pressedKeys;
        vector<Key> releasedKeys;
        vector<MouseButton> pressedButtons;
        vector<MouseButton> releasedButtons;
        vector<u64> raisedPadControls;
        vector<u64> loweredPadControls;
        for (const InjectedEvent& injected : m_InjectedQueue)
        {
            const optional<std::pair<u64, bool>> padLevel =
                injected.Kind == InjectedKind::Gamepad && injected.Gamepad
                    ? padControl(*injected.Gamepad)
                    : std::nullopt;
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
            case InjectedKind::Gamepad:
                reverses =
                    padLevel && contains(padLevel->second ? loweredPadControls : raisedPadControls,
                                         padLevel->first);
                break;
            case InjectedKind::KeyRepeat:
            case InjectedKind::MouseMove:
            case InjectedKind::Scroll:
            case InjectedKind::Text:
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
            case InjectedKind::Gamepad:
                if (padLevel)
                {
                    (padLevel->second ? raisedPadControls : loweredPadControls)
                        .push_back(padLevel->first);
                }
                break;
            case InjectedKind::KeyRepeat:
            case InjectedKind::MouseMove:
            case InjectedKind::Scroll:
            case InjectedKind::Text:
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

    void InputRouter::AssociateViewportSeat(const Renderer::Viewport& viewport, SeatRef seat)
    {
        SweepDeadAssociations();

        const Renderer::ViewportId id = viewport.GetId();
        const auto existing = std::ranges::find(m_Associations, id, &ViewportAssociation::Id);
        if (existing != m_Associations.end())
        {
            existing->Seat = seat;
            return;
        }
        m_Associations.emplace_back(ViewportAssociation{.Id = id, .Seat = seat});
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
            regions.emplace_back(PointerRegionSeat{.Region = viewport->GetRegion(),
                                                   .Viewer = association.Seat.Viewer});
        }
        PointerRouting routing = SelectPointerOwner(regions, pointerWindowPoint);

        // Publish whether the UI already owns the pointer, so gameplay can decline it. The owning
        // viewport is the one whose region won the hit-test above, resolved the same way.
        if (routing.Owner != Entity::Null)
        {
            if (const Renderer::Viewport* const viewport =
                    ResolvePointerViewport(pointerWindowPoint, false);
                viewport != nullptr)
            {
                routing.OverUi = viewport->IsPointerOverDocument(pointerWindowPoint);
            }
        }
        return routing;
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
                std::ranges::find(m_Associations, m_CursorSeat, [](const ViewportAssociation& entry)
                                  { return StackKey(entry.Seat); });
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
