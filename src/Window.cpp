#include <Veng/Window.h>
#include <Veng/WindowEvents.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/Context.h>
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

    Window::Window(const WindowInfo& info):
        m_Extent(info.Extent),
        m_Resizable(info.Resizable),
        m_EventCallback(info.EventCallback),
        m_Title(info.Title),
        m_MouseCaptured(info.CaptureMouse)
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

        m_Handle = glfwCreateWindow(m_Extent.x, m_Extent.y, m_Title.c_str(), nullptr, nullptr);

        if (!m_Handle)
        {
            VE_ASSERT(false, "Failed to create window!");
        }

        glfwSetWindowUserPointer(m_Handle, this);
        glfwSetFramebufferSizeCallback(m_Handle, [](GLFWwindow* glfwWindow, int width, int height)
        {
            auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));

            WindowResizeEvent event(width, height);
            window->m_EventCallback(event);
        });

        glfwSetWindowCloseCallback(m_Handle, [](GLFWwindow* glfwWindow)
        {
            auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
            WindowCloseEvent event;
            window->m_EventCallback(event);
        });

        glfwSetCursorPosCallback(m_Handle, [](GLFWwindow* glfwWindow, f64 xpos, f64 ypos)
        {
            auto window = static_cast<Window*>(glfwGetWindowUserPointer(glfwWindow));
            window->m_MousePosition = {xpos, ypos};
        });

        glfwGetFramebufferSize(m_Handle, &m_Extent.x, &m_Extent.y);

        if (m_MouseCaptured)
        {
            CaptureMouse();
        }
    }

    void Window::CreateSurface(const Renderer::Context& context)
    {
        VK_RAW_ASSERT(glfwCreateWindowSurface(context.GetVkInstance(), m_Handle, nullptr, &m_Surface),
                      "Failed to create window surface!");
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

    void Window::Close()
    {
        if (!m_Open)
        {
            return;
        }

        m_Open = false;
    }

    // veng is single-window for now, so window destruction terminates GLFW.
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
            glfwGetFramebufferSize(m_Handle, &m_Extent.x, &m_Extent.y);
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
        return glfwGetKey(m_Handle, static_cast<i32>(key)) == GLFW_PRESS;
    }

    VkSurfaceKHR Window::GetSurface() const
    {
        return m_Surface;
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

    GLFWwindow* Window::GetHandle() const
    {
        return m_Handle;
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
        // Build the nfd filter array, borrowing the FileDialogFilter strings
        // (which outlive the dialog call).
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
