#pragma once

#include <functional>
#include <vector>

#include <Veng/Veng.h>
#include <Veng/Event.h>
#include <Veng/Input.h>
#include <Veng/Render/DisplayCapabilities.h>
#include <Veng/Render/DisplayModes.h>

struct GLFWwindow;

namespace Veng
{
    namespace Renderer
    {
        class Context;
    }

    /// @brief Window creation parameters.
    struct WindowInfo
    {
        /// @brief Initial window size in pixels.
        uvec2 Extent;
        /// @brief Whether the window may be resized by the user.
        bool Resizable;
        /// @brief Window title bar text.
        string Title;
        /// @brief Whether to capture the mouse cursor on creation.
        bool CaptureMouse;
        /// @brief How the window occupies the display at creation; Windowed is the default.
        ///
        /// Applied after the window is created (a non-windowed value moves it onto its monitor). The
        /// runtime setter ApplyDisplayMode changes it later. Exclusive is best-effort — see
        /// FullscreenMode.
        FullscreenMode Fullscreen = FullscreenMode::Windowed;
        /// @brief Index of the monitor to target when Fullscreen is not Windowed; 0 is the primary.
        u32 MonitorId = 0;
    };

    /// @brief File-dialog filter entry, e.g. {"Images", "png,jpg"}.
    ///
    /// Replaces the nfd type in the public signature; the nfd mapping lives in Window.cpp.
    struct FileDialogFilter
    {
        /// @brief Display name for this filter category.
        string Name;
        /// @brief Comma-separated list of extensions (no dots).
        string Extensions;
    };

    /// @brief Native OS window with GLFW backing.
    class Window
    {
    public:
        /// @brief Initializes GLFW (first window only) and creates the native window.
        ///
        /// The window exists independently of any rendering context; the context
        /// borrows it and calls CreateSurface during its own initialization.
        /// @param info  Window creation parameters.
        explicit Window(const WindowInfo& info);

        /// @brief Creates a Window wrapped in a Unique<Window>.
        static Unique<Window> Create(const WindowInfo& info);

        /// @brief Opens a native open-file dialog; returns true if a path was selected.
        /// @param outSelectedPath  Receives the chosen file path on success.
        /// @param defaultPath      Initial directory for the dialog.
        /// @param filters          File-type filters presented to the user.
        static bool OpenFileDialog(string& outSelectedPath, const string& defaultPath,
                                   const vector<FileDialogFilter>& filters);

        /// @brief Opens a native save-file dialog; returns true if a path was selected.
        /// @param outSelectedPath  Receives the chosen file path on success.
        /// @param defaultPath      Initial directory for the dialog.
        /// @param filters          File-type filters presented to the user.
        static bool SaveFileDialog(string& outSelectedPath, const string& defaultPath,
                                   const vector<FileDialogFilter>& filters);

        /// @brief Destroys the native window and terminates GLFW.
        ///
        /// The Vulkan surface created by CreateSurface is owned by the render context;
        /// the context must be disposed before this destructor runs.
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        /// @brief Creates the Vulkan surface for this window on the given context.
        void CreateSurface(const Renderer::Context& context);

        /// @brief Blocks until the window has a non-zero extent (resumes after un-minimize).
        void SpinUntilValidSize();

        /// @brief Returns true if the window is currently minimized.
        [[nodiscard]] bool IsMinimized() const;

        /// @brief Returns whether the framebuffer was resized since the last call, then clears the flag.
        ///
        /// Set by the framebuffer-size callback during Update(); the render context consults it
        /// each frame to recreate the swapchain proactively, before a submit whose deferred
        /// drawable acquire would otherwise deadlock against a stale-sized swapchain.
        [[nodiscard]] bool ConsumeFramebufferResized();

        /// @brief Captures the mouse cursor, hiding it and locking it to this window.
        void CaptureMouse();
        /// @brief Releases a previously captured mouse cursor.
        ///
        /// The freed cursor honors the visibility set by SetCursorVisible: released while
        /// hidden, the cursor moves freely but is not drawn.
        void ReleaseMouse();
        /// @brief Returns true if the mouse cursor is currently captured.
        [[nodiscard]] bool IsMouseCaptured() const;

