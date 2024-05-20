#include "perspective_camera.h"

PerspectiveCamera::PerspectiveCamera(float fov, float aspect, float near, float far)
    : fov(fov), aspect(aspect), near(near), far(far)
{
    updateProjectionMatrix();
}

PerspectiveCamera::~PerspectiveCamera()
{
}

void PerspectiveCamera::updateProjectionMatrix()
{
    projectionMatrix = glm::perspective(glm::radians(fov), aspect, near, far);
    projectionMatrix[1][1] *= -1;
}