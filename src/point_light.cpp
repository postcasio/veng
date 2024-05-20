#include "point_light.h"
#include "object.h"

PointLight::PointLight()
    : Object(nullptr)
{
  type = ObjectType::PointLight;

  color = glm::vec3(1.0f, 1.0f, 1.0f);
  intensity = 1.0f;
  constant = 1.0f;
  linear = 0.00001f;
  quadratic = 0.0000000005f;
  radius = 10.0f;
}