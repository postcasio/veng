#ifndef _DESCRIPTOR_SET_LAYOUT_H_
#define _DESCRIPTOR_SET_LAYOUT_H_

#include "../gfxcommon.h"
#include "logical_device.h"

class DescriptorSetLayout
{
public:
    DescriptorSetLayout(LogicalDevice &device, std::vector<VkDescriptorSetLayoutBinding> bindings);
    ~DescriptorSetLayout();

    VkDescriptorSetLayout layout;
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{};

    LogicalDevice &device;
};

#endif