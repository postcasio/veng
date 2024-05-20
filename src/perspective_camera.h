#ifndef _PERSPECTIVE_CAMERA_H_
#define _PERSPECTIVE_CAMERA_H_

#include "camera.h"

class PerspectiveCamera : public Camera
{
public:
    PerspectiveCamera(float fov, float aspect, float near, float far);
    ~PerspectiveCamera();

    float fov;
    float aspect;
    float near;
    float far;

    void updateProjectionMatrix();
};

#endif