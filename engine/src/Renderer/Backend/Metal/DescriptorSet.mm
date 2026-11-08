#include <Veng/Renderer/DescriptorSet.h>

#include <cstring>

#include <Veng/Renderer/Context.h>

#include "Natives.h"

namespace Veng::Renderer
{
    namespace
    {
        const char* TypeName(const DescriptorType type)
        {
            switch (type)
            {
            case DescriptorType::CombinedImageSampler:
                return "a combined image sampler";
            case DescriptorType::SampledImage:
                return "a sampled image";
            case DescriptorType::StorageImage:
                return "a storage image";
            case DescriptorType::UniformBuffer:
                return "a uniform buffer";
            case DescriptorType::StorageBuffer:
                return "a storage buffer";
            case DescriptorType::Sampler:
                return "a sampler";
            case DescriptorType::UniformBufferDynamic:
                return "a dynamic uniform buffer";
            }
            return "an unknown descriptor";
        }

        u64 ResourceIdOf(const id<MTLTexture> texture)
        {
            const MTLResourceID resource = [texture gpuResourceID];
            u64 value = 0;
            std::memcpy(&value, &resource, sizeof(value));
            return value;
        }

        u64 ResourceIdOf(const id<MTLSamplerState> sampler)
        {
            const MTLResourceID resource = [sampler gpuResourceID];
            u64 value = 0;
            std::memcpy(&value, &resource, sizeof(value));
            return value;
        }
    }

    DescriptorSet::Native& DescriptorSet::GetNative() const
    {
        return *m_Native;
    }

