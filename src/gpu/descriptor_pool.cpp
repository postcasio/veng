#include "descriptor_pool.h"
#include "../engine.h"

DescriptorPool::DescriptorPool(uint32_t uniformBufferCount, uint32_t imageSamplerCount, uint32_t descriptorSetCount) : uniformBufferCount(uniformBufferCount), imageSamplerCount(imageSamplerCount), descriptorSetCount(descriptorSetCount)
{
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = uniformBufferCount;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = imageSamplerCount;

    descriptorPoolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolCreateInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    descriptorPoolCreateInfo.pPoolSizes = poolSizes.data();
    descriptorPoolCreateInfo.maxSets = descriptorSetCount;
    descriptorPoolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    VK_CHECK_RESULT(vkCreateDescriptorPool(renderer()->device, &descriptorPoolCreateInfo, nullptr, &pool), "failed to create descriptor pool!");
}

DescriptorPool::~DescriptorPool()
{
    sets.clear();

    vkDestroyDescriptorPool(renderer()->device, pool, nullptr);
}

DescriptorSet *DescriptorPool::createDescriptorSet(DescriptorSetLayout &layout)
{
    sets.push_back(std::make_unique<DescriptorSet>(layout, *this));

    return sets.back().get();
}

void DescriptorPool::freeDescriptorSet(DescriptorSet *set)
{
    auto it = std::find_if(sets.begin(), sets.end(), [set](const std::unique_ptr<DescriptorSet> &s)
                           { return s.get() == set; });

    if (it != sets.end())
    {
        sets.erase(it);
    }
}