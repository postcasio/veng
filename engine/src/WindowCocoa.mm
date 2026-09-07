#include "WindowCocoa.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>

namespace Veng
{
    void SetNativeFullscreen(GLFWwindow* handle, bool fullscreen)
    {
        NSWindow* window = glfwGetCocoaWindow(handle);
        if (window == nil)
        {
            return;
        }
        const bool isFullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (isFullscreen != fullscreen)
        {
            [window toggleFullScreen:nil];
        }
    }

    bool IsNativeFullscreen(GLFWwindow* handle)
    {
        NSWindow* window = glfwGetCocoaWindow(handle);
        if (window == nil)
        {
            return false;
        }
        return (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    }
}
