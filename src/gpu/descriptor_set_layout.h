#ifndef _DESCRIPTOR_SET_LAYOUT_H_
#define _DESCRIPTOR_SET_LAYOUT_H_

#include "../gfxcommon.h"

class DescriptorSetLayout
{
public:
    DescriptorSetLayout(std::vector<VkDescriptorSetLayoutBinding> bindings);
    ~DescriptorSetLayout();

    VkDescriptorSetLayout layout;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{};
};

#endif