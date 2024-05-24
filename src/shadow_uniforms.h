#ifndef _SHADOW_UNIFORMS_H_
#define _SHADOW_UNIFORMS_H_

#include "gfxcommon.h"
#include <vector>

#include "material.h"

struct ShadowUniformBufferObject
{
    alignas(16) glm::mat4 views[6];
    alignas(16) glm::mat4 projection;
    alignas(16) glm::mat4 model;
    alignas(16) glm::vec3 lightPos;
    alignas(4) float farPlane;
    alignas(4) int32_t layer;
};

class ShadowUniforms
{
public:
    ShadowUniforms();
    ~ShadowUniforms();

    void updateUniformBuffer(uint32_t currentFrame);
    ShadowUniformBufferObject ubo;
    std::vector<std::unique_ptr<BufferAllocation>> uniformBufferAllocations;

private:
};

#endif
