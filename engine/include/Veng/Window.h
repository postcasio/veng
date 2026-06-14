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

    struct WindowInfo
    {
        uvec2 Extent;
        bool Resizable;
        EventCallback EventCallback;
        string Title;
        bool CaptureMouse;
    };

    // A file-dialog filter, e.g. {"Images", "png,jpg"}. Replaces the nfd type in
    // the public signature; the nfd mapping lives in Window.cpp.
    struct FileDialogFilter
    {
        string Name;
        string Extensions;
    };

    class Window
    {
    public:
        // Initializes GLFW (first window only) and creates the native window.
        // The window exists independently of any rendering context; the context
        // borrows it and calls CreateSurface during its own initialization.
        explicit Window(const WindowInfo& info);

        static Unique<Window> Create(const WindowInfo& info);
        static bool OpenFileDialog(string& outSelectedPath, const string& defaultPath,
                                   const vector<FileDialogFilter>& filters);
        static bool SaveFileDialog(string& outSelectedPath, const string& defaultPath,
                                   const vector<FileDialogFilter>& filters);

        // Destroys the native window and terminates GLFW. The surface created
        // by CreateSurface is owned and destroyed by the context, so the
        // context must be disposed before the window is destroyed.
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void CreateSurface(const Renderer::Context& context);
        void SpinUntilValidSize();

        [[nodiscard]] bool IsMinimized() const;

        void CaptureMouse();
        void ReleaseMouse();
        [[nodiscard]] bool IsMouseCaptured() const;
        void Update();
        // Requests main-loop exit (IsOpen() becomes false); does not destroy anything.
        void Close();
        [[nodiscard]] bool ShouldClose() const;
        [[nodiscard]] bool KeyPressed(Key key) const;

        [[nodiscard]] uvec2 GetExtent() const;

        [[nodiscard]] u32 GetWidth() const;

        [[nodiscard]] u32 GetHeight() const;

        [[nodiscard]] bool IsOpen() const;

        [[nodiscard]] vec2 GetMousePosition() const;

        void SetTitle(const string& title);
        [[nodiscard]] string GetTitle() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

        // GLFWwindow* is kept as an incomplete-type pointer member rather than
        // moved into Native, since GLFW is a free function API that doesn't
        // need a Vulkan-style handle wrapper. Exposed via the escape hatch.
        friend GLFWwindow* GetGlfwWindow(const Window& window);

    private:
        bool m_Open = true;
        uvec2 m_Extent{};
        bool m_Resizable;
        std::function<void(Event&)> m_EventCallback;
        string m_Title;
        bool m_MouseCaptured;
        GLFWwindow* m_Handle = nullptr;
        vec2 m_MousePosition = {0, 0};

        Unique<Native> m_Native;
    };
}
