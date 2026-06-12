#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct DescriptorSetLayoutInfo
    {
        string Name;
        vector<vk::DescriptorSetLayoutBinding> Bindings;
    };

    class DescriptorSetLayout
    {
    public:
        static Ref<DescriptorSetLayout> Create(const DescriptorSetLayoutInfo& info)
        {
            return CreateRef<DescriptorSetLayout>(info);
        }

        explicit DescriptorSetLayout(const DescriptorSetLayoutInfo& info);
        ~DescriptorSetLayout();

        [[nodiscard]] const vk::DescriptorSetLayout& GetVkDescriptorSetLayout() const
        {
            return m_VkDescriptorSetLayout;
        }

        [[nodiscard]] string GetName() const { return m_Name; }
        [[nodiscard]] const vector<vk::DescriptorSetLayoutBinding>& GetBindings() const { return m_Bindings; }
        [[nodiscard]] u32 GetBindingCount() const { return static_cast<u32>(m_Bindings.size()); }

    private:
        string m_Name;
        vector<vk::DescriptorSetLayoutBinding> m_Bindings;
        vk::DescriptorSetLayout m_VkDescriptorSetLayout;
    };
}
