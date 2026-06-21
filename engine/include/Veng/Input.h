#pragma once

#include <array>

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Keyboard key codes.
    ///
    /// Engine-owned so consumers need not include GLFW. Values are GLFW-compatible;
    /// the backend mapping in Window.cpp is a direct cast.
    enum class Key : u16
    {
        Space = 32,
        Apostrophe = 39,
        Comma = 44,
        Minus = 45,
        Period = 46,
        Slash = 47,
        Num0 = 48,
        Num1,
        Num2,
        Num3,
        Num4,
        Num5,
        Num6,
        Num7,
        Num8,
        Num9,
        Semicolon = 59,
        Equal = 61,
        A = 65,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,
        LeftBracket = 91,
        Backslash = 92,
        RightBracket = 93,
        GraveAccent = 96,
        Escape = 256,
        Enter = 257,
        Tab = 258,
        Backspace = 259,
        Insert = 260,
        Delete = 261,
        Right = 262,
        Left = 263,
        Down = 264,
        Up = 265,
        PageUp = 266,
        PageDown = 267,
        Home = 268,
        End = 269,
        F1 = 290,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        LeftShift = 340,
        LeftControl = 341,
        LeftAlt = 342,
        LeftSuper = 343,
        RightShift = 344,
        RightControl = 345,
        RightAlt = 346,
        RightSuper = 347,
    };

    /// @brief Mouse button codes; values match GLFW constants.
    enum class MouseButton : u8
    {
        Left = 0,
        Right = 1,
        Middle = 2,
    };

    class Window;

    /// @brief Frame-coherent input service, owned by Application and updated once per frame.
    ///
    /// Mirrors Time: a per-frame service the run loop pumps before OnUpdate/OnRender.
    /// It snapshots current and previous key/button state each frame so callers get
    /// per-frame edges (WasKeyPressed/WasKeyReleased) and deltas (mouse + scroll) on
    /// top of the level queries the Window exposes. It borrows the Window and reads
    /// state from it; it is driven from the single render thread like the rest of veng.
    class Input
    {
    public:
        /// @brief Constructs the input service borrowing the given window.
        /// @param window  Window polled for key/button/mouse state; must outlive this.
        explicit Input(Window& window);

        /// @brief Snapshots input state for the new frame; called once at the top of the frame loop.
        ///
        /// Copies current state to previous, re-polls current key/button/mouse state from
        /// the window, computes the mouse delta, and consumes the accumulated scroll delta.
        /// @pre Must run before OnUpdate/OnRender so per-frame edges and deltas are current.
        void Update();

        /// @brief Returns true if the given key is currently held down.
        [[nodiscard]] bool IsKeyDown(Key key) const;

        /// @brief Returns true only on the frame the key transitioned from up to down.
        [[nodiscard]] bool WasKeyPressed(Key key) const;

        /// @brief Returns true only on the frame the key transitioned from down to up.
        [[nodiscard]] bool WasKeyReleased(Key key) const;

        /// @brief Returns true if the given mouse button is currently held down.
        [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const;

        /// @brief Returns true only on the frame the button transitioned from up to down.
        [[nodiscard]] bool WasMouseButtonPressed(MouseButton button) const;

        /// @brief Returns true only on the frame the button transitioned from down to up.
        [[nodiscard]] bool WasMouseButtonReleased(MouseButton button) const;

        /// @brief Returns the current mouse cursor position in window-space pixels.
        [[nodiscard]] vec2 GetMousePosition() const;

        /// @brief Returns the change in mouse position since the previous frame, in pixels.
        ///
        /// Works while the mouse is captured: relative motion accumulates so a fly
        /// camera reads continuous deltas with the OS cursor hidden and locked.
        [[nodiscard]] vec2 GetMouseDelta() const;

        /// @brief Returns the scroll wheel delta accumulated this frame as (x, y).
        [[nodiscard]] vec2 GetScrollDelta() const;

        /// @brief Captures or releases the mouse cursor.
        ///
        /// Captured hides and locks the OS cursor and accumulates relative motion,
        /// the mode a fly camera drives in. Delegates to the window.
        /// @param captured  True to capture, false to release.
        void SetMouseCaptured(bool captured);

        /// @brief Returns true if the mouse cursor is currently captured.
        [[nodiscard]] bool IsMouseCaptured() const;

    private:
        /// @brief Highest GLFW key code, sizing the key state bitsets.
        static constexpr usize MaxKeys = 512;
        /// @brief Number of tracked mouse buttons.
        static constexpr usize MaxMouseButtons = 8;

        /// @brief Borrowed window polled for input state; must outlive this.
        Window& m_Window;

        /// @brief Per-key held state this frame, indexed by key code.
        std::array<bool, MaxKeys> m_Keys{};
        /// @brief Per-key held state last frame, indexed by key code.
        std::array<bool, MaxKeys> m_PreviousKeys{};

        /// @brief Per-button held state this frame, indexed by button code.
        std::array<bool, MaxMouseButtons> m_MouseButtons{};
        /// @brief Per-button held state last frame, indexed by button code.
        std::array<bool, MaxMouseButtons> m_PreviousMouseButtons{};

        /// @brief Mouse position this frame, in window-space pixels.
        vec2 m_MousePosition = {0, 0};
        /// @brief Mouse position last frame, in window-space pixels.
        vec2 m_PreviousMousePosition = {0, 0};
        /// @brief Mouse position change since last frame.
        vec2 m_MouseDelta = {0, 0};

        /// @brief Scroll delta consumed from the window this frame.
        vec2 m_ScrollDelta = {0, 0};

        /// @brief True until the first Update, so the opening frame reports a zero mouse delta.
        bool m_FirstUpdate = true;
    };
}
