#include <Veng/Renderer/DescriptorSet.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>


namespace Veng::Renderer
{
    namespace
    {
        const char* TypeName(DescriptorType type)
        {
            return string_VkDescriptorType(static_cast<VkDescriptorType>(ToVk(type)));
        }
    }

    DescriptorSet::Native& DescriptorSet::GetNative() const { return *m_Native; }

    DescriptorSet::DescriptorSet(Context& context, const DescriptorSetInfo& info) : m_Context(context), m_Name(info.Name), m_Native(CreateUnique<Native>()),
                                                                   m_Layout(info.Layout)
    {
        vector<vk::DescriptorSetLayout> layouts = {info.Layout->GetNative().Layout};

        const vk::DescriptorSetAllocateInfo allocateInfo = {
            .descriptorPool = context.GetNative().DescriptorPool->GetVkDescriptorPool(),
            .descriptorSetCount = static_cast<u32>(layouts.size()),
            .pSetLayouts = layouts.data()
        };

        m_Native->Set = GetVkDevice(m_Context).allocateDescriptorSets(allocateInfo).value[0];

        DebugMarkers::MarkDescriptorSet(m_Native->Set, m_Name);
    }

    DescriptorSet::~DescriptorSet()
    {
        m_Context.GetNative().Retire(m_Native->Set);
    }

    void DescriptorSet::Write(u32 binding, const Ref<ImageView>& view, const Ref<Sampler>& sampler)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::CombinedImageSampler,
                  "DescriptorSet '{}': binding {} is {}, not a combined image sampler",
                  m_Name, binding, TypeName(type));

        const vk::DescriptorImageInfo imageInfo{
            .sampler = sampler->GetNative().Sampler,
            .imageView = view->GetNative().ImageView,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = m_Native->Set,
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = ToVk(type),
            .pImageInfo = &imageInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});

        m_BoundPerBinding[binding] = {
            std::static_pointer_cast<void>(view),
            std::static_pointer_cast<void>(sampler),
        };
    }

    void DescriptorSet::Write(u32 binding, const Ref<ImageView>& view)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::SampledImage || type == DescriptorType::StorageImage,
                  "DescriptorSet '{}': binding {} is {}, not a sampled or storage image",
                  m_Name, binding, TypeName(type));

        const vk::ImageLayout imageLayout = type == DescriptorType::StorageImage
                                                ? vk::ImageLayout::eGeneral
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;

        const vk::DescriptorImageInfo imageInfo{
            .imageView = view->GetNative().ImageView,
            .imageLayout = imageLayout,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = m_Native->Set,
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = ToVk(type),
            .pImageInfo = &imageInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});

        m_BoundPerBinding[binding] = {std::static_pointer_cast<void>(view)};
    }

    void DescriptorSet::Write(u32 binding, const Ref<Buffer>& buffer)
    {
        Write(binding, buffer, 0, VK_WHOLE_SIZE);
    }

    void DescriptorSet::Write(u32 binding, const Ref<Buffer>& buffer, u64 offset, u64 range)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::UniformBuffer || type == DescriptorType::StorageBuffer,
                  "DescriptorSet '{}': binding {} is {}, not a uniform or storage buffer",
                  m_Name, binding, TypeName(type));

        const vk::DescriptorBufferInfo bufferInfo{
            .buffer = buffer->GetNative().Buffer,
            .offset = offset,
            .range = range,
        };

        const vk::WriteDescriptorSet write{
            .dstSet = m_Native->Set,
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = ToVk(type),
            .pBufferInfo = &bufferInfo,
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});

        m_BoundPerBinding[binding] = {std::static_pointer_cast<void>(buffer)};
    }

    void DescriptorSet::WriteArray(u32 binding, std::span<const Ref<ImageView>> views,
                                   const Ref<Sampler>& sampler, u32 firstElement)
    {
        const DescriptorType type = m_Layout->GetBindingType(binding);
        VE_ASSERT(type == DescriptorType::CombinedImageSampler,
                  "DescriptorSet '{}': binding {} is {}, not a combined image sampler",
                  m_Name, binding, TypeName(type));

        vector<vk::DescriptorImageInfo> imageInfos;
        imageInfos.reserve(views.size());

        vector<Ref<void>> owned;
        owned.reserve(views.size() + 1);
        owned.push_back(std::static_pointer_cast<void>(sampler));

        for (const auto& view : views)
        {
            imageInfos.push_back({
                .sampler = sampler->GetNative().Sampler,
                .imageView = view->GetNative().ImageView,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            });
            owned.push_back(std::static_pointer_cast<void>(view));
        }

        const vk::WriteDescriptorSet write{
            .dstSet = m_Native->Set,
            .dstBinding = binding,
            .dstArrayElement = firstElement,
            .descriptorCount = static_cast<u32>(imageInfos.size()),
            .descriptorType = ToVk(type),
            .pImageInfo = imageInfos.data(),
        };

        GetVkDevice(m_Context).updateDescriptorSets(write, {});

        m_BoundPerBinding[binding] = std::move(owned);
    }
}
