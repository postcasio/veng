#pragma once

#include <functional>
#include <vector>

#include <Veng/Veng.h>
#include <Veng/Event.h>
#include <Veng/Input.h>

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

        /// @brief Captures the mouse cursor, hiding it and locking it to this window.
        void CaptureMouse();
        /// @brief Releases a previously captured mouse cursor.
        void ReleaseMouse();
        /// @brief Returns true if the mouse cursor is currently captured.
        [[nodiscard]] bool IsMouseCaptured() const;

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
        bool m_Resizable;
        string m_Title;
        bool m_MouseCaptured;
        GLFWwindow* m_Handle = nullptr;
        vec2 m_MousePosition = {0, 0};
        vec2 m_ScrollDelta = {0, 0};

        /// @brief This frame's queued events, filled by GLFW callbacks and drained by DrainEvents.
        vector<Unique<Event>> m_Events;

        Unique<Native> m_Native;
    };
}
