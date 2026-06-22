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
    class Event;

    /// @brief Frame-coherent input service, always present, updated once per frame.
    ///
    /// Mirrors Time: a per-frame service the run loop pumps before OnUpdate/OnRender.
    /// It holds current and previous key/button state plus per-frame mouse/scroll deltas,
    /// so callers get per-frame edges (WasKeyPressed/WasKeyReleased) and deltas. State is
    /// **event-fed, not polled**: BeginFrame rolls the snapshot forward and the InputRouter
    /// applies this frame's routed events via ApplyEvent. It is driven from the single
    /// render thread like the rest of veng. The borrowed window is nullable: with no window
    /// (a headless run) no events arrive, so the snapshot stays the neutral all-zeros input
    /// a windowed app produces with nothing pressed.
    ///
    /// Because the router applies only the events routed to it, this snapshot reflects input
    /// **only while its layer owns focus** — a gameplay-focused snapshot sees the game's
    /// input exclusively; a UI-focused one sees input the editor camera reads.
    class Input
    {
    public:
        /// @brief Constructs the input service borrowing the given window.
        /// @param window  Window borrowed for cursor capture; nullptr for a headless run that
        ///                reports the neutral all-zeros state. Must outlive this when non-null.
        explicit Input(Window* window);

        /// @brief Rolls the snapshot forward for a new frame; called once at the top of the loop.
        ///
        /// Copies current key/button state to previous and clears the per-frame mouse and
        /// scroll deltas, so the edges and deltas the router then applies via ApplyEvent are
        /// this frame's. With no events applied the state stays neutral (nothing pressed).
        /// @pre Must run before the event drain so ApplyEvent writes into a fresh frame.
        void BeginFrame();

        /// @brief Folds one routed input event into the current snapshot.
        ///
        /// Engine-internal: the InputRouter calls this for each key/mouse event routed to
        /// this snapshot. Non-input events are ignored. Out-of-range key/button codes are
        /// guarded, never an out-of-bounds write.
        /// @param event  The event to apply.
        void ApplyEvent(const Event& event);

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
        /// the mode a fly camera drives in. Delegates to the window; no-ops with no
        /// window.
        /// @param captured  True to capture, false to release.
        void SetMouseCaptured(bool captured);

        /// @brief Returns true if the mouse cursor is currently captured.
        [[nodiscard]] bool IsMouseCaptured() const;

    private:
        /// @brief Highest GLFW key code, sizing the key state bitsets.
        static constexpr usize MaxKeys = 512;
        /// @brief Number of tracked mouse buttons.
        static constexpr usize MaxMouseButtons = 8;

        /// @brief Borrowed window polled for input state; nullptr in a headless run.
        Window* m_Window;

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
        /// @brief Accumulated mouse motion this frame, summed across this frame's move events.
        vec2 m_MouseDelta = {0, 0};

        /// @brief Scroll delta accumulated this frame.
        vec2 m_ScrollDelta = {0, 0};

        /// @brief False until the first move event seeds m_MousePosition, so the opening move reports no delta.
        bool m_HavePosition = false;
    };
}
