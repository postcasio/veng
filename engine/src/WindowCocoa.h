#pragma once

struct GLFWwindow;

namespace Veng
{
    /// @brief Drives the window's native Cocoa full-screen state (macOS only).
    ///
    /// Toggles -[NSWindow toggleFullScreen:] when the window's current native state differs from
    /// @p fullscreen, so the request converges on the same full-screen a user gets from the green
    /// title-bar button (a separate Space). The transition is animated and asynchronous, so the
    /// state may not have settled by the time this returns; query IsNativeFullscreen to read it.
    /// A no-op when the window has no Cocoa backing.
    /// @param handle      The GLFW window whose Cocoa window is toggled.
    /// @param fullscreen  The desired native full-screen state.
    void SetNativeFullscreen(GLFWwindow* handle, bool fullscreen);

    /// @brief Returns the window's live native Cocoa full-screen state (macOS only).
    ///
    /// Reads NSWindowStyleMaskFullScreen from the window's style mask, so it reflects a full-screen
    /// entered or left by the green title-bar button, not only one this engine requested. False when
    /// the window has no Cocoa backing.
    /// @param handle  The GLFW window whose Cocoa window is queried.
    /// @return True when the window is in native full-screen.
    [[nodiscard]] bool IsNativeFullscreen(GLFWwindow* handle);
}
