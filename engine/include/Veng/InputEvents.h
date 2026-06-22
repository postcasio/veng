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
        explicit MouseMovedEvent(vec2 position) : m_Position(position) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(MouseMoved);

        /// @brief Returns the new cursor position in window-space pixels.
        [[nodiscard]] vec2 GetPosition() const { return m_Position; }

    private:
        /// @brief New cursor position in window-space pixels.
        const vec2 m_Position;
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
}
