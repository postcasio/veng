#ifndef _DESCRIPTOR_POOL_H_
#define _DESCRIPTOR_POOL_H_

#include "../gfxcommon.h"
#include "descriptor_set_layout.h"

class DescriptorSet;

class DescriptorPool
{
public:
    DescriptorPool(uint32_t uniformBufferCount, uint32_t imageSamplerCount, uint32_t descriptorSetCount);
    ~DescriptorPool();

    DescriptorSet *createDescriptorSet(DescriptorSetLayout &layout);
    void freeDescriptorSet(DescriptorSet *set);

    VkDescriptorPool pool;
    VkDescriptorPoolCreateInfo descriptorPoolCreateInfo{};
    std::vector<std::unique_ptr<DescriptorSet>> sets;

    uint32_t uniformBufferCount;
    uint32_t imageSamplerCount;
    uint32_t descriptorSetCount;
};

#endif