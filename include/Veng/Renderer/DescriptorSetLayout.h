#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    struct DescriptorBinding
    {
        u32 Binding = 0;
        DescriptorType Type{};
        u32 Count = 1;
        ShaderStage Stages{};
    };

    struct DescriptorSetLayoutInfo
    {
        string Name;
        vector<DescriptorBinding> Bindings;
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

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const vector<DescriptorBinding>& GetBindings() const { return m_Bindings; }

        // Look up a binding by its binding *number* (sparse-safe — works for
        // layouts like 0, 2, 5). Both are fatal if the binding does not exist.
        [[nodiscard]] DescriptorType GetBindingType(u32 binding) const;
        [[nodiscard]] u32 GetBindingCount(u32 binding) const; // descriptor count (array size)

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        vector<DescriptorBinding> m_Bindings;
        map<u32, DescriptorBinding> m_BindingsByNumber;
        Unique<Native> m_Native;
    };
}
