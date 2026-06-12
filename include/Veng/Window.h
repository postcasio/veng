#pragma once

#include <functional>
#include <vector>

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Event.h>
#include "nfd.h"

#define GLFW_BOOL(x) ((x) ? GLFW_TRUE : GLFW_FALSE)

namespace Veng
{
    namespace Renderer
    {
        class Context;
    }

    struct WindowInfo
    {
        vec2 Extent;
        bool Resizable;
        EventCallback EventCallback;
        string Title;
        bool CaptureMouse;
    };

    class Window
    {
    public:
        explicit Window(const WindowInfo& info);

        static Unique<Window> Create(const WindowInfo& info);
        static bool OpenFileDialog(string* outSelectedPath, const string& defaultPath,
                                   const vector<nfdu8filteritem_t>& extensions);
        static bool SaveFileDialog(string& outSelectedPath, const string& defaultPath,
                            const vector<nfdu8filteritem_t>& extensions);

        ~Window() { Close(); }

        void Initialize(const Renderer::Context& context);
        void CreateSurface(const Renderer::Context& context);
        void SpinUntilValidSize();

        [[nodiscard]] bool IsMinimized() const;

        void CaptureMouse();
        void ReleaseMouse();
        [[nodiscard]] bool IsMouseCaptured() const;
        void Update();
        void Close();
        void Dispose() const;
        [[nodiscard]] bool ShouldClose() const;
        [[nodiscard]] bool KeyPressed(i32 key) const;

        [[nodiscard]] VkSurfaceKHR GetSurface() const;

        [[nodiscard]] uvec2 GetExtent() const;

        [[nodiscard]] u32 GetWidth() const;

        [[nodiscard]] u32 GetHeight() const;

        [[nodiscard]] GLFWwindow* GetHandle() const;

        [[nodiscard]] bool IsOpen() const;

        [[nodiscard]] vec2 GetMousePosition() const;

        void SetTitle(const string& title);
        [[nodiscard]] string GetTitle() const;

    protected:
    private:
        bool m_Open = true;
        bool m_Minimized = false;
        ivec2 m_Extent{};
        bool m_Resizable;
        std::function<void(Event&)> m_EventCallback;
        string m_Title;
        bool m_MouseCaptured;
        GLFWwindow* m_Handle = nullptr;
        VkSurfaceKHR m_Surface = nullptr;
        vec2 m_MousePosition = {0, 0};
    };
}
