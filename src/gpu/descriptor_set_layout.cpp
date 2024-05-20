#include "descriptor_set_layout.h"
#include "../engine.h"

DescriptorSetLayout::DescriptorSetLayout(std::vector<VkDescriptorSetLayoutBinding> bindings)
{
    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorSetLayoutCreateInfo.pBindings = bindings.data();

    VK_CHECK_RESULT(vkCreateDescriptorSetLayout(renderer()->device, &descriptorSetLayoutCreateInfo, nullptr, &layout), "failed to create descriptor set layout!");
}

DescriptorSetLayout::~DescriptorSetLayout()
{
    vkDestroyDescriptorSetLayout(renderer()->device, layout, nullptr);
}