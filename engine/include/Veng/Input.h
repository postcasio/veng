#pragma once

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
        Comma = 44, Minus = 45, Period = 46, Slash = 47,
        Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
        Semicolon = 59, Equal = 61,
        A = 65, B, C, D, E, F, G, H, I, J, K, L, M,
        N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
        Escape = 256, Enter = 257, Tab = 258, Backspace = 259,
        Insert = 260, Delete = 261,
        Right = 262, Left = 263, Down = 264, Up = 265,
        PageUp = 266, PageDown = 267, Home = 268, End = 269,
        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        LeftShift = 340, LeftControl = 341, LeftAlt = 342, LeftSuper = 343,
        RightShift = 344, RightControl = 345, RightAlt = 346, RightSuper = 347,
    };

    /// @brief Mouse button codes; values match GLFW constants.
    enum class MouseButton : u8
    {
        Left = 0, Right = 1, Middle = 2,
    };
}
