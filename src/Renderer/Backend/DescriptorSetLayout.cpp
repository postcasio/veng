#include <Veng/Renderer/Backend/DescriptorSetLayout.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    DescriptorSetLayout::DescriptorSetLayout(const DescriptorSetLayoutInfo& info) : m_Name(info.Name),
        m_Bindings(info.Bindings)
    {
        for (const auto& binding : m_Bindings)
        {
            m_BindingsByNumber.emplace(binding.Binding, binding);
        }

        vector<vk::DescriptorSetLayoutBinding> vkBindings;
        vkBindings.reserve(m_Bindings.size());

        for (const auto& binding : m_Bindings)
        {
            vkBindings.push_back({
                .binding = binding.Binding,
                .descriptorType = ToVk(binding.Type),
                .descriptorCount = binding.Count,
                .stageFlags = ToVk(binding.Stages),
            });
        }

        vector<vk::DescriptorBindingFlags> bindingFlags(vkBindings.size());

        bindingFlags.assign(vkBindings.size(),
                            vk::DescriptorBindingFlagBits::ePartiallyBound |
                            vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                            vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending
        );

        const vk::DescriptorSetLayoutBindingFlagsCreateInfo descriptorSetLayoutBindingFlagsCreateInfo{
            .bindingCount = static_cast<u32>(vkBindings.size()),
            .pBindingFlags = bindingFlags.data(),
        };

        const vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{
            .pNext = &descriptorSetLayoutBindingFlagsCreateInfo,
            .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
            .bindingCount = static_cast<u32>(vkBindings.size()),
            .pBindings = vkBindings.data(),
        };

        m_VkDescriptorSetLayout = Context::Instance().GetVkDevice().createDescriptorSetLayout(
            descriptorSetLayoutCreateInfo).value;

        DebugMarkers::MarkDescriptorSetLayout(m_VkDescriptorSetLayout, m_Name);
    }

    DescriptorSetLayout::~DescriptorSetLayout()
    {
        Context::Instance().GetVkDevice().destroyDescriptorSetLayout(m_VkDescriptorSetLayout);
    }

    DescriptorType DescriptorSetLayout::GetBindingType(u32 binding) const
    {
        const auto it = m_BindingsByNumber.find(binding);
        VE_ASSERT(it != m_BindingsByNumber.end(),
                  "DescriptorSetLayout '{}' has no binding {}", m_Name, binding);
        return it->second.Type;
    }

    u32 DescriptorSetLayout::GetBindingCount(u32 binding) const
    {
        const auto it = m_BindingsByNumber.find(binding);
        VE_ASSERT(it != m_BindingsByNumber.end(),
                  "DescriptorSetLayout '{}' has no binding {}", m_Name, binding);
        return it->second.Count;
    }
}
