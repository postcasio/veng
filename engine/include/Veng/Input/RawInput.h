#pragma once

#include <Veng/Veng.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>

namespace Veng
{
    class Input;
    struct SeatInput;

    /// @brief Adapts the engine's Veng::Input snapshot to the resolver's RawInputView.
    ///
    /// The thin bridge InputMappingSystem feeds ResolveActions: it reads only this tick's
    /// state from the borrowed Veng::Input, so the resolver derives action phase from the
    /// previous ActionState it is threaded, not from any previous-raw query. A gamepad source
    /// reads the single designated pad — the first connected GamepadId — so single-seat play is
    /// controller-drivable. Public so the editor's input-mapping preview reuses the identical
    /// read surface.
    ///
    /// Mouse-axis Control codes are MouseAxisX / MouseAxisY (the pointer delta components);
    /// keyboard/mouse-button codes are the raw Key / MouseButton values a binding stores; gamepad
    /// Control codes are the GamepadButton / GamepadAxis indices.
    class RawInput final : public RawInputView
    {
    public:
        /// @brief Control code selecting the mouse's horizontal delta as an axis source.
        static constexpr u32 MouseAxisX = 0;
        /// @brief Control code selecting the mouse's vertical delta as an axis source.
        static constexpr u32 MouseAxisY = 1;

        /// @brief Constructs the adapter over a borrowed input snapshot.
        /// @param input  The frame-coherent input service, borrowed for the resolve call.
        explicit RawInput(const Input& input);

        /// @brief Whether a keyboard key is down this tick.
        /// @param code  The Key code.
        /// @return True while the key is held.
        [[nodiscard]] bool IsKeyDown(u32 code) const override;

        /// @brief Whether a device button is down this tick.
        /// @param device  The device the button belongs to.
        /// @param code    The button index (a GamepadButton for a gamepad source).
        /// @return True while the button is held; a gamepad source reads the designated pad.
        [[nodiscard]] bool IsButtonDown(InputDeviceType device, u32 code) const override;

        /// @brief The value of a device axis this tick.
        /// @param device  The device the axis belongs to.
        /// @param code    The axis index (MouseAxisX / MouseAxisY for the mouse, a GamepadAxis for a pad).
        /// @return The axis value; a gamepad source reads the designated pad.
        [[nodiscard]] f32 GetAxis(InputDeviceType device, u32 code) const override;

    private:
        /// @brief The designated pad the gamepad arms read: the first connected slot, or None.
        [[nodiscard]] GamepadId DesignatedGamepad() const;

        /// @brief The borrowed input snapshot read for this tick's raw state.
        const Input& m_Input;
    };

    /// @brief A RawInputView over Veng::Input scoped to one seat's assigned devices.
    ///
    /// Keyboard/mouse arms report neutral unless the seat holds them (SeatInput::UsesKeyboardMouse);
    /// gamepad arms read ONLY the seat's assigned pad (SeatInput::Gamepad), so a different seat's pad
    /// or an unassigned seat reads neutral. InputMappingSystem builds one per locally-owned seat from
    /// that seat's SeatInput and resolves against it, so two seats with different assignments produce
    /// distinct PlayerInputs. ResolveActions is unchanged — it still sees a plain RawInputView; only
    /// its raw backing narrows.
    ///
    /// The mouse-axis / keyboard / mouse-button / gamepad Control codes match RawInput's mapping.
    class SeatInputView final : public RawInputView
    {
    public:
        /// @brief Constructs the seat-scoped view over a borrowed input snapshot and device assignment.
        /// @param input  The frame-coherent input service, borrowed for the resolve call.
        /// @param seat   The seat's device assignment: gates keyboard/mouse and scopes the pad.
        SeatInputView(const Input& input, const SeatInput& seat);

        /// @brief Whether a keyboard key is down this tick; neutral unless the seat holds the keyboard.
        /// @param code  The Key code.
        /// @return True while the key is held and the seat sets UsesKeyboardMouse.
        [[nodiscard]] bool IsKeyDown(u32 code) const override;

        /// @brief Whether a device button is down this tick.
        /// @param device  The device the button belongs to.
        /// @param code    The button index (a GamepadButton for a gamepad source).
        /// @return True while held; keyboard/mouse gated on UsesKeyboardMouse, gamepad scoped to the
        ///         seat's assigned pad.
        [[nodiscard]] bool IsButtonDown(InputDeviceType device, u32 code) const override;

        /// @brief The value of a device axis this tick.
        /// @param device  The device the axis belongs to.
        /// @param code    The axis index (MouseAxisX / MouseAxisY for the mouse, a GamepadAxis for a pad).
        /// @return The axis value; mouse gated on UsesKeyboardMouse, gamepad scoped to the seat's pad.
        [[nodiscard]] f32 GetAxis(InputDeviceType device, u32 code) const override;

    private:
        /// @brief The borrowed input snapshot read for this tick's raw state.
        const Input& m_Input;
        /// @brief Whether this seat holds the keyboard/mouse; gates those arms.
        bool m_UsesKeyboardMouse;
        /// @brief The pad slot this seat's gamepad arms read, or GamepadId::None.
        GamepadId m_Gamepad;
    };
}
