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

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const vector<vk::DescriptorSetLayoutBinding>& GetBindings() const { return m_Bindings; }

        // Look up a binding by its binding *number* (sparse-safe — works for
        // layouts like 0, 2, 5). Both are fatal if the binding does not exist.
        [[nodiscard]] vk::DescriptorType GetBindingType(u32 binding) const;
        [[nodiscard]] u32 GetBindingCount(u32 binding) const; // descriptor count (array size)

    private:
        string m_Name;
        vector<vk::DescriptorSetLayoutBinding> m_Bindings;
        map<u32, vk::DescriptorSetLayoutBinding> m_BindingsByNumber;
        vk::DescriptorSetLayout m_VkDescriptorSetLayout;
    };
}