    DescriptorSet::DescriptorSet(Context& context, const DescriptorSetInfo& info)
        : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()),
          m_Layout(info.Layout)
    {
        const Metal::SetPlacement& placement = m_Layout->GetNative().Placement;
        const u64 bytes = static_cast<u64>(std::max(placement.ElementCount, 1u)) *
                          Cook::MetalBindingSlots::ArgumentBufferElementBytes;
        m_Native->Arguments = Buffer::Create(context, {
                                                          .Name = m_Name + " (Arguments)",
                                                          .Size = bytes,
                                                          .Usage = BufferUsage::Storage,
                                                          .HostMapped = true,
                                                      });
        // Placement-heap memory is not cleared, and an unwritten element must read as no resource.
        std::memset(m_Native->Arguments->GetMappedData(), 0, bytes);
        m_Native->DynamicAddresses.assign(placement.DynamicCount, 0);

        for (const DescriptorBinding& binding : m_Layout->GetBindings())
        {
            if (binding.ImmutableSamplers.empty())
            {
                continue;
            }
            const Metal::BindingPlacement* const entry = placement.Find(binding.Binding);
            const u32 stride = Metal::ElementsPerDescriptor(binding.Type);
            // A combined image sampler's sampler is its second element.
            const u32 samplerElement = binding.Type == DescriptorType::CombinedImageSampler ? 1 : 0;
            for (u32 i = 0; i < binding.Count; ++i)
            {
                m_Native->WriteElement(
                    entry->FirstElement + i * stride + samplerElement,
                    ResourceIdOf(binding.ImmutableSamplers[i]->GetNative().Sampler));
            }
            vector<Ref<void>>& owned = m_BoundPerBinding[binding.Binding];
            for (const Ref<Sampler>& sampler : binding.ImmutableSamplers)
            {
                owned.push_back(std::static_pointer_cast<void>(sampler));
            }
        }
    }

    DescriptorSet::~DescriptorSet() = default;

    void DescriptorSet::Write(u32 binding, const Ref<ImageView>& view, const Ref<Sampler>& sampler)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::CombinedImageSampler,
                  "DescriptorSet '{}': binding {} is {}, not a combined image sampler", m_Name,
                  binding, TypeName(type));
        const u32 first = m_Layout->GetNative().Placement.Find(binding)->FirstElement;
        m_Native->WriteElement(first, ResourceIdOf(view->GetNative().Texture));
        m_Native->WriteElement(first + 1, ResourceIdOf(sampler->GetNative().Sampler));
        m_BoundPerBinding[binding] = {
            std::static_pointer_cast<void>(view),
            std::static_pointer_cast<void>(sampler),
        };
    }

    void DescriptorSet::Write(u32 binding, const Ref<ImageView>& view)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::SampledImage || type == DescriptorType::StorageImage,
                  "DescriptorSet '{}': binding {} is {}, not a sampled or storage image", m_Name,
                  binding, TypeName(type));
        const u32 first = m_Layout->GetNative().Placement.Find(binding)->FirstElement;
        m_Native->WriteElement(first, ResourceIdOf(view->GetNative().Texture));
        m_BoundPerBinding[binding] = {std::static_pointer_cast<void>(view)};
    }

    void DescriptorSet::Write(u32 binding, const Ref<Sampler>& sampler)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::Sampler,
                  "DescriptorSet '{}': binding {} is {}, not a plain sampler", m_Name, binding,
                  TypeName(type));
        const u32 first = m_Layout->GetNative().Placement.Find(binding)->FirstElement;
        m_Native->WriteElement(first, ResourceIdOf(sampler->GetNative().Sampler));
        m_BoundPerBinding[binding] = {std::static_pointer_cast<void>(sampler)};
    }

    void DescriptorSet::Write(u32 binding, const Ref<Buffer>& buffer)
    {
        Write(binding, buffer, 0, buffer->GetSize());
    }

    void DescriptorSet::Write(u32 binding, const Ref<Buffer>& buffer, u64 offset, u64 /*range*/)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::UniformBuffer || type == DescriptorType::StorageBuffer ||
                      type == DescriptorType::UniformBufferDynamic,
                  "DescriptorSet '{}': binding {} is {}, not a uniform, storage, or "
                  "dynamic-uniform buffer",
                  m_Name, binding, TypeName(type));
        const Metal::BindingPlacement& entry = *m_Layout->GetNative().Placement.Find(binding);
        const u64 address = [buffer->GetNative().Buffer gpuAddress] + offset;
        if (type == DescriptorType::UniformBufferDynamic)
        {
            // Bound by address at its own slot when the set is bound, plus the dynamic offset.
            m_Native->DynamicAddresses[entry.DynamicIndex] = address;
        }
        else
        {
            m_Native->WriteElement(entry.FirstElement, address);
        }
        m_BoundPerBinding[binding] = {std::static_pointer_cast<void>(buffer)};
    }

    void DescriptorSet::WriteArray(u32 binding, std::span<const Ref<ImageView>> views,
                                   const Ref<Sampler>& sampler, u32 firstElement)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::CombinedImageSampler,
                  "DescriptorSet '{}': binding {} is {}, not a combined image sampler", m_Name,
                  binding, TypeName(type));
        const Metal::BindingPlacement& entry = *m_Layout->GetNative().Placement.Find(binding);
        VE_ASSERT(firstElement + views.size() <= entry.Count,
                  "DescriptorSet '{}': elements {}+{} run past binding {}'s {}", m_Name,
                  firstElement, views.size(), binding, entry.Count);

        vector<Ref<void>> owned;
        owned.reserve(views.size() + 1);
        owned.push_back(std::static_pointer_cast<void>(sampler));
        const u64 samplerId = ResourceIdOf(sampler->GetNative().Sampler);
        for (usize i = 0; i < views.size(); ++i)
        {
            const u32 element = entry.FirstElement + (firstElement + static_cast<u32>(i)) * 2;
            m_Native->WriteElement(element, ResourceIdOf(views[i]->GetNative().Texture));
            m_Native->WriteElement(element + 1, samplerId);
            owned.push_back(std::static_pointer_cast<void>(views[i]));
        }
        m_BoundPerBinding[binding] = std::move(owned);
    }
}
