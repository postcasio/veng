#ifndef _LIGHTING_UNIFORMS_H_
#define _LIGHTING_UNIFORMS_H_

#include "gfxcommon.h"
#include <vector>

#include "material.h"

struct PointLightUniformBufferObject
{
    alignas(16) glm::vec3 position;
    alignas(16) glm::vec3 color;
    alignas(4) float intensity;
    alignas(4) float constant;
    alignas(4) float linear;
    alignas(4) float quadratic;
    alignas(4) float radius;
};

struct LightingUniformBufferObject
{
    PointLightUniformBufferObject pointLights[4];
    alignas(4) uint32_t pointLightCount;
};

class LightingUniforms
{
public:
    LightingUniforms();
    ~LightingUniforms();

    void updateUniformBuffer(uint32_t currentImage);
    LightingUniformBufferObject ubo;
    std::vector<std::unique_ptr<BufferAllocation>> uniformBufferAllocations;

private:
};

#endif
