#include "camera.h"

Camera::Camera() : Object(nullptr)
{
    projectionMatrix = glm::mat4(1.0f);
    viewMatrix = glm::mat4(1.0f);
}

Camera::~Camera()
{
}

void Camera::updateProjectionMatrix()
{
}
