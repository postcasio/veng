#include <Veng/UI/GamepadPanel.h>

#include <Veng/Application.h>
#include <Veng/Input.h>
#include <Veng/Reflection/EnumName.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Widgets.h>

#include <fmt/format.h>

#include "../Platform/GamepadBackend.h"

namespace Veng::UI
{
    namespace
    {
        /// @brief Draws one pad's controls as a table of the raw value beside the shaped one.
        void ControlTable(const Input& input, const GamepadId slot)
        {
            const auto table = UI::Table("controls", 3);
            if (!table)
            {
                return;
            }
            UI::TableSetupColumn("Control");
            UI::TableSetupColumn("Raw");
            UI::TableSetupColumn("Shaped");
            UI::TableHeadersRow();

            for (const EnumEntry& button : VengReflect<GamepadButton>::Enumerators())
            {
                UI::TableNextRow();
                UI::TableNextColumn();
                UI::Text(button.Name);
                UI::TableNextColumn();
                const bool down =
                    input.IsGamepadButtonDown(slot, static_cast<GamepadButton>(button.Value));
                if (down)
                {
                    UI::Text("down");
                }
                else
                {
                    UI::TextDisabled("-");
                }
            }

            for (const EnumEntry& entry : VengReflect<GamepadAxis>::Enumerators())
            {
                const auto axis = static_cast<GamepadAxis>(entry.Value);
                UI::TableNextRow();
                UI::TableNextColumn();
                UI::Text(entry.Name);
                UI::TableNextColumn();
                UI::Text(fmt::format("{:+.3f}", input.GetRawGamepadAxis(slot, axis)));
                UI::TableNextColumn();
                UI::Text(fmt::format("{:+.3f}", input.GetGamepadAxis(slot, axis)));
            }
        }

        /// @brief Draws one pad's deadzone sliders, applying a changed zone to the pad at once.
        void DeadzoneSliders(Input& input, const GamepadId slot)
        {
            GamepadDeadzones zones = input.GetGamepadDeadzones(slot);
            bool changed = UI::Slider("Stick deadzone", zones.Stick, SliderOptions{});
            changed |= UI::Slider("Trigger deadzone", zones.Trigger, SliderOptions{});
            if (changed)
            {
                input.SetGamepadDeadzones(slot, zones.Stick, zones.Trigger);
            }
        }

        /// @brief Draws one pad's motor sliders, writing a changed level straight to the backend.
        void MotorSliders(GamepadBackend& backend, const GamepadId slot)
        {
            GamepadMotors motors = backend.GetMotors(slot);
            bool changed = false;
            if (backend.HasRumble(slot))
            {
                changed |= UI::Slider("Low motor", motors.Low, SliderOptions{});
                changed |= UI::Slider("High motor", motors.High, SliderOptions{});
            }
            if (backend.HasTriggerMotors(slot))
            {
                changed |= UI::Slider("Left trigger motor", motors.LeftTrigger, SliderOptions{});
                changed |= UI::Slider("Right trigger motor", motors.RightTrigger, SliderOptions{});
            }
            if (backend.GetMotors(slot) != GamepadMotors{} && UI::Button("Stop motors"))
            {
                motors = {};
                changed = true;
            }
            if (changed)
            {
                backend.SetMotors(slot, motors);
            }
        }
    }

    void GamepadPanel(Application& app)
    {
        GamepadBackend* backend = app.GetGamepadBackend();
        if (backend == nullptr)
        {
            UI::TextDisabled("No gamepad backend (headless).");
            return;
        }

        Input& input = app.GetInput();
        const std::span<const GamepadId> connected = input.ConnectedGamepads();
        if (connected.empty())
        {
            UI::TextDisabled("No gamepads connected.");
            return;
        }

        const vector<EnumEntry> types = VengReflect<GamepadType>::Enumerators();
        for (const GamepadId slot : connected)
        {
            const auto index = static_cast<u32>(slot);
            const auto id = UI::PushId(fmt::format("pad{}", index));
            const string type = EnumeratorName(types, static_cast<i64>(input.GetGamepadType(slot)));
            const auto header = UI::CollapsingHeader(
                fmt::format("Slot {}: {} ({})", index, input.GetGamepadName(slot), type),
                TreeFlags::DefaultOpen);
            if (!header)
            {
                continue;
            }

            if (input.IsGamepadButtonDown(slot, GamepadButton::TouchpadTouch))
            {
                UI::Text(
                    fmt::format("Touchpad finger at ({:.3f}, {:.3f}), moved ({:+.4f}, {:+.4f})",
                                input.GetGamepadAxis(slot, GamepadAxis::TouchpadX),
                                input.GetGamepadAxis(slot, GamepadAxis::TouchpadY),
                                input.GetGamepadAxis(slot, GamepadAxis::TouchpadDeltaX),
                                input.GetGamepadAxis(slot, GamepadAxis::TouchpadDeltaY)));
            }
            else
            {
                UI::TextDisabled("Touchpad: no finger");
            }

            MotorSliders(*backend, slot);
            DeadzoneSliders(input, slot);
            ControlTable(input, slot);
        }
    }
}
