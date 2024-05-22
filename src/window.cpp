#include "window.h"
#include "engine.h"

#include <iostream>

const uint32_t INITIAL_WIDTH = 1280;
const uint32_t INITIAL_HEIGHT = 720;

mouse_state mouseState;
keyboard_state keyboardState;

static void framebufferResizeCallback(GLFWwindow *window, int width, int height)
{
    auto app = reinterpret_cast<Engine *>(glfwGetWindowUserPointer(window));
    app->renderer->framebufferResized = true;
}

static void setCursorPosCallback(GLFWwindow *window, double xpos, double ypos)
{
    mouseState.position = {-xpos, ypos};

    glfwSetCursorPos(window, 0, 0);
}

mouse_state Window::getMouseState()
{
    return mouseState;
}

void Window::setMouseState(mouse_state state)
{
    mouseState = state;
}

void Window::createWindow(void *engine)
{
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "Vulkan", nullptr, nullptr);
    glfwSetWindowUserPointer(window, engine);
    glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported())
    {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }

    glfwSetCursorPosCallback(window, setCursorPosCallback);
}

VkExtent2D Window::getExtent()
{
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);

    VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height)};

    return actualExtent;
}

void Window::pollEvents()
{
    glfwPollEvents();
}

bool Window::shouldClose()
{
    return glfwWindowShouldClose(window);
}

void Window::spinUntilValidSize()
{
    int width = 0, height = 0;
    while (width == 0 || height == 0)
    {
        glfwGetFramebufferSize(window, &width, &height);
        glfwWaitEvents();
    }
}

void Window::destroy()
{
    glfwDestroyWindow(window);

    glfwTerminate();
}

keyboard_state Window::getKeyboardState()
{
    return keyboardState;
}

void Window::updateKeyboardState()
{

    keyboardState.keys.w = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
    keyboardState.keys.a = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
    keyboardState.keys.s = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
    keyboardState.keys.d = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
    keyboardState.keys.q = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
    keyboardState.keys.e = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
    keyboardState.keys.space = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    keyboardState.keys.shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
}