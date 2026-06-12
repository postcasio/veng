#include <Veng/Window.h>
#include <Veng/WindowEvents.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/Context.h>
#include <nfd.h>

namespace Veng
{
    void Window::Initialize(const Renderer::Context& context)
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_BOOL(m_Resizable));
        // glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

        Log::Info("Creating window: {0} ({1}x{2})", m_Title, m_Extent.x, m_Extent.y);

        m_Handle = glfwCreateWindow(m_Extent.x, m_Extent.y, m_Title.c_str(), nullptr, nullptr);

        if (!m_Handle)
        {
            throw std::runtime_error("Failed to create window!");
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

        // glfwSetCursorPosCallback(m_Window, setCursorPosCallback);
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

    void Window::Dispose() const
    {
        glfwDestroyWindow(m_Handle);
        glfwTerminate();
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
        return m_Minimized;
    }


    bool Window::ShouldClose() const
    {
        return glfwWindowShouldClose(m_Handle);
    }

    bool Window::KeyPressed(const i32 key) const
    {
        return glfwGetKey(m_Handle, key) == GLFW_PRESS;
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

    Window::Window(const WindowInfo& info):
        m_Extent(info.Extent),
        m_Resizable(info.Resizable),
        m_EventCallback(info.EventCallback),
        m_Title(info.Title),
        m_MouseCaptured(info.CaptureMouse)
    {
    }

    Unique<Window> Window::Create(const WindowInfo& info)
    {
        return CreateUnique<Window>(info);
    }

    bool Window::OpenFileDialog(string* outSelectedPath, const string& defaultPath,
                                const vector<nfdu8filteritem_t>& extensions)
    {
        nfdu8char_t* outPath;
        nfdopendialogu8args_t args;
        args.filterCount = static_cast<uint32_t>(extensions.size());
        args.filterList = extensions.data();
        args.defaultPath = defaultPath.c_str();
        nfdresult_t result = NFD_OpenDialogU8_With(&outPath, &args);

        if (result == NFD_OKAY)
        {
            *outSelectedPath = outPath;
            NFD_FreePathU8(outPath);

            return true;
        }
        else if (result == NFD_CANCEL)
        {
            return false;
        }
        else
        {
            Log::Error("Error: {0}", NFD_GetError());
            return false;
        }
    }

    bool Window::SaveFileDialog(string& outSelectedPath, const string& defaultPath,
                                const vector<nfdu8filteritem_t>& extensions)
    {
        nfdu8char_t* outPath;
        nfdsavedialogu8args_t args;
        args.filterCount = static_cast<uint32_t>(extensions.size());
        args.filterList = extensions.data();
        args.defaultPath = defaultPath.c_str();
        args.defaultName = "Untitled";
        nfdresult_t result = NFD_SaveDialogU8_With(&outPath, &args);

        if (result == NFD_OKAY)
        {
            outSelectedPath = outPath;
            NFD_FreePathU8(outPath);

            return true;
        }
        else if (result == NFD_CANCEL)
        {
            return false;
        }
        else
        {
            Log::Error("Error: {0}", NFD_GetError());
            return false;
        }
    }
}
