#include <Veng/Input/RawInput.h>

#include <Veng/Input.h>
#include <Veng/Scene/Components.h>

namespace Veng
{
    RawInput::RawInput(const Input& input) : m_Input(input) {}

    bool RawInput::IsKeyDown(u32 code) const
    {
        return m_Input.IsKeyDown(static_cast<Key>(code));
    }

    bool RawInput::IsButtonDown(InputDeviceType device, u32 code) const
    {
        if (device == InputDeviceType::MouseButton)
        {
            return m_Input.IsMouseButtonDown(static_cast<MouseButton>(code));
        }

        if (device == InputDeviceType::GamepadButton)
        {
            return m_Input.IsGamepadButtonDown(DesignatedGamepad(),
                                               static_cast<GamepadButton>(code));
        }

        return false;
    }

    f32 RawInput::GetAxis(InputDeviceType device, u32 code) const
    {
        if (device == InputDeviceType::MouseAxis)
        {
            const vec2 delta = m_Input.GetMouseDelta();
            switch (code)
            {
            case MouseAxisX:
                return delta.x;
            case MouseAxisY:
                return delta.y;
            default:
                return 0.0f;
            }
        }

        if (device == InputDeviceType::GamepadAxis)
        {
            return m_Input.GetGamepadAxis(DesignatedGamepad(), static_cast<GamepadAxis>(code));
        }

        return 0.0f;
    }

    GamepadId RawInput::DesignatedGamepad() const
    {
        const std::span<const GamepadId> connected = m_Input.ConnectedGamepads();
        return connected.empty() ? GamepadId::None : connected.front();
    }

    SeatInputView::SeatInputView(const Input& input, const SeatInput& seat)
        : m_Input(input), m_UsesKeyboardMouse(seat.UsesKeyboardMouse), m_Gamepad(seat.Gamepad)
    {
    }

    bool SeatInputView::IsKeyDown(u32 code) const
    {
        if (!m_UsesKeyboardMouse)
        {
            return false;
        }
        return m_Input.IsKeyDown(static_cast<Key>(code));
    }

    bool SeatInputView::IsButtonDown(InputDeviceType device, u32 code) const
    {
        if (device == InputDeviceType::MouseButton)
        {
            return m_UsesKeyboardMouse && m_Input.IsMouseButtonDown(static_cast<MouseButton>(code));
        }

        if (device == InputDeviceType::GamepadButton)
        {
            return m_Input.IsGamepadButtonDown(m_Gamepad, static_cast<GamepadButton>(code));
        }

        return false;
    }

    f32 SeatInputView::GetAxis(InputDeviceType device, u32 code) const
    {
        if (device == InputDeviceType::MouseAxis)
        {
            if (!m_UsesKeyboardMouse)
            {
                return 0.0f;
            }
            const vec2 delta = m_Input.GetMouseDelta();
            switch (code)
            {
            case RawInput::MouseAxisX:
                return delta.x;
            case RawInput::MouseAxisY:
                return delta.y;
            default:
                return 0.0f;
            }
        }

        if (device == InputDeviceType::GamepadAxis)
        {
            return m_Input.GetGamepadAxis(m_Gamepad, static_cast<GamepadAxis>(code));
        }

        return 0.0f;
    }
}
