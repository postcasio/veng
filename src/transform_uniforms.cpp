#include "gfxcommon.h"

#include <chrono>
#include <iostream>

#include "engine.h"
#include "transform_uniforms.h"
#include "object.h"
#include "camera.h"

TransformUniforms::TransformUniforms()
{
    VkDeviceSize bufferSize = sizeof(TransformUniformBufferObject);

    uniformBufferAllocations.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        uniformBufferAllocations[i] = std::make_unique<BufferAllocation>(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

TransformUniforms::~TransformUniforms()
{
    uniformBufferAllocations.clear();
}

void TransformUniforms::updateUniformBuffer(uint32_t currentFrame)
{
    vmaCopyMemoryToAllocation(renderer()->allocator, &ubo, uniformBufferAllocations[currentFrame]->allocation, 0, sizeof(ubo));
}