        /// @brief Shows or hides the free (uncaptured) mouse cursor over this window.
        ///
        /// Hidden, the cursor still moves and reports positions normally — it just is not
        /// drawn while over the window, so an app can render its own software cursor. The
        /// flag is independent of capture (a captured cursor is always hidden and locked):
        /// set while captured, it takes effect when the capture releases.
        /// @param visible  True to draw the OS cursor when free, false to hide it.
        void SetCursorVisible(bool visible);
        /// @brief Returns whether the free (uncaptured) cursor is drawn over this window.
        [[nodiscard]] bool IsCursorVisible() const;

        /// @brief Polls OS events, enqueuing each as a typed Event for this frame.
        ///
        /// GLFW callbacks fire during the poll and push events onto an internal queue;
        /// drain it with DrainEvents after calling this.
        void Update();

        /// @brief Drains this frame's queued events to @p handler, then clears the queue.
        ///
        /// Called once per frame after Update; the InputRouter is the handler, routing each
        /// event to ImGui and the Input snapshot by the active focus.
        /// @param handler  Invoked with each queued event in arrival order.
        void DrainEvents(const std::function<void(Event&)>& handler);

        /// @brief Polls every present joystick into a slot-indexed GamepadState set.
        ///
        /// Fills one GamepadState per slot the span covers, from GLFW's polled gamepad API; a slot
        /// with no gamepad-mapped pad is left unconnected. The one place GLFW's gamepad state is
        /// read, so Veng::Input stays backend-free. Called once per frame before the snapshot is
        /// finalized; connect/disconnect transitions arrive separately as queued events.
        /// @param states  Slot-indexed output, one entry per joystick slot to poll.
        void PollGamepads(std::span<GamepadState> states) const;

        /// @brief Signals the run loop to exit; sets IsOpen() to false without destroying anything.
        void Close();

        /// @brief Returns true if Close() has been called.
        [[nodiscard]] bool ShouldClose() const;

        /// @brief Returns true if the given key is currently held down.
        [[nodiscard]] bool KeyPressed(Key key) const;

        /// @brief Returns true if the given mouse button is currently held down.
        [[nodiscard]] bool MouseButtonPressed(MouseButton button) const;

        /// @brief Returns the scroll wheel offset accumulated since the last call, then resets it.
        ///
        /// Scroll arrives through a GLFW callback that sums offsets into a member; this
        /// drains and zeroes that accumulator, so consecutive calls partition the scroll.
        /// @return Accumulated scroll offset as (x, y).
        [[nodiscard]] vec2 ConsumeScrollDelta();

        /// @brief Returns the current window extent in pixels.
        [[nodiscard]] uvec2 GetExtent() const;

        /// @brief Returns the framebuffer-pixels-per-window-coordinate scale as (x, y).
        ///
        /// GLFW reports the cursor position and window size in window coordinates (logical
        /// points), while the framebuffer extent (GetExtent) is in pixels; on a HiDPI display
        /// the two differ by this ratio. Multiplying a window-coordinate point by this scale
        /// converts it into framebuffer pixels — the space the swapchain and viewport regions
        /// live in. It is (1, 1) on a 1:1 display and degenerate window sizes.
        [[nodiscard]] vec2 GetContentScale() const;

        /// @brief Returns the current window width in pixels.
        [[nodiscard]] u32 GetWidth() const;

        /// @brief Returns the current window height in pixels.
        [[nodiscard]] u32 GetHeight() const;

        /// @brief Returns true if the window has not been closed.
        [[nodiscard]] bool IsOpen() const;

        /// @brief Returns the current mouse cursor position in window-space pixels.
        [[nodiscard]] vec2 GetMousePosition() const;

        /// @brief Sets the window title bar text.
        void SetTitle(const string& title);
        /// @brief Returns the current window title bar text.
        [[nodiscard]] string GetTitle() const;

