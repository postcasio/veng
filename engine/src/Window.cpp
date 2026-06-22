#include <Veng/Window.h>
#include <Veng/WindowEvents.h>
#include <Veng/InputEvents.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <nfd.h>

#define GLFW_BOOL(x) ((x) ? GLFW_TRUE : GLFW_FALSE)

namespace Veng
{
    namespace
    {
        bool s_GlfwInitialized = false;

        void GLFWErrorCallback(int err, const char* message)
        {
            VE_ASSERT(false, "GLFW error ({0}): {1}", err, message);
        }
    }

    Window::Window(const WindowInfo& info)
        : m_Extent(info.Extent), m_Resizable(info.Resizable), m_Title(info.Title),
          m_MouseCaptured(info.CaptureMouse), m_Native(CreateUnique<Native>())
    {
        if (!s_GlfwInitialized)
        {
            Log::Info("Initializing GLFW");

            glfwSetErrorCallback(GLFWErrorCallback);

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
                // GLFW_REPEAT carries no state change: the key stays down and the ImGui
                // backend tracks held state itself, so no event is produced for it.
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

        {
            int width, height;
            glfwGetFramebufferSize(m_Handle, &width, &height);
            m_Extent = {static_cast<u32>(width), static_cast<u32>(height)};
        }

        if (m_MouseCaptured)
        {
            CaptureMouse();
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
        glfwSetInputMode(m_Handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    bool Window::IsMouseCaptured() const
    {
        return m_MouseCaptured;
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
        if (m_Handle)
        {
            glfwDestroyWindow(m_Handle);
            m_Handle = nullptr;
        }

        glfwTerminate();
        s_GlfwInitialized = false;
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
