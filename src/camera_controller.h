#ifndef _CAMERA_CONTROLLER_H
#define _CAMERA_CONTROLLER_H

#include "gfxcommon.h"

#include "window.h"
#include "camera.h"

class CameraController
{
public:
    Camera *camera;
    Window *window;

    CameraController(Camera *camera, Window *window);
    ~CameraController();

    void updateCamera();
    void setCamera(Camera *camera);

private:
    float lon = 0.0f;
    float lat = 0.0f;
};

#endif