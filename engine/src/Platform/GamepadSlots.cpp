#include "GamepadSlots.h"

#include <Veng/Log.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        /// @brief Clamps an axis value to its control's range: triggers 0..1, sticks −1..1.
        f32 ClampAxis(const GamepadAxis axis, const f32 value)
        {
            switch (axis)
            {
            case GamepadAxis::LeftTrigger:
            case GamepadAxis::RightTrigger:
                return std::clamp(value, 0.0f, 1.0f);
            default:
                return std::clamp(value, -1.0f, 1.0f);
            }
        }

        /// @brief Resets a state to unconnected in place, keeping the name's storage.
        void Clear(GamepadState& state)
        {
            state.Connected = false;
            state.Buttons.fill(false);
            state.Axes.fill(0.0f);
            state.Type = GamepadType::Unknown;
            state.Name.clear();
        }
    }

    GamepadSlots::Slot* GamepadSlots::SlotFor(const GamepadId id)
    {
        const auto index = static_cast<usize>(id);
        return index < SlotCount ? &m_Slots[index] : nullptr;
    }

    const GamepadSlots::Slot* GamepadSlots::SlotFor(const GamepadId id) const
    {
        const auto index = static_cast<usize>(id);
        return index < SlotCount ? &m_Slots[index] : nullptr;
    }

    void GamepadSlots::Attach(const u64 device)
    {
        if (Find(device) || std::ranges::find(m_Waiting, device) != m_Waiting.end())
        {
            return;
        }
        m_Waiting.push_back(device);
    }

    void GamepadSlots::Detach(const u64 device)
    {
        if (const auto waiting = std::ranges::find(m_Waiting, device); waiting != m_Waiting.end())
        {
            m_Waiting.erase(waiting);
            return;
        }
        if (const optional<GamepadId> slot = Find(device))
        {
            Free(static_cast<usize>(*slot));
        }
    }

    void GamepadSlots::Free(const usize index)
    {
        Slot& slot = m_Slots[index];
        slot.Kind = Occupant::Empty;
        slot.Device = 0;
        slot.Cooling = true;
        Clear(slot.Pad);
        slot.Motors = {};
        m_Events.push_back(CreateUnique<GamepadDisconnectedEvent>(static_cast<GamepadId>(index)));
    }

    void GamepadSlots::Commit()
    {
        usize index = 0;
        while (!m_Waiting.empty())
        {
            while (index < SlotCount &&
                   (m_Slots[index].Kind != Occupant::Empty || m_Slots[index].Cooling))
            {
                ++index;
            }
            if (index == SlotCount)
            {
                break;
            }
            m_Slots[index].Kind = Occupant::Device;
            m_Slots[index].Device = m_Waiting.front();
            m_Waiting.erase(m_Waiting.begin());
            m_Events.push_back(CreateUnique<GamepadConnectedEvent>(static_cast<GamepadId>(index)));
        }

        for (Slot& slot : m_Slots)
        {
            slot.Cooling = false;
        }
    }

    optional<GamepadId> GamepadSlots::Find(const u64 device) const
    {
        for (usize index = 0; index < SlotCount; ++index)
        {
            if (m_Slots[index].Kind == Occupant::Device && m_Slots[index].Device == device)
            {
                return static_cast<GamepadId>(index);
            }
        }
        return std::nullopt;
    }

    optional<u64> GamepadSlots::GetDevice(const GamepadId slot) const
    {
        const Slot* record = SlotFor(slot);
        if (record == nullptr || record->Kind != Occupant::Device)
        {
            return std::nullopt;
        }
        return record->Device;
    }

    bool GamepadSlots::IsVirtual(const GamepadId slot) const
    {
        const Slot* record = SlotFor(slot);
        return record != nullptr && record->Kind == Occupant::Virtual;
    }

    void GamepadSlots::ApplyVirtual(const VirtualGamepadEvent& edit)
    {
        Slot* slot = SlotFor(edit.GetSlot());
        const auto index = static_cast<usize>(edit.GetSlot());
        if (slot == nullptr)
        {
            Log::Warn("Virtual gamepad edit names slot {}, outside 0..{}", index, SlotCount - 1);
            return;
        }

        if (edit.GetOp() == VirtualGamepadOp::Connect)
        {
            if (slot->Kind != Occupant::Empty || slot->Cooling)
            {
                Log::Warn("Virtual gamepad connect refused: slot {} is {}", index,
                          slot->Kind != Occupant::Empty ? "held" : "freed this frame");
                return;
            }
            slot->Kind = Occupant::Virtual;
            Clear(slot->Pad);
            slot->Pad.Connected = true;
            slot->Pad.Type = edit.GetType();
            slot->Pad.Name = "Virtual Gamepad";
            m_Events.push_back(CreateUnique<GamepadConnectedEvent>(edit.GetSlot()));
            return;
        }

        if (slot->Kind != Occupant::Virtual)
        {
            Log::Warn("Virtual gamepad edit ignored: slot {} holds no virtual pad", index);
            return;
        }

        switch (edit.GetOp())
        {
        case VirtualGamepadOp::Connect:
            break;
        case VirtualGamepadOp::Disconnect:
            Free(index);
            break;
        case VirtualGamepadOp::Button:
        {
            const auto button = static_cast<usize>(edit.GetButton());
            if (button < slot->Pad.Buttons.size())
            {
                slot->Pad.Buttons[button] = edit.IsDown();
            }
            break;
        }
        case VirtualGamepadOp::Axis:
        {
            const GamepadAxis axis = edit.GetAxis();
            const auto axisIndex = static_cast<usize>(axis);
            // The touchpad axes are set through Touch, and the deltas are Input's to derive.
            if (axisIndex < static_cast<usize>(GamepadAxis::TouchpadX))
            {
                slot->Pad.Axes[axisIndex] = ClampAxis(axis, edit.GetValue());
            }
            break;
        }
        case VirtualGamepadOp::Touch:
        {
            const bool down = edit.IsDown();
            const vec2 position =
                down ? glm::clamp(edit.GetPosition(), vec2{0.0f}, vec2{1.0f}) : vec2{0.0f};
            slot->Pad.Buttons[static_cast<usize>(GamepadButton::TouchpadTouch)] = down;
            slot->Pad.Axes[static_cast<usize>(GamepadAxis::TouchpadX)] = position.x;
            slot->Pad.Axes[static_cast<usize>(GamepadAxis::TouchpadY)] = position.y;
            break;
        }
        }
    }

    void GamepadSlots::Fill(const std::span<GamepadState> states, const bool focused,
                            const function<void(u64 device, GamepadState& state)>& readDevice) const
    {
        const usize count = std::min(states.size(), SlotCount);
        for (usize index = 0; index < count; ++index)
        {
            const Slot& slot = m_Slots[index];
            GamepadState& state = states[index];
            switch (slot.Kind)
            {
            case Occupant::Empty:
                Clear(state);
                break;
            case Occupant::Virtual:
                state = slot.Pad;
                break;
            case Occupant::Device:
                readDevice(slot.Device, state);
                if (!focused)
                {
                    // Keyboard and mouse arrive only while the window is focused; a pad read without
                    // a window system would not, so it is held neutral to match. It stays connected,
                    // so its seat keeps it across the focus change.
                    state.Buttons.fill(false);
                    state.Axes.fill(0.0f);
                }
                break;
            }
        }
    }

    void GamepadSlots::SetMotors(const GamepadId slot, const GamepadMotors& motors)
    {
        Slot* record = SlotFor(slot);
        if (record == nullptr || record->Kind == Occupant::Empty)
        {
            return;
        }
        const auto unit = [](const f32 value) { return std::clamp(value, 0.0f, 1.0f); };
        record->Motors = GamepadMotors{.Low = unit(motors.Low),
                                       .High = unit(motors.High),
                                       .LeftTrigger = unit(motors.LeftTrigger),
                                       .RightTrigger = unit(motors.RightTrigger)};
    }

    GamepadMotors GamepadSlots::GetMotors(const GamepadId slot) const
    {
        const Slot* record = SlotFor(slot);
        return record != nullptr ? record->Motors : GamepadMotors{};
    }

    vector<Unique<Event>> GamepadSlots::TakeEvents()
    {
        return std::exchange(m_Events, {});
    }
}
