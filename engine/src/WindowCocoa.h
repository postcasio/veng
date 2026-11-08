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

    /// @brief Backs the window's content view with a CAMetalLayer and returns it (macOS only).
    ///
    /// The view owns the layer; the pointer is returned +0, and a caller keeping it retains it. The
    /// layer's contents scale follows the window's backing scale, so its drawables are in pixels.
    /// @param handle  The GLFW window whose content view takes the layer.
    /// @return The CAMetalLayer, as an opaque pointer; null when the window has no Cocoa backing.
    [[nodiscard]] void* AttachMetalLayer(GLFWwindow* handle);
}
