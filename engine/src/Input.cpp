#include <Veng/Input.h>

#include <Veng/InputEvents.h>
#include <Veng/Window.h>

namespace Veng
{
    Input::Input(Window* window) : m_Window(window) {}

    void Input::BeginFrame(const bool rollEdges)
    {
        // The per-frame deltas are a once-per-frame quantity, so they clear every frame regardless of
        // the edge gate. Motion a Sim tick has not consumed is held in m_Sim*Accumulator instead,
        // which BeginSimTick drains — the two cadences are independent.
        m_MouseDelta = {0, 0};
        m_ScrollDelta = {0, 0};

        // A frame that ran no Sim tick leaves the edges latched: holding the previous state keeps a
        // pressed/released edge observable until a tick-running frame consumes it. The roll below
        // advances the previous state to the current one, clearing the edge — so it runs only when
        // the previous frame consumed it.
        if (!rollEdges)
        {
            return;
        }

        // Capture previous *before* lowering any deferred-release level, so the released edge fires
        // this roll for an edge consumer even though the action layer reads level.
        m_PreviousKeys = m_Keys;
        m_PreviousMouseButtons = m_MouseButtons;

        // Apply any release deferred while its press had not yet crossed a roll (a tap that went down
        // and up entirely between two ticks): the tick just consumed saw the key/button down, so its
        // release is honored now. Then clear the pressed-since-roll gate for the window beginning now.
        for (usize code = 0; code < MaxKeys; ++code)
        {
            if (m_KeyReleaseDeferred[code])
            {
                m_Keys[code] = false;
                m_KeyReleaseDeferred[code] = false;
            }
            m_KeyPressedSinceRoll[code] = false;
        }
        for (usize index = 0; index < MaxMouseButtons; ++index)
        {
            if (m_MouseReleaseDeferred[index])
            {
                m_MouseButtons[index] = false;
                m_MouseReleaseDeferred[index] = false;
            }
            m_MousePressedSinceRoll[index] = false;
        }

        // Roll the pad button bits before this frame's poll overwrites the current state, so the
        // pressed-edge query compares this frame's poll against last frame's.
        for (usize slot = 0; slot < MaxGamepads; ++slot)
        {
            m_PreviousGamepadButtons[slot] = m_Gamepads[slot].Buttons;
        }
    }

    void Input::BeginSimTick()
    {
        m_SimMouseDelta = m_SimMouseAccumulator;
        m_SimScrollDelta = m_SimScrollAccumulator;
        m_SimMouseAccumulator = {0, 0};
        m_SimScrollAccumulator = {0, 0};
    }

    void Input::BeginGamepadSimTick()
    {
        for (usize slot = 0; slot < MaxGamepads; ++slot)
        {
            // A step that is the first to see the finger down reads no motion: whatever moved between
            // the landing and this step is the finger settling, not a drag the step should apply.
            m_SimTouchDelta[slot] = m_SimTouchDown[slot] ? m_SimTouchAccumulator[slot] : vec2{0, 0};
            m_SimTouchAccumulator[slot] = {0, 0};
            m_SimTouchDown[slot] = m_TouchDown[slot];
        }
    }

