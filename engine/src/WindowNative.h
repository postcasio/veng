#pragma once

// The window's backend state, shared by Window.cpp and the GPU backend. It holds the presentation
// surface the backend created for the window as an opaque handle, so the window names no graphics
// API; the render context owns the surface and destroys it.

#include <Veng/Veng.h>
#include <Veng/Window.h>

struct GLFWwindow;

namespace Veng
{
    struct Window::Native
    {
        /// @brief The backend's presentation surface for the window, opaque here; zero until
        ///        Window::CreateSurface.
        u64 PresentationSurface = 0;
    };
}

namespace Veng::Renderer
{
    class Context;

    /// @brief Creates the backend's presentation surface for a window on a context.
    ///
    /// Implemented by the backend compiled in; the context takes ownership of the surface.
    /// @param context The context the surface presents through.
    /// @param window  The window's platform handle.
    /// @return The surface, as an opaque handle.
    [[nodiscard]] u64 CreatePresentationSurface(const Context& context, GLFWwindow* window);
}
