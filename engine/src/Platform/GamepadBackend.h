#pragma once

#include <Veng/Event.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/Veng.h>

#include <span>

#include "GamepadSlots.h"

namespace Veng
{
    /// @brief The engine's gamepad device layer: physical pads through SDL's gamepad subsystem,
    ///        merged with virtual pads into the slot table everything above reads.
    ///
    /// GLFW keeps the window, keyboard and mouse; this owns only pads. It brings SDL up with the
    /// gamepad subsystem alone and with SDL's signal handlers suppressed, so the process keeps its
    /// default SIGINT/SIGTERM behaviour and a launcher still ends on Ctrl+C or a kill by PID. On
    /// Windows, pad detection and raw input run on SDL's own thread, independent of the window's
    /// message pump, with the Windows.Gaming.Input backend off (XInput and raw input cover Xbox pads,
    /// SDL's HIDAPI drivers the rest).
    ///
    /// The Application creates it only with a window: headless and dedicated hosts have no pads.
    /// Failing to bring SDL up is not fatal — the backend then reports no physical pads, and virtual
    /// pads still work.
    ///
    /// It also owns the device rumble primitive, SetMotors. That is engine-internal on purpose:
    /// applications play rumble through the engine's haptics, which is the primitive's one caller,
    /// so a pad's motors have exactly one writer.
    class GamepadBackend
    {
    public:
        /// @brief Brings SDL's gamepad subsystem up.
        GamepadBackend();

        /// @brief Closes every pad and shuts SDL down.
        ~GamepadBackend();

        /// @brief Not copyable: it owns the SDL subsystem.
        GamepadBackend(const GamepadBackend&) = delete;

        /// @brief Not copyable: it owns the SDL subsystem.
        GamepadBackend& operator=(const GamepadBackend&) = delete;

        /// @brief Polls the pads for this frame and writes every slot's state.
        ///
        /// Updates SDL's pads, drains its event queue (taking connects and disconnects from it and
        /// discarding everything else, so nothing accumulates), assigns newly connected pads to
        /// slots, fills @p states, then sends each physical pad's changed or due motor levels.
        /// @param states   Output, one entry per slot. Kept by the caller across frames so a pad's
        ///                 name reuses its storage.
        /// @param focused  Whether physical pads are live: false holds them neutral (see
        ///                 GamepadSlots::Fill).
        void Update(std::span<GamepadState> states, bool focused);

        /// @brief Moves out the connect/disconnect events raised since the last call, in order.
        [[nodiscard]] vector<Unique<Event>> TakeEvents();

        /// @brief Applies one virtual-pad edit (see GamepadSlots::ApplyVirtual).
        /// @param edit  The edit.
        void ApplyVirtual(const VirtualGamepadEvent& edit);

        /// @brief Sets a pad's motor levels, each clamped to 0..1.
        ///
        /// The levels hold until set again. Update sends a physical pad its levels only when one
        /// changed, or, while any is non-zero, again before the previous send's short duration
        /// lapses — so a stalled frame loop lets the pad fall silent rather than buzz on. Trigger
        /// levels reach only pads that have trigger motors. A virtual pad records the levels and
        /// drives nothing.
        /// @param slot    The pad's slot; an empty slot is ignored.
        /// @param motors  The levels.
        void SetMotors(GamepadId slot, const GamepadMotors& motors);

        /// @brief Returns a pad's motor levels as last set; zero for an empty slot.
        /// @param slot  The pad's slot.
        [[nodiscard]] GamepadMotors GetMotors(GamepadId slot) const;

        /// @brief Whether the pad in a slot has rumble motors in its grips.
        /// @param slot  The pad's slot.
        [[nodiscard]] bool HasRumble(GamepadId slot) const;

        /// @brief Whether the pad in a slot has motors in its triggers.
        /// @param slot  The pad's slot.
        [[nodiscard]] bool HasTriggerMotors(GamepadId slot) const;

    private:
        /// @brief The SDL handles and per-device send state, defined in the implementation.
        struct Native;

        /// @brief Sends each physical pad its motor levels where they changed or are due a renewal.
        void FlushMotors();

        /// @brief The SDL handles and per-device send state.
        Unique<Native> m_Native;
        /// @brief The slot table, virtual pads and motor levels.
        GamepadSlots m_Slots;
    };
}
