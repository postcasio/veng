#pragma once

#include <Veng/Veng.h>
#include <Veng/Event.h>
#include <Veng/Input.h>

namespace Veng
{
    /// @brief Fired when a key transitions to the pressed state.
    ///
    /// Carries the engine Key vocab plus the GLFW-native scancode and modifier bits, so a
    /// consumer reads it in engine terms while the ImGui sink forwards the raw values to the
    /// GLFW backend with no remapping.
    class KeyPressedEvent final : public Event
    {
    public:
        /// @brief Constructs the event.
        /// @param key       The key that was pressed.
        /// @param scancode  Platform-specific scancode (GLFW-native), for the ImGui sink.
        /// @param mods      GLFW modifier-key bitfield, for the ImGui sink.
        KeyPressedEvent(Key key, i32 scancode, i32 mods)
            : m_Key(key), m_Scancode(scancode), m_Mods(mods)
        {
        }

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(KeyPressed);

        /// @brief Returns the key that was pressed.
        [[nodiscard]] Key GetKey() const { return m_Key; }
        /// @brief Returns the platform-specific scancode.
        [[nodiscard]] i32 GetScancode() const { return m_Scancode; }
        /// @brief Returns the GLFW modifier-key bitfield.
        [[nodiscard]] i32 GetMods() const { return m_Mods; }

    private:
        /// @brief The key that was pressed.
        const Key m_Key;
        /// @brief Platform-specific scancode (GLFW-native).
        const i32 m_Scancode;
        /// @brief GLFW modifier-key bitfield.
        const i32 m_Mods;
    };

    /// @brief Fired when a key transitions to the released state.
    class KeyReleasedEvent final : public Event
    {
    public:
        /// @brief Constructs the event.
        /// @param key       The key that was released.
        /// @param scancode  Platform-specific scancode (GLFW-native), for the ImGui sink.
        /// @param mods      GLFW modifier-key bitfield, for the ImGui sink.
        KeyReleasedEvent(Key key, i32 scancode, i32 mods)
            : m_Key(key), m_Scancode(scancode), m_Mods(mods)
        {
        }

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(KeyReleased);

        /// @brief Returns the key that was released.
        [[nodiscard]] Key GetKey() const { return m_Key; }
        /// @brief Returns the platform-specific scancode.
        [[nodiscard]] i32 GetScancode() const { return m_Scancode; }
        /// @brief Returns the GLFW modifier-key bitfield.
        [[nodiscard]] i32 GetMods() const { return m_Mods; }

    private:
        /// @brief The key that was released.
        const Key m_Key;
        /// @brief Platform-specific scancode (GLFW-native).
        const i32 m_Scancode;
        /// @brief GLFW modifier-key bitfield.
        const i32 m_Mods;
    };

    /// @brief Fired when the platform's auto-repeat re-asserts a key that is already down.
    ///
    /// Carries no state transition — the key was down before this event and stays down after — so it
    /// is a *distinct type* from KeyPressed rather than a flag on it. That separation is the contract:
    /// the Input snapshot ignores repeats outright, so an edge query (WasKeyPressed) still fires
    /// exactly once per physical press however long the key is held, and a consumer opts in to
    /// repetition by handling this type. Only actions that are meaningful to repeat should read it —
    /// caret movement and deletion in a focused text field — never a discrete action.
    ///
    /// Cadence is the platform's: the initial delay and the rate are the ones the operating system's
    /// keyboard settings define, so the engine holds no repeat timer and matches every other
    /// application on the machine. Repeats arrive only while the window holds keyboard focus.
    class KeyRepeatEvent final : public Event
    {
    public:
        /// @brief Constructs the event.
        /// @param key       The key the platform re-asserted; already down.
        /// @param scancode  Platform-specific scancode (GLFW-native).
        /// @param mods      GLFW modifier-key bitfield.
        KeyRepeatEvent(Key key, i32 scancode, i32 mods)
            : m_Key(key), m_Scancode(scancode), m_Mods(mods)
        {
        }

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(KeyRepeat);

