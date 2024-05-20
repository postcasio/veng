#include "gfxcommon.h"

#include <chrono>
#include <iostream>

#include "engine.h"
#include "lighting_uniforms.h"
#include "object.h"
#include "camera.h"

LightingUniforms::LightingUniforms()
{
    VkDeviceSize bufferSize = sizeof(LightingUniformBufferObject);

    uniformBufferAllocations.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        uniformBufferAllocations[i] = std::make_unique<BufferAllocation>(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

LightingUniforms::~LightingUniforms()
{
    uniformBufferAllocations.clear();
}

void LightingUniforms::updateUniformBuffer(uint32_t currentImage)
{
    // memcpy(&uniformBuffers[currentImage], &ubo, sizeof(ubo));
    vmaCopyMemoryToAllocation(renderer()->allocator, &ubo, uniformBufferAllocations[currentImage]->allocation, 0, sizeof(ubo));
}
