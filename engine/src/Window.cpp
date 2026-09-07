#include <Veng/Window.h>
#include <Veng/WindowEvents.h>
#include <Veng/InputEvents.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <nfd.h>

#include "Render/DisplayResolve.h"
#include "WindowCocoa.h"

#define GLFW_BOOL(x) ((x) ? GLFW_TRUE : GLFW_FALSE)

namespace Veng
{
    namespace
    {
        bool s_GlfwInitialized = false;

        // The joystick callback is a GLFW global with no window argument, so the single-window
        // engine routes connect/disconnect events through the live window here.
        Window* s_JoystickEventWindow = nullptr;

        void GLFWErrorCallback(int err, const char* message)
        {
            VE_ASSERT(false, "GLFW error ({0}): {1}", err, message);
        }

        // Engine GamepadButton index → GLFW_GAMEPAD_BUTTON_*, in GamepadButton declaration order.
        constexpr std::array<int, usize(GamepadButton::Count)> GamepadButtonToGlfw{
            GLFW_GAMEPAD_BUTTON_A,           GLFW_GAMEPAD_BUTTON_B,
            GLFW_GAMEPAD_BUTTON_X,           GLFW_GAMEPAD_BUTTON_Y,
            GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,
            GLFW_GAMEPAD_BUTTON_BACK,        GLFW_GAMEPAD_BUTTON_START,
            GLFW_GAMEPAD_BUTTON_GUIDE,       GLFW_GAMEPAD_BUTTON_LEFT_THUMB,
            GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, GLFW_GAMEPAD_BUTTON_DPAD_UP,
            GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,  GLFW_GAMEPAD_BUTTON_DPAD_DOWN,
            GLFW_GAMEPAD_BUTTON_DPAD_LEFT};

        // Engine GamepadAxis index → GLFW_GAMEPAD_AXIS_*, in GamepadAxis declaration order.
        constexpr std::array<int, usize(GamepadAxis::Count)> GamepadAxisToGlfw{
            GLFW_GAMEPAD_AXIS_LEFT_X,       GLFW_GAMEPAD_AXIS_LEFT_Y,
            GLFW_GAMEPAD_AXIS_RIGHT_X,      GLFW_GAMEPAD_AXIS_RIGHT_Y,
            GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER};
    }

    Window::Window(const WindowInfo& info)
        : m_Extent(info.Extent), m_Resizable(info.Resizable), m_Title(info.Title),
          m_MouseCaptured(info.CaptureMouse), m_Native(CreateUnique<Native>())
    {
        if (!s_GlfwInitialized)
        {
            Log::Info("Initializing GLFW");

            glfwSetErrorCallback(GLFWErrorCallback);

            // GLFW's Cocoa init chdir's the process into a bundle's Contents/Resources when it
            // finds one, so every relative path the process touches — its own and its host's —
            // silently resolves inside the application bundle, which is read-only in every sense
            // that matters (its contents are sealed by the code signature). Relocating the host
            // process's working directory is not the windowing layer's call to make.
            glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);

            if (glfwInit() != GLFW_TRUE)
            {
                VE_ASSERT(false, "Failed to initialize GLFW!");
            }

            s_GlfwInitialized = true;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_BOOL(m_Resizable));

        Log::Info("Creating window: {0} ({1}x{2})", m_Title, m_Extent.x, m_Extent.y);

        m_Handle = glfwCreateWindow(static_cast<int>(m_Extent.x), static_cast<int>(m_Extent.y),
                                    m_Title.c_str(), nullptr, nullptr);

        if (!m_Handle)
        {
            VE_ASSERT(false, "Failed to create window!");
        }

        glfwSetWindowUserPointer(m_Handle, this);

        glfwSetFramebufferSizeCallback(
            m_Handle,
            [](GLFWwindow* glfwWindow, int width, int height)
            {
                auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                // Keep the live extent current so GetWidth/GetHeight track the framebuffer, and
                // flag the change so the context recreates the swapchain before the next submit
                // (a swapchain that lags the CAMetalLayer deadlocks MoltenVK's drawable acquire).
                window->m_Extent = {static_cast<u32>(width), static_cast<u32>(height)};
                window->m_FramebufferResized = true;
                window->m_Events.push_back(CreateUnique<WindowResizeEvent>(
                    static_cast<u32>(width), static_cast<u32>(height)));
            });