        /// @brief Returns the key the platform re-asserted.
        [[nodiscard]] Key GetKey() const { return m_Key; }
        /// @brief Returns the platform-specific scancode.
        [[nodiscard]] i32 GetScancode() const { return m_Scancode; }
        /// @brief Returns the GLFW modifier-key bitfield.
        [[nodiscard]] i32 GetMods() const { return m_Mods; }

    private:
        /// @brief The key the platform re-asserted.
        const Key m_Key;
        /// @brief Platform-specific scancode (GLFW-native).
        const i32 m_Scancode;
        /// @brief GLFW modifier-key bitfield.
        const i32 m_Mods;
    };

    /// @brief Fired when text input produces a Unicode codepoint.
    ///
    /// Distinct from KeyPressed: this is the layout-resolved character a text field consumes,
    /// not a physical key. Only the ImGui sink reads it; gameplay input ignores it.
    class KeyTypedEvent final : public Event
    {
    public:
        /// @brief Constructs the event with the produced Unicode codepoint.
        explicit KeyTypedEvent(u32 codepoint) : m_Codepoint(codepoint) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(KeyTyped);

        /// @brief Returns the produced Unicode codepoint.
        [[nodiscard]] u32 GetCodepoint() const { return m_Codepoint; }

    private:
        /// @brief The produced Unicode codepoint.
        const u32 m_Codepoint;
    };

    /// @brief Fired when a mouse button transitions to the pressed state.
    class MouseButtonPressedEvent final : public Event
    {
    public:
        /// @brief Constructs the event.
        /// @param button  The button that was pressed.
        /// @param mods    GLFW modifier-key bitfield, for the ImGui sink.
        MouseButtonPressedEvent(MouseButton button, i32 mods) : m_Button(button), m_Mods(mods) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(MouseButtonPressed);

        /// @brief Returns the button that was pressed.
        [[nodiscard]] MouseButton GetButton() const { return m_Button; }
        /// @brief Returns the GLFW modifier-key bitfield.
        [[nodiscard]] i32 GetMods() const { return m_Mods; }

    private:
        /// @brief The button that was pressed.
        const MouseButton m_Button;
        /// @brief GLFW modifier-key bitfield.
        const i32 m_Mods;
    };

    /// @brief Fired when a mouse button transitions to the released state.
    class MouseButtonReleasedEvent final : public Event
    {
    public:
        /// @brief Constructs the event.
        /// @param button  The button that was released.
        /// @param mods    GLFW modifier-key bitfield, for the ImGui sink.
        MouseButtonReleasedEvent(MouseButton button, i32 mods) : m_Button(button), m_Mods(mods) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(MouseButtonReleased);

        /// @brief Returns the button that was released.
        [[nodiscard]] MouseButton GetButton() const { return m_Button; }
        /// @brief Returns the GLFW modifier-key bitfield.
        [[nodiscard]] i32 GetMods() const { return m_Mods; }

    private:
        /// @brief The button that was released.
        const MouseButton m_Button;
        /// @brief GLFW modifier-key bitfield.
        const i32 m_Mods;
    };

    /// @brief Fired when the cursor moves.
    ///
    /// Position is in window-space pixels; under a captured (disabled) cursor it is a virtual
    /// accumulating coordinate, so successive events still yield correct relative motion.
    class MouseMovedEvent final : public Event
    {
    public:
        /// @brief Constructs the event with the new cursor position in window-space pixels.
        /// @param position  The new cursor position.
        /// @param basis     The coordinate basis @p position is measured in (see GetBasis).
        explicit MouseMovedEvent(vec2 position, u32 basis = 0)
            : m_Position(position), m_Basis(basis)
        {
        }

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(MouseMoved);

        /// @brief Returns the new cursor position in window-space pixels.
        [[nodiscard]] vec2 GetPosition() const { return m_Position; }

        /// @brief Returns the coordinate basis the position is measured in.
        ///
        /// Positions are only comparable within one basis. Capturing the cursor measures it in a
        /// virtual coordinate that accumulates relative motion without bound, and releasing it puts
        /// the OS cursor back where the capture began, so the positions either side of a capture
        /// switch are unrelated and their difference is not motion. The window advances the basis
        /// at each switch (Window::GetCursorBasis); a consumer deriving travel from successive
        /// positions re-seeds when it changes.
        [[nodiscard]] u32 GetBasis() const { return m_Basis; }

