#include <Veng/Input/RawInput.h>

#include <Veng/Input.h>

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
}
