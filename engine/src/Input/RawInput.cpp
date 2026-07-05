#include <Veng/Input/RawInput.h>

#include <Veng/Input.h>
#include <Veng/InputRouter.h>
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
            case MouseScrollX:
                return m_Input.GetScrollDelta().x;
            case MouseScrollY:
                return m_Input.GetScrollDelta().y;
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

    SeatInputView::SeatInputView(const Input& input, const SeatInput& seat,
                                 const PointerRouting& pointer, Entity viewer)
        : m_Input(input), m_Pointer(pointer), m_Viewer(viewer),
          m_UsesKeyboardMouse(seat.UsesKeyboardMouse), m_Gamepad(seat.Gamepad)
    {
    }

    bool SeatInputView::OwnsPointer() const
    {
        return m_UsesKeyboardMouse && m_Pointer.OwnerThisFrame() == m_Viewer;
    }

    bool SeatInputView::IsKeyDown(u32 code) const
    {
        // Keyboard is not region-gated: one keyboard, held by whichever seat sets UsesKeyboardMouse.
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
            return OwnsPointer() && m_Input.IsMouseButtonDown(static_cast<MouseButton>(code));
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
            if (!OwnsPointer())
            {
                return 0.0f;
            }
            // Look delta stays raw pixels (translation-invariant, so sensitivity-invariant across
            // region size); position reports the pointer's region-local coordinate.
            const vec2 delta = m_Input.GetMouseDelta();
            switch (code)
            {
            case RawInput::MouseAxisX:
                return delta.x;
            case RawInput::MouseAxisY:
                return delta.y;
            case MousePositionX:
                return m_Pointer.LocalPosition.x;
            case MousePositionY:
                return m_Pointer.LocalPosition.y;
            case RawInput::MouseScrollX:
                return m_Input.GetScrollDelta().x;
            case RawInput::MouseScrollY:
                return m_Input.GetScrollDelta().y;
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