    private:
        /// @brief New cursor position in window-space pixels.
        const vec2 m_Position;
        /// @brief The coordinate basis m_Position is measured in.
        const u32 m_Basis;
    };

    /// @brief Fired when the scroll wheel moves.
    class MouseScrolledEvent final : public Event
    {
    public:
        /// @brief Constructs the event with the scroll offset as (x, y).
        explicit MouseScrolledEvent(vec2 offset) : m_Offset(offset) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(MouseScrolled);

        /// @brief Returns the scroll offset as (x, y).
        [[nodiscard]] vec2 GetOffset() const { return m_Offset; }

    private:
        /// @brief Scroll offset as (x, y).
        const vec2 m_Offset;
    };

    /// @brief Fired when the cursor enters or leaves the window's content area.
    class MouseEnteredEvent final : public Event
    {
    public:
        /// @brief Constructs the event.
        /// @param entered  True when the cursor entered, false when it left.
        explicit MouseEnteredEvent(bool entered) : m_Entered(entered) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(MouseEntered);

        /// @brief Returns true if the cursor entered the window, false if it left.
        [[nodiscard]] bool HasEntered() const { return m_Entered; }

    private:
        /// @brief True when the cursor entered, false when it left.
        const bool m_Entered;
    };

    /// @brief Fired when a gamepad is connected to a slot.
    ///
    /// The discrete transition an assignment policy consumes; per-frame button/axis state stays a
    /// polled snapshot on Veng::Input, not events.
    class GamepadConnectedEvent final : public Event
    {
    public:
        /// @brief Constructs the event with the slot the pad connected to.
        explicit GamepadConnectedEvent(GamepadId id) : m_Id(id) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(GamepadConnected);

        /// @brief Returns the slot the pad connected to.
        [[nodiscard]] GamepadId GetGamepadId() const { return m_Id; }

    private:
        /// @brief The slot the pad connected to.
        const GamepadId m_Id;
    };

    /// @brief Fired when a gamepad is disconnected from a slot.
    class GamepadDisconnectedEvent final : public Event
    {
    public:
        /// @brief Constructs the event with the slot the pad left.
        explicit GamepadDisconnectedEvent(GamepadId id) : m_Id(id) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(GamepadDisconnected);

        /// @brief Returns the slot the pad left.
        [[nodiscard]] GamepadId GetGamepadId() const { return m_Id; }

    private:
        /// @brief The slot the pad left.
        const GamepadId m_Id;
    };

    /// @brief Which edit a VirtualGamepadEvent makes to its slot's virtual pad.
    enum class VirtualGamepadOp : u8
    {
        /// @brief Connects a virtual pad in the slot, of the event's GamepadType.
        Connect,
        /// @brief Disconnects the slot's virtual pad.
        Disconnect,
        /// @brief Sets one button down or up.
        Button,
        /// @brief Sets one axis to a value.
        Axis,
        /// @brief Puts the first touchpad finger down at a position, moves it, or lifts it.
        Touch,
    };

    /// @brief A synthetic edit to a virtual gamepad, the automation seam for pad input.
    ///
    /// A virtual pad occupies a slot exactly as a physical one does, and everything reading pads
    /// sees an ordinary pad there. An injector (a test harness, an automation tool) posts these
    /// through InputRouter::PostInjectedEvent; the router paces them like injected keys, so a press
    /// and its release straddle a frame, and hands each to the pad backend rather than to the input
    /// snapshot. Fields not read by the event's op are ignored.
    class VirtualGamepadEvent final : public Event
    {
    public:
        /// @brief Builds an event connecting a virtual pad in a slot.
        /// @param slot  The slot to occupy; the connect is refused while it holds another pad.
        /// @param type  The family the virtual pad reports.
        /// @return The event.
        [[nodiscard]] static VirtualGamepadEvent Connect(GamepadId slot, GamepadType type)
        {
            VirtualGamepadEvent event(VirtualGamepadOp::Connect, slot);
            event.m_Type = type;
            return event;
        }