    void Input::DropSimDeltas()
    {
        m_SimMouseAccumulator = {0, 0};
        m_SimScrollAccumulator = {0, 0};
        m_SimMouseDelta = {0, 0};
        m_SimScrollDelta = {0, 0};
        m_SimTouchAccumulator.fill({0, 0});
        m_SimTouchDelta.fill({0, 0});
        m_SimTouchDown.fill(false);
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
                m_KeyPressedSinceRoll[code] = true;
                m_KeyReleaseDeferred[code] = false;
            }
            break;
        }
        case EventType::KeyReleased:
        {
            const auto code =
                static_cast<usize>(static_cast<const KeyReleasedEvent&>(event).GetKey());
            if (code < MaxKeys)
            {
                // Defer the release if the press has not yet crossed a roll, so a tap between two
                // ticks stays down until a tick observes it; otherwise release immediately.
                if (m_KeyPressedSinceRoll[code])
                {
                    m_KeyReleaseDeferred[code] = true;
                }
                else
                {
                    m_Keys[code] = false;
                }
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
                m_MousePressedSinceRoll[index] = true;
                m_MouseReleaseDeferred[index] = false;
            }
            break;
        }
        case EventType::MouseButtonReleased:
        {
            const auto index =
                static_cast<usize>(static_cast<const MouseButtonReleasedEvent&>(event).GetButton());
            if (index < MaxMouseButtons)
            {
                // Defer the release if the press has not yet crossed a roll, so a tap between two
                // ticks stays down until a tick observes it; otherwise release immediately.
                if (m_MousePressedSinceRoll[index])
                {
                    m_MouseReleaseDeferred[index] = true;
                }
                else
                {
                    m_MouseButtons[index] = false;
                }
            }
            break;
        }
        case EventType::MouseMoved:
        {
            const auto& moved = static_cast<const MouseMovedEvent&>(event);
            const vec2 position = moved.GetPosition();
            // Seed the first position with no delta, so the opening move reports no spurious
            // jump from the {0,0} initial value; later moves accumulate relative motion
            // (correct under a captured cursor's virtual coordinate). A move in a new basis seeds
            // too: across a capture switch the positions are unrelated, and their difference would
            // arrive as one jump of the whole previous capture's travel.
            if (m_HavePosition && moved.GetBasis() == m_MouseBasis)
            {
                const vec2 travel = position - m_MousePosition;
                m_MouseDelta += travel;
                m_SimMouseAccumulator += travel;
            }
            m_MousePosition = position;
            m_MouseBasis = moved.GetBasis();
            m_HavePosition = true;
            break;
        }
        case EventType::MouseScrolled:
        {
            const vec2 offset = static_cast<const MouseScrolledEvent&>(event).GetOffset();
            m_ScrollDelta += offset;
            m_SimScrollAccumulator += offset;
            break;
        }
        case EventType::KeyRepeat:
            // A repeat re-asserts a key that is already down, so it changes nothing here: the level
            // is already true and the pressed-since-roll gate already set. Folding it in as a press
            // would re-arm the edge, making WasKeyPressed fire again on a key that never went up —
            // a held key would look like a stream of discrete presses. The snapshot is level and
            // edge state only, so repetition is a consumer's concern, never the snapshot's.
            break;
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

    void Input::WithholdHeldMouseButtons()
    {
        // Input is event-driven, so a button cleared here stays up until its next press: the release
        // that arrives for it then finds it already up and changes nothing.
        m_MouseButtons.fill(false);
        m_PreviousMouseButtons.fill(false);
        m_MousePressedSinceRoll.fill(false);
        m_MouseReleaseDeferred.fill(false);
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

    vec2 Input::GetSimMouseDelta() const
    {
        return m_SimMouseDelta;
    }

    vec2 Input::GetSimScrollDelta() const
    {
        return m_SimScrollDelta;
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

    void Input::SetCursorVisible(const bool visible)
    {
        if (m_Window != nullptr)
        {
            m_Window->SetCursorVisible(visible);
        }
    }

    bool Input::IsCursorVisible() const
    {
        return m_Window == nullptr || m_Window->IsCursorVisible();
    }

    void Input::IngestGamepadStates(const std::span<const GamepadState> states)
    {
        constexpr auto TouchX = static_cast<usize>(GamepadAxis::TouchpadX);
        constexpr auto TouchY = static_cast<usize>(GamepadAxis::TouchpadY);
        constexpr auto DeltaX = static_cast<usize>(GamepadAxis::TouchpadDeltaX);
        constexpr auto DeltaY = static_cast<usize>(GamepadAxis::TouchpadDeltaY);
        constexpr auto Touch = static_cast<usize>(GamepadButton::TouchpadTouch);

        m_ConnectedGamepads.clear();
        for (usize slot = 0; slot < MaxGamepads; ++slot)
        {
            GamepadState& pad = m_Gamepads[slot];
            if (slot < states.size())
            {
                pad = states[slot];
            }
            else
            {
                pad = GamepadState{};
            }
            if (pad.Connected)
            {
                m_ConnectedGamepads.push_back(static_cast<GamepadId>(slot));
            }

            // Motion is only the travel of one finger held across two ingests: a landing, a lift or
            // a disconnect contributes none, so a finger set down elsewhere never reads as a jump.
            const bool touching = pad.Connected && pad.Buttons[Touch];
            const vec2 position{pad.Axes[TouchX], pad.Axes[TouchY]};
            const vec2 delta =
                touching && m_TouchDown[slot] ? position - m_TouchPosition[slot] : vec2{0, 0};
            pad.Axes[DeltaX] = delta.x;
            pad.Axes[DeltaY] = delta.y;
            m_SimTouchAccumulator[slot] += delta;
            m_TouchDown[slot] = touching;
            m_TouchPosition[slot] = position;
        }
    }

    const GamepadState* Input::PadFor(const GamepadId id) const
    {
        const auto slot = static_cast<usize>(id);
        if (slot >= MaxGamepads || !m_Gamepads[slot].Connected)
        {
            return nullptr;
        }
        return &m_Gamepads[slot];
    }

    bool Input::IsGamepadConnected(const GamepadId id) const
    {
        return PadFor(id) != nullptr;
    }

    bool Input::IsGamepadButtonDown(const GamepadId id, const GamepadButton button) const
    {
        const GamepadState* pad = PadFor(id);
        const auto index = static_cast<usize>(button);
        return pad != nullptr && index < pad->Buttons.size() && pad->Buttons[index];
    }

    bool Input::WasGamepadButtonPressed(const GamepadId id, const GamepadButton button) const
    {
        const GamepadState* pad = PadFor(id);
        if (pad == nullptr)
        {
            return false;
        }
        const auto index = static_cast<usize>(button);
        return index < pad->Buttons.size() && pad->Buttons[index] &&
               !m_PreviousGamepadButtons[static_cast<usize>(id)][index];
    }

    f32 Input::GetGamepadAxis(const GamepadId id, const GamepadAxis axis) const
    {
        const GamepadState* pad = PadFor(id);
        const auto index = static_cast<usize>(axis);
        return pad != nullptr && index < pad->Axes.size() ? pad->Axes[index] : 0.0f;
    }

    f32 Input::GetSimGamepadAxis(const GamepadId id, const GamepadAxis axis) const
    {
        const GamepadState* pad = PadFor(id);
        if (pad == nullptr)
        {
            return 0.0f;
        }
        switch (axis)
        {
        case GamepadAxis::TouchpadDeltaX:
            return m_SimTouchDelta[static_cast<usize>(id)].x;
        case GamepadAxis::TouchpadDeltaY:
            return m_SimTouchDelta[static_cast<usize>(id)].y;
        default:
            return static_cast<usize>(axis) < pad->Axes.size() ? pad->Axes[static_cast<usize>(axis)]
                                                               : 0.0f;
        }
    }

    GamepadType Input::GetGamepadType(const GamepadId id) const
    {
        const GamepadState* pad = PadFor(id);
        return pad != nullptr ? pad->Type : GamepadType::Unknown;
    }

    string_view Input::GetGamepadName(const GamepadId id) const
    {
        const GamepadState* pad = PadFor(id);
        return pad != nullptr ? string_view(pad->Name) : string_view();
    }

    std::span<const GamepadId> Input::ConnectedGamepads() const
    {
        return m_ConnectedGamepads;
    }
}
