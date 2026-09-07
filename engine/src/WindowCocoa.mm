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
        // A native fullscreen Space is held only by a window allowed to take a primary fullscreen
        // Space; a programmatic toggle on a window without that behavior enters the Space and is
        // immediately bounced back to the previous one. The green title-bar button sets this up
        // itself, so a toggle driven from code has to assert it too.
        window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;

        const bool isFullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (isFullscreen == fullscreen)
        {
            return;
        }
        // Entering fullscreen: the Space is dismissed the moment it opens unless the window is the
        // key window of the active application, so make it so before the transition rather than
        // relying on the toggle to foreground a background app's window.
        if (fullscreen)
        {
            [NSApp activateIgnoringOtherApps:YES];
            [window makeKeyAndOrderFront:nil];
        }
        [window toggleFullScreen:nil];
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
