#include "camera_controller.h"

#include <algorithm>
#include <iostream>

CameraController::CameraController(Camera *camera, Window *window)
{
    this->camera = camera;
    this->window = window;
}

CameraController::~CameraController()
{
}

void CameraController::updateCamera()
{

    auto mouseState = window->getMouseState();
    float lookSpeed = 0.5f;
    float mouseX = mouseState.position.x;
    float mouseY = mouseState.position.y;

    lon -= mouseX * lookSpeed;
    lat -= mouseY * lookSpeed;

    lat = std::max(-85.0f, std::min(85.0f, this->lat));

    float phi = glm::radians(90 - this->lat);
    float theta = glm::radians(this->lon);

    glm::vec3 position = this->camera->position;

    glm::vec3 targetPosition;
    targetPosition.x = sin(phi) * cos(theta);
    targetPosition.y = cos(phi);
    targetPosition.z = sin(phi) * sin(theta);

    targetPosition += position;

    camera->lookAt(targetPosition);

    keyboard_state keyboardState = window->getKeyboardState();

    float speed = 2.0f;

    if (keyboardState.keys.w)
    {
        camera->translateZ(-speed);
    }
    if (keyboardState.keys.s)
    {
        camera->translateZ(speed);
    }
    if (keyboardState.keys.a)
    {
        camera->translateX(-speed);
    }
    if (keyboardState.keys.d)
    {
        camera->translateX(speed);
    }
}