        /// @brief Builds an event disconnecting the virtual pad in a slot.
        /// @param slot  The slot to free; a slot holding a physical pad is left alone.
        /// @return The event.
        [[nodiscard]] static VirtualGamepadEvent Disconnect(GamepadId slot)
        {
            return VirtualGamepadEvent(VirtualGamepadOp::Disconnect, slot);
        }

        /// @brief Builds an event setting one button of a virtual pad.
        /// @param slot    The virtual pad's slot.
        /// @param button  The button to set.
        /// @param down    True to hold it down, false to release it.
        /// @return The event.
        [[nodiscard]] static VirtualGamepadEvent SetButton(GamepadId slot, GamepadButton button,
                                                           bool down)
        {
            VirtualGamepadEvent event(VirtualGamepadOp::Button, slot);
            event.m_Button = button;
            event.m_Down = down;
            return event;
        }

        /// @brief Builds an event setting one axis of a virtual pad.
        /// @param slot   The virtual pad's slot.
        /// @param axis   The axis to set; a touchpad axis is set through Touch instead.
        /// @param value  The value, clamped to the axis's range when applied.
        /// @return The event.
        [[nodiscard]] static VirtualGamepadEvent SetAxis(GamepadId slot, GamepadAxis axis,
                                                         f32 value)
        {
            VirtualGamepadEvent event(VirtualGamepadOp::Axis, slot);
            event.m_Axis = axis;
            event.m_Value = value;
            return event;
        }

        /// @brief Builds an event placing, moving or lifting a virtual pad's first touchpad finger.
        /// @param slot      The virtual pad's slot.
        /// @param down      True while the finger rests on the touchpad, false to lift it.
        /// @param position  The finger's position, 0..1 on each axis; ignored when lifting.
        /// @return The event.
        [[nodiscard]] static VirtualGamepadEvent SetTouch(GamepadId slot, bool down, vec2 position)
        {
            VirtualGamepadEvent event(VirtualGamepadOp::Touch, slot);
            event.m_Down = down;
            event.m_Position = position;
            return event;
        }

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(VirtualGamepad);

        /// @brief Returns the edit this event makes.
        [[nodiscard]] VirtualGamepadOp GetOp() const { return m_Op; }

        /// @brief Returns the slot the edit applies to.
        [[nodiscard]] GamepadId GetSlot() const { return m_Slot; }

        /// @brief Returns the family a Connect gives the pad.
        [[nodiscard]] GamepadType GetType() const { return m_Type; }

        /// @brief Returns the button a Button edit sets.
        [[nodiscard]] GamepadButton GetButton() const { return m_Button; }

        /// @brief Returns the axis an Axis edit sets.
        [[nodiscard]] GamepadAxis GetAxis() const { return m_Axis; }

        /// @brief Returns the value an Axis edit sets.
        [[nodiscard]] f32 GetValue() const { return m_Value; }

        /// @brief Returns whether a Button edit holds its button, or a Touch edit puts its finger
        ///        down.
        [[nodiscard]] bool IsDown() const { return m_Down; }

        /// @brief Returns the finger position a Touch edit sets, 0..1 on each axis.
        [[nodiscard]] vec2 GetPosition() const { return m_Position; }

    private:
        /// @brief Constructs an event of the given op for a slot; the factories fill the rest.
        VirtualGamepadEvent(VirtualGamepadOp op, GamepadId slot) : m_Op(op), m_Slot(slot) {}

        /// @brief The edit this event makes.
        VirtualGamepadOp m_Op;
        /// @brief The slot the edit applies to.
        GamepadId m_Slot;
        /// @brief The family a Connect gives the pad.
        GamepadType m_Type = GamepadType::Standard;
        /// @brief The button a Button edit sets.
        GamepadButton m_Button = GamepadButton::A;
        /// @brief The axis an Axis edit sets.
        GamepadAxis m_Axis = GamepadAxis::LeftX;
        /// @brief The value an Axis edit sets.
        f32 m_Value = 0.0f;
        /// @brief Whether a Button edit holds its button, or a Touch edit puts its finger down.
        bool m_Down = false;
        /// @brief The finger position a Touch edit sets.
        vec2 m_Position = {0, 0};
    };
}
