#ifndef _WINDOW_H_
#define _WINDOW_H_

#include "gfxcommon.h"

struct mouse_state
{
    struct
    {
        bool left = false;
        bool right = false;
        bool middle = false;
    } buttons;
    glm::vec2 position;
};

struct keyboard_state
{
    struct
    {
        bool w = false;
        bool a = false;
        bool s = false;
        bool d = false;
        bool q = false;
        bool e = false;
        bool space = false;
        bool shift = false;
    } keys;
};

class Window
{
public:
    GLFWwindow *window;
    void createWindow(void *engine);
    VkExtent2D getExtent();
    bool shouldClose();
    void pollEvents();
    void spinUntilValidSize();
    mouse_state getMouseState();
    void setMouseState(mouse_state state);
    keyboard_state getKeyboardState();
    void updateKeyboardState();
    void destroy();
};

#endif