        glfwSetWindowCloseCallback(m_Handle,
                                   [](GLFWwindow* glfwWindow)
                                   {
                                       auto window = static_cast<Window*>(
                                           glfwGetWindowUserPointer(glfwWindow));
                                       window->m_Events.push_back(CreateUnique<WindowCloseEvent>());
                                   });

        glfwSetWindowFocusCallback(
            m_Handle,
            [](GLFWwindow* glfwWindow, int focused)
            {
                auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                window->m_Events.push_back(CreateUnique<WindowFocusEvent>(focused == GLFW_TRUE));
            });

        glfwSetKeyCallback(
            m_Handle,
            [](GLFWwindow* glfwWindow, int key, int scancode, int action, int mods)
            {
                // GLFW_KEY_UNKNOWN (-1) has no engine Key; skip it. Named keys round-trip
                // through the u16 Key enum exactly, so the ImGui sink recovers the GLFW code.
                if (key < 0)
                {
                    return;
                }
                auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                const auto code = static_cast<Key>(key);
                if (action == GLFW_PRESS)
                {
                    window->m_Events.push_back(CreateUnique<KeyPressedEvent>(code, scancode, mods));
                }
                else if (action == GLFW_RELEASE)
                {
                    window->m_Events.push_back(
                        CreateUnique<KeyReleasedEvent>(code, scancode, mods));
                }
                else if (action == GLFW_REPEAT)
                {
                    // GLFW_REPEAT re-asserts a key that is already down, at the cadence the OS
                    // keyboard settings define. It carries no state transition, so it becomes a
                    // KeyRepeatEvent rather than a second KeyPressedEvent: the Input snapshot
                    // ignores that type, keeping every edge query one-per-physical-press, and a
                    // consumer opts in where repetition is meaningful.
                    window->m_Events.push_back(CreateUnique<KeyRepeatEvent>(code, scancode, mods));
                }
            });

        glfwSetCharCallback(m_Handle,
                            [](GLFWwindow* glfwWindow, unsigned int codepoint)
                            {
                                auto window =
                                    static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                                window->m_Events.push_back(CreateUnique<KeyTypedEvent>(codepoint));
                            });

        glfwSetMouseButtonCallback(
            m_Handle,
            [](GLFWwindow* glfwWindow, int button, int action, int mods)
            {
                auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                const auto code = static_cast<MouseButton>(button);
                if (action == GLFW_PRESS)
                {
                    window->m_Events.push_back(CreateUnique<MouseButtonPressedEvent>(code, mods));
                }
                else if (action == GLFW_RELEASE)
                {
                    window->m_Events.push_back(CreateUnique<MouseButtonReleasedEvent>(code, mods));
                }
            });

        glfwSetCursorPosCallback(
            m_Handle,
            [](GLFWwindow* glfwWindow, f64 xpos, f64 ypos)
            {
                auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                const vec2 position{static_cast<f32>(xpos), static_cast<f32>(ypos)};
                window->m_MousePosition = position;
                window->m_Events.push_back(CreateUnique<MouseMovedEvent>(position));
            });

        glfwSetCursorEnterCallback(
            m_Handle,
            [](GLFWwindow* glfwWindow, int entered)
            {
                auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                window->m_Events.push_back(CreateUnique<MouseEnteredEvent>(entered == GLFW_TRUE));
            });

        glfwSetScrollCallback(
            m_Handle,
            [](GLFWwindow* glfwWindow, f64 xoffset, f64 yoffset)
            {
                auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
                const vec2 offset{static_cast<f32>(xoffset), static_cast<f32>(yoffset)};
                window->m_ScrollDelta += offset;
                window->m_Events.push_back(CreateUnique<MouseScrolledEvent>(offset));
            });

        // The joystick callback is a GLFW global; route its connect/disconnect through this window.
        s_JoystickEventWindow = this;
        glfwSetJoystickCallback(
            [](int jid, int event)
            {
                if (s_JoystickEventWindow == nullptr)
                {
                    return;
                }
                const auto id = static_cast<GamepadId>(jid);
                if (event == GLFW_CONNECTED)
                {
                    s_JoystickEventWindow->m_Events.push_back(
                        CreateUnique<GamepadConnectedEvent>(id));
                }
                else if (event == GLFW_DISCONNECTED)
                {
                    s_JoystickEventWindow->m_Events.push_back(
                        CreateUnique<GamepadDisconnectedEvent>(id));
                }
            });

        {
            int width, height;
            glfwGetFramebufferSize(m_Handle, &width, &height);
            m_Extent = {static_cast<u32>(width), static_cast<u32>(height)};
        }

        if (m_MouseCaptured)
        {
            CaptureMouse();
        }

