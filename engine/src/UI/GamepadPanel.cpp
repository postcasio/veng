#include <Veng/UI/GamepadPanel.h>

#include <Veng/Application.h>
#include <Veng/Haptics/Haptics.h>
#include <Veng/Input.h>
#include <Veng/Reflection/EnumName.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Widgets.h>

#include <fmt/format.h>

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

        /// @brief Draws one pad's mixed rumble, its live instances and a play button per clip.
        void RumbleRows(Haptics::HapticsEngine& haptics, const GamepadId slot,
                        const std::span<const GamepadPanelClip> clips)
        {
            const Haptics::RumbleChannels output = haptics.GetOutput(slot);
            UI::ProgressBar(output.LowFrequency, {-1.0f, 0.0f},
                            fmt::format("Low {:.2f}", output.LowFrequency));
            UI::ProgressBar(output.HighFrequency, {-1.0f, 0.0f},
                            fmt::format("High {:.2f}", output.HighFrequency));
            UI::ProgressBar(output.LeftTrigger, {-1.0f, 0.0f},
                            fmt::format("Left trigger {:.2f}", output.LeftTrigger));
            UI::ProgressBar(output.RightTrigger, {-1.0f, 0.0f},
                            fmt::format("Right trigger {:.2f}", output.RightTrigger));

            bool any = false;
            for (const Haptics::RumbleInstanceInfo& instance : haptics.GetAllInstances())
            {
                if (instance.Gamepad != slot)
                {
                    continue;
                }
                any = true;
                UI::Text(fmt::format(
                    "0x{:016X}  {:.2f}/{:.2f}s  x{:.2f}{}{}{}", instance.Clip.Value, instance.Time,
                    instance.Duration, instance.Intensity * instance.Fade,
                    instance.Loop ? "  loop" : "", instance.Stopping ? "  stopping" : "",
                    instance.Paused ? "  paused" : ""));
            }
            if (!any)
            {
                UI::TextDisabled("Nothing playing.");
            }

            for (const GamepadPanelClip& clip : clips)
            {
                if (UI::Button(fmt::format("Play {}", clip.Name)))
                {
                    haptics.Play(Haptics::RumbleTarget::ForGamepad(slot), clip.Clip);
                }
                UI::SameLine();
            }
            if (UI::Button("Stop"))
            {
                haptics.StopAll(Haptics::RumbleTarget::ForGamepad(slot));
            }
        }
    }

    void GamepadPanel(Application& app, const std::span<const GamepadPanelClip> clips)
    {
        Haptics::HapticsEngine& haptics = app.GetHaptics();
        UI::Text(fmt::format("Master intensity {:.2f}{}", haptics.GetMasterIntensity(),
                             haptics.IsOutputSuspended() ? " (suspended: window unfocused)" : ""));

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

            RumbleRows(haptics, slot, clips);
            DeadzoneSliders(input, slot);
            ControlTable(input, slot);
        }
    }
}
