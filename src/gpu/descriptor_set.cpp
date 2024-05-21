#include "descriptor_set.h"
#include "descriptor_set_layout.h"
#include "../engine.h"

DescriptorSet::DescriptorSet(DescriptorSetLayout &layout, DescriptorPool &pool) : pool(pool), layout(layout)
{
    auto imageCount = MAX_FRAMES_IN_FLIGHT;

    std::vector<VkDescriptorSetLayout> layouts(imageCount, layout.layout);

    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.descriptorPool = pool.pool;
    descriptorSetAllocateInfo.descriptorSetCount = static_cast<uint32_t>(imageCount);
    descriptorSetAllocateInfo.pSetLayouts = layouts.data();

    sets.resize(imageCount);

    VK_CHECK_RESULT(vkAllocateDescriptorSets(pool.device.device, &descriptorSetAllocateInfo, sets.data()), "failed to allocate descriptor sets!");
}

DescriptorSet::~DescriptorSet()
{
    pool.destroyDescriptorSets(sets);
}