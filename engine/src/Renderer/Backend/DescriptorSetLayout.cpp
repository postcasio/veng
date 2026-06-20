#include <Veng/Renderer/DescriptorSetLayout.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

namespace Veng::Renderer
{
    DescriptorSetLayout::Native& DescriptorSetLayout::GetNative() const
    {
        return *m_Native;
    }

    DescriptorSetLayout::DescriptorSetLayout(Context& context, const DescriptorSetLayoutInfo& info)
        : m_Context(context), m_Name(info.Name), m_Bindings(info.Bindings),
          m_Native(CreateUnique<Native>())
    {
        for (const auto& binding : m_Bindings)
        {
            m_BindingsByNumber.emplace(binding.Binding, binding);
        }

        vector<vk::DescriptorSetLayoutBinding> vkBindings;
        vkBindings.reserve(m_Bindings.size());

        // The vk::Sampler arrays a binding's pImmutableSamplers points at must
        // outlive createDescriptorSetLayout, so they are held here for the call.
        vector<vector<vk::Sampler>> immutableSamplers;
        immutableSamplers.reserve(m_Bindings.size());

        for (const auto& binding : m_Bindings)
        {
            const vk::Sampler* immutable = nullptr;
            if (!binding.ImmutableSamplers.empty())
            {
                VE_ASSERT(
                    binding.ImmutableSamplers.size() == binding.Count,
                    "DescriptorSetLayout '{}': binding {} has {} immutable samplers but Count {}",
                    m_Name, binding.Binding, binding.ImmutableSamplers.size(), binding.Count);

                vector<vk::Sampler>& samplers = immutableSamplers.emplace_back();
                samplers.reserve(binding.ImmutableSamplers.size());
                for (const Ref<Sampler>& sampler : binding.ImmutableSamplers)
                    samplers.push_back(sampler->GetNative().Sampler);
                immutable = samplers.data();
            }
            else
            {
                immutableSamplers.emplace_back();
            }

            vkBindings.push_back({
                .binding = binding.Binding,
                .descriptorType = ToVk(binding.Type),
                .descriptorCount = binding.Count,
                .stageFlags = ToVk(binding.Stages),
                .pImmutableSamplers = immutable,
            });
        }

        // Non-bindless bindings need none of UpdateAfterBind/partiallyBound;
        // those require descriptor-indexing device features (see GetDescriptorTypeInfo)
        // and are only set for bindings that explicitly opt in via Bindless.
        vector<vk::DescriptorBindingFlags> bindingFlags;
        bindingFlags.reserve(m_Bindings.size());

        bool anyBindless = false;
        for (const auto& binding : m_Bindings)
        {
            if (binding.Bindless)
            {
                VE_ASSERT(
                    GetDescriptorTypeInfo(binding.Type).SupportsBindless,
                    "DescriptorSetLayout '{}': binding {} requests Bindless, but DescriptorType {} "
                    "does not support UpdateAfterBind on this device",
                    m_Name, binding.Binding, static_cast<u32>(binding.Type));

                bindingFlags.push_back(vk::DescriptorBindingFlagBits::ePartiallyBound |
                                       vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                                       vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending);
                anyBindless = true;
            }
            else
            {
                bindingFlags.push_back({});
            }
        }

        const vk::DescriptorSetLayoutBindingFlagsCreateInfo
            descriptorSetLayoutBindingFlagsCreateInfo{
                .bindingCount = static_cast<u32>(vkBindings.size()),
                .pBindingFlags = bindingFlags.data(),
            };

        const vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{
            .pNext = &descriptorSetLayoutBindingFlagsCreateInfo,
            .flags = anyBindless ? vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool
                                 : vk::DescriptorSetLayoutCreateFlags{},
            .bindingCount = static_cast<u32>(vkBindings.size()),
            .pBindings = vkBindings.data(),
        };

        m_Native->Layout =
            GetVkDevice(m_Context).createDescriptorSetLayout(descriptorSetLayoutCreateInfo).value;

        DebugMarkers::MarkDescriptorSetLayout(GetVkDevice(m_Context), m_Native->Layout, m_Name);
    }

    DescriptorSetLayout::~DescriptorSetLayout()
    {
        GetVkDevice(m_Context).destroyDescriptorSetLayout(m_Native->Layout);
    }

    DescriptorType DescriptorSetLayout::GetBindingType(u32 binding) const
    {
        const auto it = m_BindingsByNumber.find(binding);
        VE_ASSERT(it != m_BindingsByNumber.end(), "DescriptorSetLayout '{}' has no binding {}",
                  m_Name, binding);
        return it->second.Type;
    }

    u32 DescriptorSetLayout::GetBindingCount(u32 binding) const
    {
        const auto it = m_BindingsByNumber.find(binding);
        VE_ASSERT(it != m_BindingsByNumber.end(), "DescriptorSetLayout '{}' has no binding {}",
                  m_Name, binding);
        return it->second.Count;
    }
}
