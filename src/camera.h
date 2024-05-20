#ifndef _CAMERA_H_
#define _CAMERA_H_

#include "gfxcommon.h"
#include "object.h"

class Camera : public Object
{
public:
    Camera();
    ~Camera();

    glm::mat4 projectionMatrix;
    glm::mat4 viewMatrix;

    ObjectType type = ObjectType::Object;

    void updateProjectionMatrix();
};

#endif