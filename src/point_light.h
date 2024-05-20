#ifndef _POINT_LIGHT_H_
#define _POINT_LIGHT_H_

#include "gfxcommon.h"
#include "object.h"

class PointLight : public Object
{
public:
    PointLight();

    glm::vec3 color;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float radius;
};

#endif