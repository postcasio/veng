#pragma once

#include <Veng/Event.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Veng.h>

#include <array>
#include <span>

namespace Veng
{
    /// @brief One pad's motor levels, each 0..1.
    struct GamepadMotors
    {
        /// @brief The low-frequency (heavy) rumble motor.
        f32 Low = 0.0f;
        /// @brief The high-frequency (light) rumble motor.
        f32 High = 0.0f;
        /// @brief The left trigger's motor, on pads that have one.
        f32 LeftTrigger = 0.0f;
        /// @brief The right trigger's motor, on pads that have one.
        f32 RightTrigger = 0.0f;

        /// @brief Compares every channel.
        bool operator==(const GamepadMotors&) const = default;
    };

    /// @brief The device-free half of the pad backend: the slot table, virtual pads and motor levels.
    ///
    /// Physical devices are named by an opaque key the device layer chooses (its connection id),
    /// which grows per connection; the table maps each to a small, stable GamepadId slot. A slot
    /// freed in a frame is not reused until the next frame's Commit, so every slot reads
    /// disconnected for at least one frame before another pad can take it, and a seat bound to it is
    /// cleared rather than silently re-pointed. A device connecting while every slot is held waits
    /// and takes the first slot that frees.
    ///
    /// Virtual pads occupy slots exactly as devices do, are edited through VirtualGamepadEvent, and
    /// report their state through Fill like any other pad. Connect and disconnect raise the ordinary
    /// GamepadConnectedEvent / GamepadDisconnectedEvent for both.
    class GamepadSlots
    {
    public:
        /// @brief The number of slots.
        static constexpr usize SlotCount = Input::MaxGamepads;

        /// @brief Records a newly connected device; it takes a slot at the next Commit.
        /// @param device  The device's connection key, unique among live devices.
        void Attach(u64 device);

        /// @brief Records a device's disconnect, freeing its slot (or dropping it if still waiting).
        /// @param device  The device's connection key.
        void Detach(u64 device);

        /// @brief Assigns waiting devices to free slots, lowest first, then ends this frame's
        ///        cooling so slots freed this frame are free from the next Commit.
        void Commit();

        /// @brief Returns the slot a device occupies, or nullopt while it waits or is unknown.
        /// @param device  The device's connection key.
        [[nodiscard]] optional<GamepadId> Find(u64 device) const;

        /// @brief Returns the device occupying a slot, or nullopt for an empty or virtual slot.
        /// @param slot  The slot.
        [[nodiscard]] optional<u64> GetDevice(GamepadId slot) const;

        /// @brief Whether a slot holds a virtual pad.
        /// @param slot  The slot.
        [[nodiscard]] bool IsVirtual(GamepadId slot) const;

        /// @brief Applies one virtual-pad edit.
        ///
        /// A connect takes its slot at once, raising the connect event, and is refused (with a
        /// warning) when the slot is held or was freed this frame. Every other edit applies only to
        /// a slot holding a virtual pad and is otherwise ignored with a warning.
        /// @param edit  The edit.
        void ApplyVirtual(const VirtualGamepadEvent& edit);

        /// @brief Writes every slot's state for this frame.
        ///
        /// An empty slot is written unconnected; a virtual slot copies its pad; a device slot is
        /// filled by @p readDevice, then, while @p focused is false, has its buttons and axes zeroed.
        /// Virtual pads are never masked: they carry no OS focus to lose.
        /// @param states      Output, one entry per slot; entries past SlotCount are untouched.
        /// @param focused     Whether device input is live (the window is focused, or background
        ///                    input is retained).
        /// @param readDevice  Fills a GamepadState from a device; it must set Connected.
        void Fill(std::span<GamepadState> states, bool focused,
                  const function<void(u64 device, GamepadState& state)>& readDevice) const;

        /// @brief Sets a slot's motor levels, clamped to 0..1; ignored for an empty slot.
        /// @param slot    The slot.
        /// @param motors  The levels.
        void SetMotors(GamepadId slot, const GamepadMotors& motors);

        /// @brief Returns a slot's motor levels; zero for an empty slot.
        /// @param slot  The slot.
        [[nodiscard]] GamepadMotors GetMotors(GamepadId slot) const;

        /// @brief Moves out the connect/disconnect events raised since the last call, in order.
        [[nodiscard]] vector<Unique<Event>> TakeEvents();

    private:
        /// @brief What holds a slot.
        enum class Occupant : u8
        {
            /// @brief Nothing.
            Empty,
            /// @brief A physical device.
            Device,
            /// @brief A virtual pad.
            Virtual,
        };

        /// @brief One slot's occupancy and the state kept for it.
        struct Slot
        {
            /// @brief What holds the slot.
            Occupant Kind = Occupant::Empty;
            /// @brief The device's connection key, for a Device slot.
            u64 Device = 0;
            /// @brief Freed this frame, so not reassigned before the next Commit.
            bool Cooling = false;
            /// @brief The virtual pad's state, for a Virtual slot.
            GamepadState Pad;
            /// @brief The motor levels last set for the slot.
            GamepadMotors Motors;
        };

        /// @brief Returns the slot record for an id, or nullptr for None or out of range.
        [[nodiscard]] Slot* SlotFor(GamepadId id);

        /// @brief Returns the slot record for an id, or nullptr for None or out of range.
        [[nodiscard]] const Slot* SlotFor(GamepadId id) const;

        /// @brief Empties a slot, starts its cooling and raises the disconnect event.
        void Free(usize index);

        /// @brief The slot table.
        std::array<Slot, SlotCount> m_Slots;
        /// @brief Devices awaiting a slot, in connection order.
        vector<u64> m_Waiting;
        /// @brief Connect/disconnect events not yet taken.
        vector<Unique<Event>> m_Events;
    };
}