        // Remember the windowed rectangle so a later return from fullscreen restores it.
        {
            int x = 0;
            int y = 0;
            glfwGetWindowPos(m_Handle, &x, &y);
            m_WindowedPosition = {x, y};
            int w = 0;
            int h = 0;
            glfwGetWindowSize(m_Handle, &w, &h);
            m_WindowedExtent = {static_cast<u32>(w), static_cast<u32>(h)};
        }

        // A non-windowed initial mode moves the freshly created window onto its monitor.
        if (info.Fullscreen != FullscreenMode::Windowed)
        {
            ApplyDisplayMode(info.Fullscreen, info.MonitorId, uvec2{0, 0}, 0);
        }
    }

    Window::Native& Window::GetNative() const
    {
        return *m_Native;
    }

    void Window::CreateSurface(const Renderer::Context& context)
    {
        VkSurfaceKHR surface;
        VK_RAW_ASSERT(glfwCreateWindowSurface(GetVkInstance(context), m_Handle, nullptr, &surface),
                      "Failed to create window surface!");
        m_Native->Surface = surface;
    }

    void Window::CaptureMouse()
    {
        m_MouseCaptured = true;

        glfwSetInputMode(m_Handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        if (glfwRawMouseMotionSupported())
        {
            glfwSetInputMode(m_Handle, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
    }

    void Window::ReleaseMouse()
    {
        m_MouseCaptured = false;

        if (glfwRawMouseMotionSupported())
        {
            glfwSetInputMode(m_Handle, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        }
        glfwSetInputMode(m_Handle, GLFW_CURSOR,
                         m_CursorVisible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
    }

    bool Window::IsMouseCaptured() const
    {
        return m_MouseCaptured;
    }

    void Window::SetCursorVisible(const bool visible)
    {
        if (m_CursorVisible == visible)
        {
            return;
        }
        m_CursorVisible = visible;

        // A captured cursor is already hidden and locked; the flag applies on release.
        if (!m_MouseCaptured)
        {
            glfwSetInputMode(m_Handle, GLFW_CURSOR,
                             visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
        }
    }

    bool Window::IsCursorVisible() const
    {
        return m_CursorVisible;
    }

    void Window::Update()
    {
        glfwPollEvents();

        if (ShouldClose())
        {
            Close();
        }
    }

    void Window::DrainEvents(const std::function<void(Event&)>& handler)
    {
        for (const Unique<Event>& event : m_Events)
        {
            handler(*event);
        }
        m_Events.clear();
    }

    void Window::Close()
    {
        if (!m_Open)
        {
            return;
        }

        m_Open = false;
    }

    // veng is single-window, so window destruction terminates GLFW.
    Window::~Window()
    {
        if (s_JoystickEventWindow == this)
        {
            glfwSetJoystickCallback(nullptr);
            s_JoystickEventWindow = nullptr;
        }

        if (m_Handle)
        {
            glfwDestroyWindow(m_Handle);
            m_Handle = nullptr;
        }

        glfwTerminate();
        s_GlfwInitialized = false;
    }

    void Window::PollGamepads(const std::span<GamepadState> states) const
    {
        for (usize slot = 0; slot < states.size(); ++slot)
        {
            GamepadState& state = states[slot];
            state = GamepadState{};

            const auto jid = static_cast<int>(slot);
            GLFWgamepadstate raw;
            if (glfwJoystickPresent(jid) != GLFW_TRUE ||
                glfwGetGamepadState(jid, &raw) != GLFW_TRUE)
            {
                continue;
            }

            state.Connected = true;
            for (usize button = 0; button < state.Buttons.size(); ++button)
            {
                state.Buttons[button] = raw.buttons[GamepadButtonToGlfw[button]] == GLFW_PRESS;
            }
            for (usize axis = 0; axis < state.Axes.size(); ++axis)
            {
                state.Axes[axis] = raw.axes[GamepadAxisToGlfw[axis]];
            }
        }
    }

    void Window::SpinUntilValidSize()
    {
        while (m_Extent.x == 0 || m_Extent.y == 0)
        {
            int width, height;
            glfwGetFramebufferSize(m_Handle, &width, &height);
            m_Extent = {static_cast<u32>(width), static_cast<u32>(height)};
            glfwWaitEvents();
        }
    }

    bool Window::IsMinimized() const
    {
        return glfwGetWindowAttrib(m_Handle, GLFW_ICONIFIED) == GLFW_TRUE;
    }

    bool Window::ConsumeFramebufferResized()
    {
        const bool resized = m_FramebufferResized;
        m_FramebufferResized = false;
        return resized;
    }

    bool Window::ShouldClose() const
    {
        return glfwWindowShouldClose(m_Handle);
    }

    bool Window::KeyPressed(const Key key) const
    {
        // glfwGetKey raises GLFW_INVALID_ENUM (a fatal error through our callback) for
        // any code outside [GLFW_KEY_SPACE, GLFW_KEY_LAST]; Input sweeps every key slot,
        // so reject the gaps and out-of-range slots here rather than at the call site.
        const auto code = static_cast<i32>(key);
        if (code < GLFW_KEY_SPACE || code > GLFW_KEY_LAST)
        {
            return false;
        }
        return glfwGetKey(m_Handle, code) == GLFW_PRESS;
    }

    bool Window::MouseButtonPressed(const MouseButton button) const
    {
        return glfwGetMouseButton(m_Handle, static_cast<i32>(button)) == GLFW_PRESS;
    }

    vec2 Window::ConsumeScrollDelta()
    {
        const vec2 delta = m_ScrollDelta;
        m_ScrollDelta = {0, 0};
        return delta;
    }

    uvec2 Window::GetExtent() const
    {
        return m_Extent;
    }

    vec2 Window::GetContentScale() const
    {
        // The platform backing/DPI factor, read straight from the window rather than derived as the
        // framebuffer/window-size ratio: a native (Cocoa) fullscreen toggle moves the framebuffer and
        // the window size through separate, briefly-inconsistent updates, so the ratio reads ~1 across
        // the transition while the backing factor stays the display's true scale.
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetWindowContentScale(m_Handle, &scaleX, &scaleY);
        if (scaleX <= 0.0f || scaleY <= 0.0f)
        {
            return {1.0f, 1.0f};
        }
        return {scaleX, scaleY};
    }

    u32 Window::GetWidth() const
    {
        return m_Extent.x;
    }

    u32 Window::GetHeight() const
    {
        return m_Extent.y;
    }

    bool Window::IsOpen() const
    {
        return m_Open;
    }

    vec2 Window::GetMousePosition() const
    {
        return m_MousePosition;
    }

    void Window::SetTitle(const string& title)
    {
        m_Title = title;

        glfwSetWindowTitle(m_Handle, title.c_str());
    }

    string Window::GetTitle() const
    {
        return m_Title;
    }

    vector<MonitorInfo> Window::EnumerateMonitors()
    {
        if (!s_GlfwInitialized)
        {
            return {};
        }

        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        vector<MonitorInfo> result;
        result.reserve(static_cast<usize>(count));

        for (int i = 0; i < count; ++i)
        {
            GLFWmonitor* monitor = monitors[i];
            MonitorInfo info;
            // GLFW returns the primary monitor first, so the array index is the stable MonitorId
            // the persisted selection keys on.
            info.MonitorId = static_cast<u32>(i);
            if (const char* name = glfwGetMonitorName(monitor))
            {
                info.Name = name;
            }

            // GLFW reports video modes in screen coordinates (points); the swapchain and every
            // resolution field are in backing pixels, so each mode scales up by the monitor's content
            // scale to report true pixel resolutions. ApplyDisplayMode divides back to points for the
            // GLFW sizing calls, which take screen coordinates.
            float scaleX = 1.0f;
            float scaleY = 1.0f;
            glfwGetMonitorContentScale(monitor, &scaleX, &scaleY);
            scaleX = scaleX > 0.0f ? scaleX : 1.0f;
            scaleY = scaleY > 0.0f ? scaleY : 1.0f;
            const auto toPixels = [scaleX, scaleY](const int w, const int h)
            {
                return uvec2{static_cast<u32>(static_cast<f32>(w) * scaleX + 0.5f),
                             static_cast<u32>(static_cast<f32>(h) * scaleY + 0.5f)};
            };

            if (const GLFWvidmode* current = glfwGetVideoMode(monitor))
            {
                info.CurrentResolution = toPixels(current->width, current->height);
                info.CurrentRefreshRateHz = static_cast<u32>(current->refreshRate);
            }

            int modeCount = 0;
            const GLFWvidmode* modes = glfwGetVideoModes(monitor, &modeCount);
            vector<DisplayVideoMode> raw;
            raw.reserve(static_cast<usize>(modeCount));
            for (int k = 0; k < modeCount; ++k)
            {
                raw.push_back(
                    DisplayVideoMode{.Resolution = toPixels(modes[k].width, modes[k].height),
                                     .RefreshRateHz = static_cast<u32>(modes[k].refreshRate)});
            }
            info.Modes = DedupVideoModes(raw);

            result.push_back(std::move(info));
        }

        return result;
    }

    namespace
    {
        // Screen coordinates (points) for the GLFW sizing calls from a pixel resolution: the monitor
        // enumeration reports true backing pixels, so a list-selected resolution divides back by the
        // target display's content scale here. A zero or unit scale leaves it unchanged.
        uvec2 ScreenCoordsFromPixels(const uvec2 pixels, const float scaleX, const float scaleY)
        {
            const float sx = scaleX > 0.0f ? scaleX : 1.0f;
            const float sy = scaleY > 0.0f ? scaleY : 1.0f;
            return {static_cast<u32>(static_cast<float>(pixels.x) / sx + 0.5f),
                    static_cast<u32>(static_cast<float>(pixels.y) / sy + 0.5f)};
        }
    }

    void Window::ApplyDisplayMode(const FullscreenMode mode, const u32 monitorId,
                                  const uvec2 resolution, const u32 refreshHz)
    {
#if defined(__APPLE__)
        // macOS drives full-screen through native Cocoa (-[NSWindow toggleFullScreen:], a separate
        // Space) — the same state the green title-bar button toggles, so the settings menu and the
        // button can never disagree. Borderless is the one fullscreen choice here (Exclusive resolves
        // to it upstream); monitorId/refreshHz do not apply to a native toggle.
        (void)monitorId;
        (void)refreshHz;
        if (mode == FullscreenMode::Windowed)
        {
            // Cocoa restores the pre-fullscreen frame on exit, so no windowed-rectangle bookkeeping
            // is needed; a windowed resolution request then resizes the restored window.
            SetNativeFullscreen(m_Handle, false);
            if (resolution != uvec2{0, 0})
            {
                float sx = 1.0f;
                float sy = 1.0f;
                glfwGetWindowContentScale(m_Handle, &sx, &sy);
                const uvec2 points = ScreenCoordsFromPixels(resolution, sx, sy);
                glfwSetWindowSize(m_Handle, static_cast<int>(points.x), static_cast<int>(points.y));
            }
        }
        else
        {
            SetNativeFullscreen(m_Handle, true);
        }
        // The toggle is animated/asynchronous, so read the real state back rather than assuming it
        // took; GetFullscreenMode also queries it live, so a green-button change stays reflected.
        m_Fullscreen =
            IsNativeFullscreen(m_Handle) ? FullscreenMode::Borderless : FullscreenMode::Windowed;
        return;
#else
        if (mode == FullscreenMode::Windowed)
        {
            // Returning to (or staying) windowed: detach from any monitor and restore the remembered
            // windowed rectangle, resizing to the requested resolution when one was named. A selected
            // resolution is in backing pixels and divides back to points; the remembered extent was
            // captured from glfwGetWindowSize and is already points.
            uvec2 size = m_WindowedExtent;
            if (resolution != uvec2{0, 0})
            {
                float sx = 1.0f;
                float sy = 1.0f;
                glfwGetWindowContentScale(m_Handle, &sx, &sy);
                size = ScreenCoordsFromPixels(resolution, sx, sy);
            }
            const int width = size.x != 0 ? static_cast<int>(size.x) : static_cast<int>(m_Extent.x);
            const int height =
                size.y != 0 ? static_cast<int>(size.y) : static_cast<int>(m_Extent.y);
            glfwSetWindowMonitor(m_Handle, nullptr, m_WindowedPosition.x, m_WindowedPosition.y,
                                 width, height, GLFW_DONT_CARE);
            m_Fullscreen = FullscreenMode::Windowed;
            return;
        }

        // Capture the windowed rectangle before leaving it, so a later return restores it.
        if (m_Fullscreen == FullscreenMode::Windowed)
        {
            int x = 0;
            int y = 0;
            glfwGetWindowPos(m_Handle, &x, &y);
            m_WindowedPosition = {x, y};
            int w = 0;
            int h = 0;
            glfwGetWindowSize(m_Handle, &w, &h);
            m_WindowedExtent = {static_cast<u32>(w), static_cast<u32>(h)};
        }

        int monitorCount = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
        if (monitorCount == 0)
        {
            Log::Warn("ApplyDisplayMode: no monitors connected; staying windowed");
            return;
        }
        const int index =
            monitorId < static_cast<u32>(monitorCount) ? static_cast<int>(monitorId) : 0;
        GLFWmonitor* monitor = monitors[index];
        const GLFWvidmode* current = glfwGetVideoMode(monitor);

        // Borderless takes the monitor's current mode; Exclusive takes the requested mode/refresh.
        const bool useCurrentMode = mode == FullscreenMode::Borderless;

        int width = current != nullptr ? current->width : static_cast<int>(m_Extent.x);
        int height = current != nullptr ? current->height : static_cast<int>(m_Extent.y);
        int refresh = current != nullptr ? current->refreshRate : GLFW_DONT_CARE;
        if (!useCurrentMode)
        {
            // Exclusive on a platform that supports it: take the requested mode/refresh, native
            // (the monitor's current mode) where a field is left zero. The requested resolution is in
            // backing pixels and divides back to the points the video-mode match works in.
            if (resolution.x != 0 && resolution.y != 0)
            {
                float sx = 1.0f;
                float sy = 1.0f;
                glfwGetMonitorContentScale(monitor, &sx, &sy);
                const uvec2 points = ScreenCoordsFromPixels(resolution, sx, sy);
                width = static_cast<int>(points.x);
                height = static_cast<int>(points.y);
            }
            if (refreshHz != 0)
            {
                refresh = static_cast<int>(refreshHz);
            }
        }

        glfwSetWindowMonitor(m_Handle, monitor, 0, 0, width, height, refresh);
        m_Fullscreen = mode == FullscreenMode::Exclusive && !useCurrentMode
                           ? FullscreenMode::Exclusive
                           : FullscreenMode::Borderless;

        // The single VkSurfaceKHR (created once in CreateSurface) is not recreated here: the surface
        // wraps the window's swapchain-backing layer, which persists across a monitor move, so moving
        // the same window between monitors does not invalidate it. A platform whose surface does not
        // survive a monitor switch would need surface recreation added to this path.
#endif
    }

    FullscreenMode Window::GetFullscreenMode() const
    {
#if defined(__APPLE__)
        // macOS full-screen is the native Cocoa state, which the green title-bar button can change
        // behind us, so query it live rather than trusting the cached value.
        return IsNativeFullscreen(m_Handle) ? FullscreenMode::Borderless : FullscreenMode::Windowed;
#else
        return m_Fullscreen;
#endif
    }

    Unique<Window> Window::Create(const WindowInfo& info)
    {
        return CreateUnique<Window>(info);
    }

    namespace
    {
        // Borrows FileDialogFilter strings, which outlive the synchronous dialog call.
        vector<nfdu8filteritem_t> ToNfdFilters(const vector<FileDialogFilter>& filters)
        {
            vector<nfdu8filteritem_t> items;
            items.reserve(filters.size());
            for (const auto& filter : filters)
            {
                items.push_back({filter.Name.c_str(), filter.Extensions.c_str()});
            }
            return items;
        }
    }

    bool Window::OpenFileDialog(string& outSelectedPath, const string& defaultPath,
                                const vector<FileDialogFilter>& filters)
    {
        const auto items = ToNfdFilters(filters);
        nfdu8char_t* outPath = nullptr;
        nfdopendialogu8args_t args = {};
        args.filterCount = static_cast<nfdfiltersize_t>(items.size());
        args.filterList = items.data();
        args.defaultPath = defaultPath.c_str();
        const nfdresult_t result = NFD_OpenDialogU8_With(&outPath, &args);

        if (result == NFD_OKAY)
        {
            outSelectedPath = outPath;
            NFD_FreePathU8(outPath);

            return true;
        }

        if (result != NFD_CANCEL)
        {
            Log::Error("Error: {0}", NFD_GetError());
        }

        return false;
    }

    bool Window::SaveFileDialog(string& outSelectedPath, const string& defaultPath,
                                const vector<FileDialogFilter>& filters)
    {
        const auto items = ToNfdFilters(filters);
        nfdu8char_t* outPath = nullptr;
        nfdsavedialogu8args_t args = {};
        args.filterCount = static_cast<nfdfiltersize_t>(items.size());
        args.filterList = items.data();
        args.defaultPath = defaultPath.c_str();
        args.defaultName = "Untitled";
        const nfdresult_t result = NFD_SaveDialogU8_With(&outPath, &args);

        if (result == NFD_OKAY)
        {
            outSelectedPath = outPath;
            NFD_FreePathU8(outPath);

            return true;
        }

        if (result != NFD_CANCEL)
        {
            Log::Error("Error: {0}", NFD_GetError());
        }

        return false;
    }
}
