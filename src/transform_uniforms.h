#ifndef _TRANSFORM_UNIFORMS_H_
#define _TRANSFORM_UNIFORMS_H_

#include "gfxcommon.h"
#include <vector>

#include "material.h"

struct TransformUniformBufferObject
{
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::mat3 normal;
    alignas(16) glm::vec3 cameraPos;
};

class TransformUniforms
{
public:
    TransformUniforms();
    ~TransformUniforms();

    void updateUniformBuffer(uint32_t currentFrame);
    TransformUniformBufferObject ubo;
    std::vector<std::unique_ptr<BufferAllocation>> uniformBufferAllocations;

private:
};

#endif
