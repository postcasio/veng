#pragma once

#include <Veng/Veng.h>
#include <Veng/Input/Actions.h>

namespace Veng
{
    class Input;

    /// @brief Adapts the engine's Veng::Input snapshot to the resolver's RawInputView.
    ///
    /// The thin bridge InputMappingSystem feeds ResolveActions: it reads only this tick's
    /// state from the borrowed Veng::Input, so the resolver derives action phase from the
    /// previous ActionState it is threaded, not from any previous-raw query. A gamepad
    /// source reads neutral (Veng::Input carries no gamepad state yet). Public so the
    /// editor's input-mapping preview reuses the identical read surface.
    ///
    /// Mouse-axis Control codes are MouseAxisX / MouseAxisY (the pointer delta components);
    /// keyboard/mouse-button codes are the raw Key / MouseButton values a binding stores.
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
        /// @param code    The button index.
        /// @return True while the button is held; false for a gamepad source (no device state yet).
        [[nodiscard]] bool IsButtonDown(InputDeviceType device, u32 code) const override;

        /// @brief The value of a device axis this tick.
        /// @param device  The device the axis belongs to.
        /// @param code    The axis index (MouseAxisX / MouseAxisY for the mouse).
        /// @return The axis value; zero for a gamepad source (no device state yet).
        [[nodiscard]] f32 GetAxis(InputDeviceType device, u32 code) const override;

    private:
        /// @brief The borrowed input snapshot read for this tick's raw state.
        const Input& m_Input;
    };
}
