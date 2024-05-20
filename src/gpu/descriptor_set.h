#ifndef _GPU_DESCRIPTOR_SET_H_
#define _GPU_DESCRIPTOR_SET_H_

#include "../gfxcommon.h"
#include "descriptor_set_layout.h"
#include "descriptor_pool.h"

class DescriptorSet
{
public:
    DescriptorSet(DescriptorSetLayout &layout, DescriptorPool &pool);
    ~DescriptorSet();

    DescriptorPool *pool;
    DescriptorSetLayout *layout;
    std::vector<VkDescriptorSet> sets;
    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
};

#endif