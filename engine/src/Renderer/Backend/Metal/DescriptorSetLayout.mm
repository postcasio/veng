#include <Veng/Renderer/DescriptorSetLayout.h>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

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
            VE_ASSERT(binding.ImmutableSamplers.empty() ||
                          binding.ImmutableSamplers.size() == binding.Count,
                      "DescriptorSetLayout '{}': binding {} has {} immutable samplers but Count {}",
                      m_Name, binding.Binding, binding.ImmutableSamplers.size(), binding.Count);
        }
        // The placement the cook gives a shader declaring this set, element for element.
        m_Native->Placement = Metal::PlaceSet(m_Name, m_Bindings);
    }

    DescriptorSetLayout::~DescriptorSetLayout() = default;

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