        /// @brief Enumerates the connected monitors and the video modes each can drive.
        ///
        /// GLFW-backed, so it reports the live hardware (index 0 the primary, matching
        /// BuiltinDisplayChoices::MonitorId). Each monitor's mode list is deduplicated and ordered.
        /// Returns an empty list before GLFW is initialized (no window has been created). The
        /// present-mode half of the display capabilities is a swapchain concern and lives on Context.
        /// @return One MonitorInfo per connected monitor.
        [[nodiscard]] static vector<MonitorInfo> EnumerateMonitors();

        /// @brief Applies a display mode to the live window: windowed / borderless / exclusive.
        ///
        /// On macOS full-screen is driven natively (Cocoa's -[NSWindow toggleFullScreen:], a separate
        /// Space, the same state the green title-bar button toggles): any non-windowed @p mode enters
        /// it and Windowed leaves it, with @p monitorId and @p refreshHz not applying to a native
        /// toggle. Elsewhere Windowed leaves the window on the desktop (resizing it to @p resolution
        /// when non-zero), Borderless moves it onto @p monitorId at that monitor's current mode, and
        /// Exclusive takes the requested @p resolution and @p refreshHz. A zero resolution or refresh
        /// means the display's native value. The framebuffer-size change this causes drives the
        /// existing swapchain recreation on the next frame, so the caller applies it at a frame-safe
        /// point and needs no separate recreate call. The transition may be asynchronous.
        /// @param mode        The target fullscreen mode.
        /// @param monitorId   The monitor to target for a non-windowed mode; 0 is the primary.
        /// @param resolution  The requested pixel resolution, or (0, 0) for the display's native size.
        /// @param refreshHz   The requested refresh rate in Hz, or 0 for the display's native refresh.
        void ApplyDisplayMode(FullscreenMode mode, u32 monitorId, uvec2 resolution, u32 refreshHz);

        /// @brief Returns the window's current fullscreen mode.
        ///
        /// On macOS this reads the live native Cocoa full-screen state, so a full-screen entered or
        /// left by the green title-bar button is reflected here — the persisted selection and the
        /// window can never disagree. Elsewhere it returns the last mode ApplyDisplayMode set.
        [[nodiscard]] FullscreenMode GetFullscreenMode() const;

        /// @brief Returns whether the window is currently full-screen (any non-windowed mode).
        [[nodiscard]] bool IsFullscreen() const
        {
            return GetFullscreenMode() != FullscreenMode::Windowed;
        }

        struct Native;
        /// @brief Returns the backend-private native handle struct.
        [[nodiscard]] Native& GetNative() const;

        /// @brief Escape hatch returning the raw GLFWwindow* for interop.
        ///
        /// GLFWwindow* is kept as a direct member rather than inside Native because
        /// GLFW is a free-function API that does not need a Vulkan-style handle wrapper.
        friend GLFWwindow* GetGlfwWindow(const Window& window);

    private:
        bool m_Open = true;
        uvec2 m_Extent{};
        /// @brief Set by the framebuffer-size callback, cleared by ConsumeFramebufferResized().
        bool m_FramebufferResized = false;
        bool m_Resizable;
        string m_Title;
        /// @brief How the window currently occupies the display.
        FullscreenMode m_Fullscreen = FullscreenMode::Windowed;
        /// @brief The windowed position (window coords) captured before going fullscreen, to restore.
        ivec2 m_WindowedPosition{0, 0};
        /// @brief The windowed size (window coords) captured before going fullscreen, to restore.
        uvec2 m_WindowedExtent{0, 0};
        bool m_MouseCaptured;
        /// @brief Whether the free cursor is drawn; applied on release while captured.
        bool m_CursorVisible = true;
        GLFWwindow* m_Handle = nullptr;
        vec2 m_MousePosition = {0, 0};
        vec2 m_ScrollDelta = {0, 0};

        /// @brief This frame's queued events, filled by GLFW callbacks and drained by DrainEvents.
        vector<Unique<Event>> m_Events;

        Unique<Native> m_Native;
    };
}
