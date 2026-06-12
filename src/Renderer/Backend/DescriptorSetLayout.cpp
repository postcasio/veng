#include <Veng/Renderer/Backend/DescriptorSetLayout.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    DescriptorSetLayout::DescriptorSetLayout(const DescriptorSetLayoutInfo& info) : m_Name(info.Name),
        m_Bindings(info.Bindings)
    {

        vector<vk::DescriptorBindingFlags> bindingFlags(m_Bindings.size());

        bindingFlags.assign(m_Bindings.size(),
                            vk::DescriptorBindingFlagBits::ePartiallyBound |
                            vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                            vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending
        );

        const vk::DescriptorSetLayoutBindingFlagsCreateInfo descriptorSetLayoutBindingFlagsCreateInfo{
            .bindingCount = static_cast<u32>(m_Bindings.size()),
            .pBindingFlags = bindingFlags.data(),
        };

        const vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{
            .pNext = &descriptorSetLayoutBindingFlagsCreateInfo,
            .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
            .bindingCount = static_cast<u32>(m_Bindings.size()),
            .pBindings = m_Bindings.data(),
        };

        m_VkDescriptorSetLayout = Context::Instance().GetVkDevice().createDescriptorSetLayout(
            descriptorSetLayoutCreateInfo);

        DebugMarkers::MarkDescriptorSetLayout(m_VkDescriptorSetLayout, m_Name);
    }

    DescriptorSetLayout::~DescriptorSetLayout()
    {
        Context::Instance().GetVkDevice().destroyDescriptorSetLayout(m_VkDescriptorSetLayout);
    }
}
