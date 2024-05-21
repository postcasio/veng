#ifndef _DESCRIPTOR_POOL_H_
#define _DESCRIPTOR_POOL_H_

#include "../gfxcommon.h"
#include "descriptor_set_layout.h"
#include "logical_device.h"

class DescriptorSet;

class DescriptorPool
{
public:
    DescriptorPool(LogicalDevice &device, uint32_t uniformBufferCount, uint32_t imageSamplerCount, uint32_t descriptorSetCount);
    ~DescriptorPool();

    std::unique_ptr<DescriptorSet> createDescriptorSet(DescriptorSetLayout &layout);
    void destroyDescriptorSets(std::vector<VkDescriptorSet> sets);

    VkDescriptorPool pool;
    VkDescriptorPoolCreateInfo descriptorPoolCreateInfo{};

    LogicalDevice &device;
    uint32_t uniformBufferCount;
    uint32_t imageSamplerCount;
    uint32_t descriptorSetCount;
};

#endif