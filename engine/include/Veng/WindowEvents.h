#pragma once

#include <Veng/Veng.h>
#include <Veng/Event.h>

namespace Veng
{
    /// @brief Fired when the window is resized.
    class WindowResizeEvent final : public Event
    {
    public:
        /// @brief Constructs the event with the new window dimensions.
        /// @param width   New width in pixels.
        /// @param height  New height in pixels.
        WindowResizeEvent(u32 width, u32 height) : m_Width(width), m_Height(height) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(WindowResize);

        /// @brief Returns the new window width in pixels.
        [[nodiscard]] u32 GetWidth() const { return m_Width; }
        /// @brief Returns the new window height in pixels.
        [[nodiscard]] u32 GetHeight() const { return m_Height; }

    private:
        /// @brief New width in pixels.
        const u32 m_Width;
        /// @brief New height in pixels.
        const u32 m_Height;
    };

    /// @brief Fired when the user requests the window to close.
    class WindowCloseEvent final : public Event
    {
    public:
        WindowCloseEvent() = default;

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(WindowClose);
    };

    /// @brief Fired when the window gains or loses OS input focus.
    ///
    /// The router pops a held gameplay focus on focus loss so alt-tab frees a captured cursor.
    class WindowFocusEvent final : public Event
    {
    public:
        /// @brief Constructs the event.
        /// @param focused  True when the window gained focus, false when it lost focus.
        explicit WindowFocusEvent(bool focused) : m_Focused(focused) {}

        /// @brief Injects this event's type-identity members (see the EVENT macro).
        EVENT(WindowFocus);

        /// @brief Returns true if the window gained focus, false if it lost focus.
        [[nodiscard]] bool IsFocused() const { return m_Focused; }

    private:
        /// @brief True when the window gained focus, false when it lost it.
        const bool m_Focused;
    };
}
