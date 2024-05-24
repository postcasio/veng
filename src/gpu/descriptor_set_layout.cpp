#include "descriptor_set_layout.h"
#include "../engine.h"

DescriptorSetLayout::DescriptorSetLayout(LogicalDevice &device, std::vector<VkDescriptorSetLayoutBinding> bindings) : device(device)
{

    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorSetLayoutCreateInfo.pBindings = bindings.data();

    VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device.device, &descriptorSetLayoutCreateInfo, nullptr, &layout), "failed to create descriptor set layout!");
}

DescriptorSetLayout::DescriptorSetLayout(LogicalDevice &device, std::vector<VkDescriptorSetLayoutBinding> bindings, VkDescriptorBindingFlags bindingFlags) : device(device)
{

    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorSetLayoutCreateInfo.pBindings = bindings.data();
    descriptorSetLayoutCreateInfo.pNext = &bindingFlags;

    VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device.device, &descriptorSetLayoutCreateInfo, nullptr, &layout), "failed to create descriptor set layout!");
}

DescriptorSetLayout::~DescriptorSetLayout()
{
    device.destroyDescriptorSetLayout(layout);
}